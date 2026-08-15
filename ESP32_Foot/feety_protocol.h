/*
  Feety - radio protocol shared by the foot unit and the hip unit.

  IMPORTANT: this file exists twice, once in ESP32_Foot/ and once in ESP32_Hip/.
  The Arduino IDE cannot include headers from outside a sketch folder, so both
  copies must stay byte-for-byte identical. If you change anything here, bump
  FEETY_PROTOCOL_VERSION and copy the file to the other sketch folder. The
  receiver rejects packets whose version does not match, so a forgotten copy
  shows up as "no data" instead of as silently garbled sensor values.
*/

#pragma once

#include <stdint.h>

#define FEETY_PROTOCOL_VERSION 1

// Number of pressure sensors in the sole and vibration motors on the hip.
// These are separate constants on purpose: a future build may drive fewer
// motors than there are sensors (or the other way round).
#define FEETY_SENSOR_COUNT 4
#define FEETY_MOTOR_COUNT  4

// ---------------------------------------------------------------------------
// foot -> hip: one packet per sampling cycle
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
  uint8_t  version;                    // FEETY_PROTOCOL_VERSION
  uint8_t  flags;                      // see FEETY_FLAG_*
  uint16_t seq;                        // increments per packet, wraps around
  uint16_t raw[FEETY_SENSOR_COUNT];    // raw 12 bit ADC readings, 0..4095
  uint16_t vbat_mv;                    // foot battery voltage in millivolts
} feety_sensor_packet_t;               // 14 bytes

#define FEETY_FLAG_BATT_LOW 0x01       // foot battery below the warning level

// ---------------------------------------------------------------------------
// hip -> foot: control channel
//
// The heartbeat doubles as the foot's link check. As long as heartbeats keep
// arriving the foot knows the hip is listening; when they stop it eventually
// puts itself to sleep instead of transmitting into the void on battery.
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
  uint8_t version;                     // FEETY_PROTOCOL_VERSION
  uint8_t command;                     // see FEETY_CMD_*
} feety_cmd_packet_t;                  // 2 bytes

#define FEETY_CMD_HEARTBEAT 0x00       // "still here"
#define FEETY_CMD_SLEEP     0x01       // hip is shutting down, follow it

// Broadcast address. Used as the default peer so a fresh pair of boards talks
// to each other without any configuration - see FEETY_PEER_MAC in
// feety_config.h for why you want to replace it with a real MAC.
#define FEETY_BROADCAST_MAC { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
