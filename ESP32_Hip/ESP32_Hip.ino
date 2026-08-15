/*
  Feety - hip unit (feedback side)

  Receives the sole's pressure readings over ESP-NOW and drives four vibration
  motors on the hip belt.

  Design notes:
    - loop() never blocks. The button, the motors, the battery check and the
      status LED all run off millis(). In the old sketch a long press blocked
      the loop for three seconds while the motors kept running their last
      value, and the whole button handling sat inside "if (SerialBT.available())"
      so it stopped working the moment the link dropped.
    - Only the newest packet is kept. The receive callback overwrites the
      buffer, so there is no queue that can build up a backlog. The old
      readStringUntil() approach consumed the oldest line in the buffer, so
      whenever the sender outran the receiver the felt latency grew without
      limit.
    - No fresh packet for FEETY_SIGNAL_TIMEOUT_MS means the motors are switched
      off. Vibration must never be driven from stale data - a wearer reading it
      as ground contact is the failure mode that matters here.
    - Calibration is per channel and lives in NVS. Type "help" in the serial
      monitor at 115200 baud.

  See README.md for wiring and first-run setup.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/rtc_io.h>
#include <Preferences.h>

#include "feety_config.h"
#include "feety_protocol.h"

#if FEETY_DEBUG
#define LOG(...)   Serial.printf(__VA_ARGS__)
#else
#define LOG(...)   do {} while (0)
#endif

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------
struct MotorCal {
  uint16_t threshold;    // raw ADC counts below which the motor stays off
  uint16_t saturation;   // raw ADC counts at which it reaches pwmMax
  uint8_t  pwmMin;
  uint8_t  pwmMax;
};

static MotorCal cal[FEETY_MOTOR_COUNT];
static Preferences prefs;

static void calDefaults() {
  for (int i = 0; i < FEETY_MOTOR_COUNT; i++) {
    cal[i].threshold  = FEETY_CAL_THRESHOLD;
    cal[i].saturation = FEETY_CAL_SATURATION;
    cal[i].pwmMin     = FEETY_CAL_PWM_MIN;
    cal[i].pwmMax     = FEETY_CAL_PWM_MAX;
  }
}

static void calLoad() {
  calDefaults();
  prefs.begin("feety", true);
  size_t stored = prefs.getBytesLength("cal");
  if (stored == sizeof(cal)) {
    prefs.getBytes("cal", cal, sizeof(cal));
  }
  prefs.end();
}

static void calSave() {
  prefs.begin("feety", false);
  prefs.putBytes("cal", cal, sizeof(cal));
  prefs.end();
  Serial.println("calibration saved");
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
static volatile feety_sensor_packet_t rxShared;   // written by the callback
static volatile bool     rxValid = false;
static volatile uint32_t rxLastMs = 0;

static feety_sensor_packet_t sensors;   // snapshot used by loop()

static bool     vibrationOn = true;
static uint16_t hipVbatMv   = 4200;
static bool     footBattLow = false;

static uint32_t lastHeartbeatMs = 0;
static uint32_t lastBattMs      = 0;

static uint8_t  lastDuty[FEETY_MOTOR_COUNT]   = { 0 };
static uint32_t kickUntilMs[FEETY_MOTOR_COUNT] = { 0 };
static uint32_t testUntilMs[FEETY_MOTOR_COUNT] = { 0 };

static bool     btnStable = false, btnRaw = false;
static uint32_t btnChangeMs = 0, btnPressMs = 0;
static bool     sleepArmed = false;

static uint32_t ledNextToggleMs = 0;
static bool     ledState = false;

static bool streamRaw = false;   // "raw" console command
static uint32_t lastStreamMs = 0;

// ---------------------------------------------------------------------------
// PWM - the LEDC API changed between Arduino core 2.x and 3.x
// ---------------------------------------------------------------------------
static void pwmInit(uint8_t pin, uint8_t channel) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcAttach(pin, FEETY_PWM_FREQ_HZ, FEETY_PWM_BITS);
#else
  ledcSetup(channel, FEETY_PWM_FREQ_HZ, FEETY_PWM_BITS);
  ledcAttachPin(pin, channel);
#endif
}

static void pwmWrite(uint8_t pin, uint8_t channel, uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)channel;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(channel, duty);
#endif
}

static void motorsOff() {
  for (int i = 0; i < FEETY_MOTOR_COUNT; i++) {
    pwmWrite(FEETY_MOTOR_PINS[i], i, 0);
    lastDuty[i]    = 0;
    kickUntilMs[i] = 0;
    testUntilMs[i] = 0;
  }
}

// ---------------------------------------------------------------------------
// ESP-NOW
// ---------------------------------------------------------------------------
static void sendCommand(uint8_t command) {
  feety_cmd_packet_t cmd;
  cmd.version = FEETY_PROTOCOL_VERSION;
  cmd.command = command;
  esp_now_send(FEETY_PEER_MAC, (const uint8_t *)&cmd, sizeof(cmd));
}

static void handlePacket(const uint8_t *data, int len) {
  if (len != (int)sizeof(feety_sensor_packet_t)) {
    return;   // not ours, or truncated
  }
  feety_sensor_packet_t pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.version != FEETY_PROTOCOL_VERSION) {
    return;   // mismatched firmware on the other side
  }

  portENTER_CRITICAL(&rxMux);
  memcpy((void *)&rxShared, &pkt, sizeof(pkt));
  rxValid  = true;
  rxLastMs = millis();
  portEXIT_CRITICAL(&rxMux);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  handlePacket(data, len);
}
#else
static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  handlePacket(data, len);
}
#endif

static bool radioBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(FEETY_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.printf("Hip unit MAC: %s\n", WiFi.macAddress().c_str());

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
    Serial.println("WARNING: peer is the broadcast address. Set FEETY_PEER_MAC "
                   "to the foot unit's MAC in feety_config.h.");
  }
  return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
static void enterDeepSleep(const char *reason) {
  Serial.printf("Sleeping: %s\n", reason);
  motorsOff();
  sendCommand(FEETY_CMD_SLEEP);   // take the foot unit down with us
  delay(30);                      // let the frame leave before the radio dies
  digitalWrite(FEETY_LED_PIN, LOW);

  while (digitalRead(FEETY_BUTTON_PIN) == HIGH) {
    delay(10);
  }
  delay(50);

  rtc_gpio_pulldown_en((gpio_num_t)FEETY_BUTTON_PIN);
  rtc_gpio_pullup_dis((gpio_num_t)FEETY_BUTTON_PIN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)FEETY_BUTTON_PIN, HIGH);

  Serial.flush();
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// Motor mapping
// ---------------------------------------------------------------------------
static uint8_t mapRawToDuty(uint16_t raw, const MotorCal &c) {
  if (raw < c.threshold) {
    return 0;
  }
  if (c.saturation <= c.threshold || raw >= c.saturation) {
    return c.pwmMax;
  }
  uint32_t span  = (uint32_t)(c.saturation - c.threshold);
  uint32_t range = (uint32_t)(c.pwmMax - c.pwmMin);
  return (uint8_t)(c.pwmMin + ((uint32_t)(raw - c.threshold) * range) / span);
}

static void updateMotors(bool linkOk) {
  uint32_t now = millis();

  for (int i = 0; i < FEETY_MOTOR_COUNT; i++) {
    uint8_t duty = 0;

    if (now < testUntilMs[i]) {
      duty = cal[i].pwmMax;               // "test" console command
    } else if (vibrationOn && linkOk) {
      duty = mapRawToDuty(sensors.raw[i], cal[i]);
    }

    // Break-away pulse when a motor starts from standstill.
    if (duty > 0 && lastDuty[i] == 0) {
      kickUntilMs[i] = now + FEETY_KICK_MS;
    }
    lastDuty[i] = duty;

    uint8_t out = duty;
    if (duty > 0 && now < kickUntilMs[i] && duty < FEETY_KICK_DUTY) {
      out = FEETY_KICK_DUTY;
    }
    pwmWrite(FEETY_MOTOR_PINS[i], i, out);
  }
}

// ---------------------------------------------------------------------------
// Button: short press toggles vibration, long press sends both units to sleep
// ---------------------------------------------------------------------------
static void handleButton() {
  uint32_t now = millis();
  bool raw = (digitalRead(FEETY_BUTTON_PIN) == HIGH);

  if (raw != btnRaw) {
    btnRaw = raw;
    btnChangeMs = now;
  }

  if ((now - btnChangeMs) >= FEETY_DEBOUNCE_MS && btnStable != btnRaw) {
    btnStable = btnRaw;
    if (btnStable) {
      btnPressMs = now;
      sleepArmed = false;
    } else if (!sleepArmed) {
      vibrationOn = !vibrationOn;
      if (!vibrationOn) {
        motorsOff();
      }
      Serial.printf("vibration %s\n", vibrationOn ? "on" : "off");
    }
  }

  if (btnStable && !sleepArmed && (now - btnPressMs) >= FEETY_SLEEP_PRESS_MS) {
    sleepArmed = true;   // do not also toggle vibration on release
    enterDeepSleep("button held");
  }
}

// ---------------------------------------------------------------------------
// Battery and LED
// ---------------------------------------------------------------------------
static void readBattery() {
  hipVbatMv = (uint16_t)(analogReadMilliVolts(FEETY_VBAT_PIN) * FEETY_VBAT_DIVIDER);
  if (hipVbatMv <= FEETY_VBAT_CRITICAL_MV) {
    enterDeepSleep("battery critical");
  }
}

static void updateLed(bool linkOk) {
  uint32_t now = millis();
  uint32_t period;

  if (hipVbatMv <= FEETY_VBAT_LOW_MV || footBattLow) {
    period = 150;            // any battery low: fast blink
  } else if (!linkOk) {
    period = 700;            // no data from the foot: slow blink
  } else if (!vibrationOn) {
    period = 1500;           // muted: occasional flash
  } else {
    if (!ledState) {
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
// Serial console - calibration without reflashing.
// A phone app would later send exactly these commands over the same link.
// ---------------------------------------------------------------------------
static void printCal() {
  Serial.println("ch  pin  threshold  saturation  pwmMin  pwmMax");
  for (int i = 0; i < FEETY_MOTOR_COUNT; i++) {
    Serial.printf("%2d  %3d  %9u  %10u  %6u  %6u\n", i, FEETY_MOTOR_PINS[i],
                  cal[i].threshold, cal[i].saturation, cal[i].pwmMin, cal[i].pwmMax);
  }
}

static void handleConsoleLine(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd) {
    return;
  }

  if (!strcmp(cmd, "help")) {
    Serial.println("help                                  this text");
    Serial.println("show                                  print calibration");
    Serial.println("set <ch> <thr> <sat> <pmin> <pmax>    set one channel");
    Serial.println("save                                  store in NVS");
    Serial.println("reset                                 back to defaults");
    Serial.println("test <ch>                             run one motor for 1 s");
    Serial.println("raw                                   toggle live sensor stream");
    Serial.println("mac                                   print this unit's MAC");
  } else if (!strcmp(cmd, "show")) {
    printCal();
  } else if (!strcmp(cmd, "save")) {
    calSave();
  } else if (!strcmp(cmd, "reset")) {
    calDefaults();
    Serial.println("defaults restored (use 'save' to keep them)");
  } else if (!strcmp(cmd, "raw")) {
    streamRaw = !streamRaw;
    Serial.printf("raw stream %s\n", streamRaw ? "on" : "off");
  } else if (!strcmp(cmd, "mac")) {
    Serial.printf("Hip unit MAC: %s\n", WiFi.macAddress().c_str());
  } else if (!strcmp(cmd, "test")) {
    char *a = strtok(NULL, " \t");
    int ch = a ? atoi(a) : -1;
    if (ch < 0 || ch >= FEETY_MOTOR_COUNT) {
      Serial.println("usage: test <ch>");
    } else {
      testUntilMs[ch] = millis() + 1000;
      Serial.printf("motor %d on pin %d\n", ch, FEETY_MOTOR_PINS[ch]);
    }
  } else if (!strcmp(cmd, "set")) {
    char *a[5];
    for (int i = 0; i < 5; i++) {
      a[i] = strtok(NULL, " \t");
      if (!a[i]) {
        Serial.println("usage: set <ch> <thr> <sat> <pmin> <pmax>");
        return;
      }
    }
    int ch = atoi(a[0]);
    if (ch < 0 || ch >= FEETY_MOTOR_COUNT) {
      Serial.println("channel out of range");
      return;
    }
    cal[ch].threshold  = (uint16_t)constrain(atoi(a[1]), 0, 4095);
    cal[ch].saturation = (uint16_t)constrain(atoi(a[2]), 0, 4095);
    cal[ch].pwmMin     = (uint8_t)constrain(atoi(a[3]), 0, 255);
    cal[ch].pwmMax     = (uint8_t)constrain(atoi(a[4]), 0, 255);
    printCal();
  } else {
    Serial.println("unknown command, try 'help'");
  }
}

static void pollConsole() {
  static char buf[80];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      buf[len] = '\0';
      if (len) {
        handleConsoleLine(buf);
      }
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ---------------------------------------------------------------------------
void setup() {
  pinMode(FEETY_LED_PIN, OUTPUT);
  digitalWrite(FEETY_LED_PIN, HIGH);
  pinMode(FEETY_BUTTON_PIN, INPUT_PULLDOWN);

  for (int i = 0; i < FEETY_MOTOR_COUNT; i++) {
    pwmInit(FEETY_MOTOR_PINS[i], i);
  }
  motorsOff();

  analogReadResolution(12);
  analogSetPinAttenuation(FEETY_VBAT_PIN, ADC_11db);

  Serial.begin(115200);
  delay(100);
  Serial.printf("\nFeety hip unit, protocol v%d - type 'help'\n", FEETY_PROTOCOL_VERSION);

  calLoad();

  if (!radioBegin()) {
    Serial.println("ESP-NOW init failed, restarting");
    delay(1000);
    ESP.restart();
  }

  readBattery();
}

void loop() {
  uint32_t now = millis();

  pollConsole();
  handleButton();

  // Take a snapshot of the newest packet. Anything older has already been
  // overwritten, which is exactly what we want - old pressure data is worse
  // than no pressure data.
  bool haveData;
  uint32_t dataAgeMs;
  portENTER_CRITICAL(&rxMux);
  haveData  = rxValid;
  dataAgeMs = now - rxLastMs;
  if (haveData) {
    memcpy(&sensors, (const void *)&rxShared, sizeof(sensors));
  }
  portEXIT_CRITICAL(&rxMux);

  bool linkOk = haveData && (dataAgeMs < FEETY_SIGNAL_TIMEOUT_MS);
  footBattLow = linkOk && (sensors.flags & FEETY_FLAG_BATT_LOW);

  updateMotors(linkOk);

  if (now - lastHeartbeatMs >= FEETY_HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    sendCommand(FEETY_CMD_HEARTBEAT);
  }

  if (now - lastBattMs >= FEETY_BATT_INTERVAL_MS) {
    lastBattMs = now;
    readBattery();
  }

  updateLed(linkOk);

  if (streamRaw && (now - lastStreamMs) >= 200) {
    lastStreamMs = now;
    Serial.printf("raw %4u %4u %4u %4u | duty %3u %3u %3u %3u | link %s | foot %umV\n",
                  sensors.raw[0], sensors.raw[1], sensors.raw[2], sensors.raw[3],
                  lastDuty[0], lastDuty[1], lastDuty[2], lastDuty[3],
                  linkOk ? "ok" : "--", sensors.vbat_mv);
  }
}
