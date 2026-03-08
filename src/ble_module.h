/**
 * @file ble_module.h
 * @brief BLE module for BTWifiSerial
 *
 * Supports three roles:
 * - Peripheral: Advertises FrSky service (0xFFF0), receives channel data from radio
 *   via GATT writes/notifications and forwards to serial output.
 * - Central: Scans and connects to a remote BLE device (e.g. HeadTracker),
 *   subscribes to FrSky characteristic (0xFFF6), receives channel data.
 * - Telemetry: Bidirectional relay (future).
 *
 * Uses NimBLE for lightweight BLE stack on ESP32-C3.
 */

#pragma once

#include <Arduino.h>

// ─── Scan result entry ──────────────────────────────────────────────
struct BleScanResult {
    char    address[18];   // "XX:XX:XX:XX:XX:XX"
    int     rssi;
    char    name[32];
    bool    hasFrskyService;
    uint8_t addrType;      // 0=public, 1=random (needed for NimBLEAddress)
};

static constexpr uint8_t MAX_SCAN_RESULTS = 16;

// ─── Public API ─────────────────────────────────────────────────────
void bleInit();
void bleStop();
void bleScheduleReinit();  // thread-safe: schedules stop+init from loop task
void bleUpdateAdvertisingName(); // Peripheral only: restart adv with new name, no deinit
void bleLoop();           // Non-blocking, call from main loop

// Central mode
bool bleScanStart();
void bleScanStop();
bool bleIsScanning();
uint8_t bleGetScanResults(BleScanResult* results, uint8_t maxCount);
bool bleConnectTo(const char* address);
void bleDisconnect();
void bleKickClient();   // Peripheral: disconnect the currently connected central
void bleForget();       // Central: disconnect + clear saved address (no auto-reconnect)

// Status
bool bleIsInitialized();  // true after bleInit(), false after bleStop()
bool bleIsConnected();
bool bleIsConnecting();   // true while a connection attempt is in progress
const char* bleGetLocalAddress();
const char* bleGetRemoteAddress();
int  bleGetRSSI();

// Telemetry forwarding (send raw bytes as BLE notification)
void bleSendRawNotification(const uint8_t* data, size_t len);
