/*
  Feety - foot unit configuration.

  Everything you are likely to change when adapting the project to your own
  build lives in this file.
*/

#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Pin assignment
//
// All four sensor pins MUST be on ADC1 (GPIO 32-39). ESP-NOW runs on the WiFi
// radio, and the WiFi driver takes exclusive ownership of ADC2 - analogRead()
// on an ADC2 pin (GPIO 0, 2, 4, 12-15, 25-27) returns garbage or blocks while
// the radio is up. The old Bluetooth build used GPIO 15/4/27 and that is where
// its "wrong signals on all pins" behaviour came from.
//
// GPIO 34-39 are input only and have no internal pull resistors, which is
// exactly what you want for a sensor divider: nothing loads the divider and
// falsifies the reading.
// ---------------------------------------------------------------------------
static const uint8_t FEETY_SENSOR_PINS[4] = { 32, 33, 34, 35 };

#define FEETY_VBAT_PIN    39   // ADC1, battery divider
#define FEETY_LED_PIN      2   // status LED (on-board LED on most boards)
#define FEETY_BUTTON_PIN  27   // power button, active HIGH, needs to be an RTC
                               // GPIO so it can wake the chip from deep sleep.
                               // Digital use of an ADC2 pin is fine - only
                               // analogRead() is blocked by the radio.

// ---------------------------------------------------------------------------
// Radio
//
// FEETY_PEER_MAC defaults to broadcast so two freshly flashed boards find each
// other immediately. Replace it with the hip unit's real MAC before you use
// the system for real: broadcast means any second Feety in radio range would
// feed this hip as well, and broadcast frames cannot be encrypted.
//
// Both sketches print their own MAC over serial at boot - flash them, read the
// two addresses, and paste each into the other sketch's config.
// ---------------------------------------------------------------------------
static const uint8_t FEETY_PEER_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define FEETY_WIFI_CHANNEL 1   // must be identical on both units

// ---------------------------------------------------------------------------
// Timing (all in milliseconds, all non-blocking)
// ---------------------------------------------------------------------------
#define FEETY_SEND_INTERVAL_MS    20      // 50 Hz sensor stream
#define FEETY_BATT_INTERVAL_MS    2000    // battery is checked continuously,
                                          // not just once at boot
#define FEETY_LINK_TIMEOUT_MS     1500    // no heartbeat -> show "link lost"
#define FEETY_IDLE_SLEEP_MS       300000  // no heartbeat for 5 min -> deep
                                          // sleep instead of draining the cell

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------
#define FEETY_OVERSAMPLE  8    // averaged samples per sensor per cycle; cheap
                               // and removes most of the ADC noise

// Battery divider ratio: measured voltage * this factor = real cell voltage.
// 1.435 is the value from the original build - measure yours with a multimeter
// and adjust, the ADC cannot know your resistor values.
#define FEETY_VBAT_DIVIDER 1.435f

#define FEETY_VBAT_LOW_MV      3400   // warn (flag in packet, LED pattern)
#define FEETY_VBAT_CRITICAL_MV 3200   // shut down to protect the Li-Ion cell

// Long press on the power button (ms) to enter deep sleep.
#define FEETY_SLEEP_PRESS_MS 3000

// Set to 0 to strip the serial diagnostics once the unit is in a case.
#define FEETY_DEBUG 1
