/*
  Feety - hip unit configuration.
*/

#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Pin assignment
//
// The motor pins are the ones from the original build. They are PWM outputs,
// so it does not matter that GPIO 25/26/27 belong to ADC2 - the WiFi/ADC2
// conflict only affects analogRead().
//
// The battery sense pin does have to be on ADC1 (GPIO 32-39) for the same
// reason as on the foot unit.
// ---------------------------------------------------------------------------
static const uint8_t FEETY_MOTOR_PINS[4] = { 14, 27, 26, 25 };

#define FEETY_VBAT_PIN    35   // ADC1, battery divider
#define FEETY_LED_PIN     32   // status LED
#define FEETY_BUTTON_PIN   4   // active HIGH, RTC GPIO (deep sleep wake source)

// ---------------------------------------------------------------------------
// Radio - see the comment in the foot unit's feety_config.h
// ---------------------------------------------------------------------------
static const uint8_t FEETY_PEER_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define FEETY_WIFI_CHANNEL 1   // must be identical on both units

// ---------------------------------------------------------------------------
// Timing (milliseconds)
// ---------------------------------------------------------------------------
#define FEETY_HEARTBEAT_INTERVAL_MS 500     // keeps the foot unit awake
#define FEETY_SIGNAL_TIMEOUT_MS     300     // no fresh data -> motors OFF.
                                            // Safety: never keep vibrating on
                                            // stale readings.
#define FEETY_BATT_INTERVAL_MS      5000
#define FEETY_SLEEP_PRESS_MS        3000    // long press -> both units sleep
#define FEETY_DEBOUNCE_MS           40

// ---------------------------------------------------------------------------
// Motors
// ---------------------------------------------------------------------------
#define FEETY_PWM_FREQ_HZ  20000   // above the audible range, so the motors do
                                   // not whistle. The old analogWrite() default
                                   // was ~1 kHz, right in the middle of it.
#define FEETY_PWM_BITS     8       // duty 0..255

// ERM motors need a short full-power pulse to break away from standstill,
// otherwise low duty cycles produce nothing but a hum.
#define FEETY_KICK_MS      40
#define FEETY_KICK_DUTY    255

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------
#define FEETY_VBAT_DIVIDER      1.435f   // measure and adjust for your build
#define FEETY_VBAT_LOW_MV       3400
#define FEETY_VBAT_CRITICAL_MV  3200

// ---------------------------------------------------------------------------
// Calibration defaults
//
// Per channel: below "threshold" the motor stays off, at "saturation" and
// above it runs at pwmMax, in between the duty is interpolated from pwmMin to
// pwmMax. These replace the hardcoded +70/+100/+50/-50 offsets and the
// 200/125/40/30 step ladder of the original sketch.
//
// The values live in NVS and can be changed at runtime over the serial console
// ("help" lists the commands) - no reflash needed to calibrate a new sole, and
// the same commands are what a phone app would later send.
// ---------------------------------------------------------------------------
#define FEETY_CAL_THRESHOLD   400    // raw ADC counts (0..4095)
#define FEETY_CAL_SATURATION  3200
#define FEETY_CAL_PWM_MIN     70     // lowest duty that still moves the motor
#define FEETY_CAL_PWM_MAX     255

// Set to 0 to strip the serial console and diagnostics.
#define FEETY_DEBUG 1
