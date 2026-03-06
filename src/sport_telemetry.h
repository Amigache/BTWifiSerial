/**
 * @file sport_telemetry.h
 * @brief S.PORT telemetry receiver and forwarder
 *
 * Receives S.PORT telemetry from EdgeTX via two possible input modes:
 * - BT-framed: EdgeTX BLUETOOTH=ON sends re-framed S.PORT at 115200 8N1
 *              with 0x7E framing, byte-stuffing, XOR CRC, batched 2 packets
 * - Raw mirror: EdgeTX AUX2 Telemetry Mirror sends raw S.PORT at 57600/115200
 *              as a transparent copy of the RF module's S.PORT bus
 *
 * Parsed telemetry packets are forwarded to the selected output:
 * - WiFi UDP: broadcast on configurable port (default 5010)
 * - BLE: send as notifications on FrSky characteristic (0xFFF6)
 */

#pragma once

#include <Arduino.h>

// ─── S.PORT telemetry packet (parsed, 8 bytes) ─────────────────────
struct SportPacket {
    uint8_t  physId;     // Physical sensor ID
    uint8_t  primId;     // Primitive ID (0x10 = DATA_FRAME)
    uint16_t dataId;     // Sensor data ID (e.g. 0x0100 = altitude)
    uint32_t value;      // 32-bit sensor value
} __attribute__((packed));

static constexpr uint8_t SPORT_DATA_FRAME = 0x10;

// ─── Public API ─────────────────────────────────────────────────────
void sportTelemetryInit();
void sportTelemetryLoop();
void sportTelemetryStop();

// ─── Output control ─────────────────────────────────────────────────
bool sportUdpIsActive();          // Is WiFi STA + UDP running?
bool sportBleIsForwarding();      // Is BLE telemetry forwarding active?

// ─── Stats ──────────────────────────────────────────────────────────
uint32_t sportGetPacketCount();   // Total parsed S.PORT packets
uint32_t sportGetPacketsPerSec(); // Packets per second (rolling average)
