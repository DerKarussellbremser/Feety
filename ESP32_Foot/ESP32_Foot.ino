/*
  Feety - foot unit (sensor side)

  Reads the pressure sensors in the prosthesis sole and streams them to the hip
  unit over ESP-NOW at 50 Hz.

  Design notes:
    - Nothing in loop() blocks. delay() is only used in one place: while waiting
      for the button to be released just before entering deep sleep.
    - Raw 12 bit ADC values go over the air. Converting to volts here and back
      to a PWM duty on the hip only threw away resolution; the hip now maps raw
      readings straight to motor levels using its own per-channel calibration.
    - The packet carries a length and a version, so a half-received or
      mismatched frame is dropped instead of being parsed into wrong values.
      The old comma-separated text format silently lost the last sensor,
      because the sender terminated it with '\n' while the receiver looked for
      a ',' on every field - that is what the fifth dummy pin was working
      around.

  See README.md for wiring and first-run setup.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/rtc_io.h>

#include "feety_config.h"
#include "feety_protocol.h"

#if FEETY_DEBUG
#define LOG(...)   Serial.printf(__VA_ARGS__)
#else
#define LOG(...)   do {} while (0)
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static feety_sensor_packet_t txPacket;

static uint32_t lastSendMs = 0;
static uint32_t lastBattMs = 0;
static volatile uint32_t lastHipContactMs = 0;   // written by the ESP-NOW callback
static uint32_t buttonDownMs = 0;
static bool     buttonWasDown = false;
static volatile bool sleepRequested = false;   // set from the ESP-NOW callback

static uint16_t vbatMv = 4200;

// LED blink pattern, driven from millis() instead of delay()
static uint32_t ledNextToggleMs = 0;
static bool     ledState = false;

// ---------------------------------------------------------------------------
// Deep sleep
// ---------------------------------------------------------------------------
static void enterDeepSleep(const char *reason) {
  LOG("Sleeping: %s\n", reason);
  digitalWrite(FEETY_LED_PIN, LOW);

  // Wait for the button to be released, otherwise ext0 fires immediately and
  // the unit wakes right back up.
  while (digitalRead(FEETY_BUTTON_PIN) == HIGH) {
    delay(10);
  }
  delay(50);   // debounce the release

  // The internal pulldown is switched off in deep sleep unless it is enabled
  // on the RTC side. Without this the wake pin floats and wakes the board at
  // random - one of the reasons the old build seemed to drain its cell.
  rtc_gpio_pulldown_en((gpio_num_t)FEETY_BUTTON_PIN);
  rtc_gpio_pullup_dis((gpio_num_t)FEETY_BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)FEETY_BUTTON_PIN, HIGH);

  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// ESP-NOW
// ---------------------------------------------------------------------------
static void handleCommand(const uint8_t *data, int len) {
  if (len != (int)sizeof(feety_cmd_packet_t)) {
    return;
  }
  feety_cmd_packet_t cmd;
  memcpy(&cmd, data, sizeof(cmd));
  if (cmd.version != FEETY_PROTOCOL_VERSION) {
    return;
  }

  lastHipContactMs = millis();

  if (cmd.command == FEETY_CMD_SLEEP) {
    sleepRequested = true;   // acted on in loop(), not inside the callback
  }
}

// The receive callback signature changed with Arduino core 3.x.
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  handleCommand(data, len);
}
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  handleCommand(data, len);
}
#endif

static bool radioBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);

  // Pin the channel. Both units must agree; without this they can end up on
  // different channels and never hear each other.
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(FEETY_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  // No WiFi power save: modem sleep adds tens of milliseconds of latency,
  // which is the one thing a step feedback system cannot afford.
  esp_wifi_set_ps(WIFI_PS_NONE);

  LOG("Foot unit MAC: %s\n", WiFi.macAddress().c_str());

  if (esp_now_init() != ESP_OK) {
    return false;
  }
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, FEETY_PEER_MAC, 6);
  peer.channel = FEETY_WIFI_CHANNEL;
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    return false;
  }

  const uint8_t broadcast[6] = FEETY_BROADCAST_MAC;
  if (memcmp(FEETY_PEER_MAC, broadcast, 6) == 0) {
    LOG("WARNING: peer is the broadcast address. Set FEETY_PEER_MAC to the "
        "hip unit's MAC in feety_config.h.\n");
  }
  return true;
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
static void readSensors() {
  for (int i = 0; i < FEETY_SENSOR_COUNT; i++) {
    uint32_t sum = 0;
    for (int n = 0; n < FEETY_OVERSAMPLE; n++) {
      sum += analogRead(FEETY_SENSOR_PINS[i]);
    }
    txPacket.raw[i] = (uint16_t)(sum / FEETY_OVERSAMPLE);
  }
}

static void readBattery() {
  // analogReadMilliVolts() applies the chip's factory ADC calibration, which
  // is far more accurate than scaling the raw count by a nominal 3.3 V.
  vbatMv = (uint16_t)(analogReadMilliVolts(FEETY_VBAT_PIN) * FEETY_VBAT_DIVIDER);

  if (vbatMv <= FEETY_VBAT_CRITICAL_MV) {
    enterDeepSleep("battery critical");
  }
}

// ---------------------------------------------------------------------------
// Button and LED
// ---------------------------------------------------------------------------
static void handleButton() {
  bool down = (digitalRead(FEETY_BUTTON_PIN) == HIGH);
  uint32_t now = millis();

  if (down && !buttonWasDown) {
    buttonDownMs = now;
  } else if (down && (now - buttonDownMs >= FEETY_SLEEP_PRESS_MS)) {
    enterDeepSleep("button held");
  }
  buttonWasDown = down;
}

static void updateLed(bool linkUp) {
  uint32_t now = millis();
  uint32_t period;

  if (vbatMv <= FEETY_VBAT_LOW_MV) {
    period = 150;          // battery low: fast blink
  } else if (!linkUp) {
    period = 700;          // no hip in range: slow blink
  } else {
    if (!ledState) {       // linked: steady on
      ledState = true;
      digitalWrite(FEETY_LED_PIN, HIGH);
    }
    return;
  }

  if (now >= ledNextToggleMs) {
    ledNextToggleMs = now + period;
    ledState = !ledState;
    digitalWrite(FEETY_LED_PIN, ledState ? HIGH : LOW);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  pinMode(FEETY_LED_PIN, OUTPUT);
  digitalWrite(FEETY_LED_PIN, HIGH);
  pinMode(FEETY_BUTTON_PIN, INPUT_PULLDOWN);

  // Sensor pins are left in their default state on purpose. The old sketch set
  // INPUT_PULLDOWN on them, which loads the sensor divider and shifts every
  // reading downwards.
  analogReadResolution(12);
  for (int i = 0; i < FEETY_SENSOR_COUNT; i++) {
    analogSetPinAttenuation(FEETY_SENSOR_PINS[i], ADC_11db);   // full 0-3.3 V
  }
  analogSetPinAttenuation(FEETY_VBAT_PIN, ADC_11db);

  Serial.begin(115200);
  delay(100);
  LOG("\nFeety foot unit, protocol v%d\n", FEETY_PROTOCOL_VERSION);

  if (!radioBegin()) {
    LOG("ESP-NOW init failed, restarting\n");
    delay(1000);
    ESP.restart();
  }

  txPacket.version = FEETY_PROTOCOL_VERSION;
  txPacket.seq     = 0;

  readBattery();
  lastHipContactMs = millis();   // grace period before the idle sleep timer
}

void loop() {
  uint32_t now = millis();

  handleButton();

  if (sleepRequested) {
    enterDeepSleep("hip unit went to sleep");
  }

  if (now - lastBattMs >= FEETY_BATT_INTERVAL_MS) {
    lastBattMs = now;
    readBattery();
  }

  if (now - lastSendMs >= FEETY_SEND_INTERVAL_MS) {
    lastSendMs = now;

    readSensors();
    txPacket.seq++;
    txPacket.vbat_mv = vbatMv;
    txPacket.flags   = (vbatMv <= FEETY_VBAT_LOW_MV) ? FEETY_FLAG_BATT_LOW : 0;

    esp_now_send(FEETY_PEER_MAC, (const uint8_t *)&txPacket, sizeof(txPacket));

#if FEETY_DEBUG
    if ((txPacket.seq % 50) == 0) {   // once a second, not 50 times
      LOG("seq=%u  %u %u %u %u  vbat=%umV\n", txPacket.seq,
          txPacket.raw[0], txPacket.raw[1], txPacket.raw[2], txPacket.raw[3],
          vbatMv);
    }
#endif
  }

  bool linkUp = (now - lastHipContactMs) < FEETY_LINK_TIMEOUT_MS;
  updateLed(linkUp);

  if (now - lastHipContactMs >= FEETY_IDLE_SLEEP_MS) {
    enterDeepSleep("no hip unit in range");
  }
}
