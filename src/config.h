/**
 * @file config.h
 * @brief Persistent configuration management for BTWifiSerial
 *
 * Uses ESP32 Preferences (NVS) to store and retrieve settings.
 * All configurable parameters: serial output mode, BT name, BT role,
 * saved remote BT address.
 */

#pragma once

#include <Arduino.h>

// ─── Hardware pin definitions (ESP32-C3 SuperMini) ──────────────────
static constexpr gpio_num_t PIN_SERIAL_TX    = GPIO_NUM_21;
static constexpr gpio_num_t PIN_SERIAL_RX    = GPIO_NUM_20;
static constexpr gpio_num_t PIN_BOOT_BUTTON  = GPIO_NUM_9;   // Active LOW
static constexpr gpio_num_t PIN_LED          = GPIO_NUM_8;    // Active LOW on SuperMini

// ─── Serial output mode ─────────────────────────────────────────────
enum class OutputMode : uint8_t {
    FRSKY        = 0,  // FrSky CC2540 trainer protocol (115200 8N1)
    SBUS         = 1,  // SBUS trainer (100000 8E2 inverted)
    SPORT_BT     = 2,  // S.PORT telemetry via BT framing (115200 8N1, XOR CRC)
    SPORT_MIRROR = 3   // S.PORT telemetry mirror from AUX2 (57600/115200, raw)
};

// ─── Telemetry output destination ───────────────────────────────────
enum class TelemetryOutput : uint8_t {
    WIFI_UDP = 0,  // Broadcast via WiFi UDP
    BLE      = 1   // Forward via BLE notifications
};

// ─── BLE role ───────────────────────────────────────────────────────
enum class BleRole : uint8_t {
    PERIPHERAL = 0,   // Peripheral (slave) - radio connects to us
    CENTRAL    = 1,   // Central (master) - we connect to a device (e.g. HeadTracker)
    TELEMETRY  = 2    // Telemetry relay mode
};

// ─── Configuration structure ────────────────────────────────────────
struct Config {
    OutputMode      serialMode;
    BleRole         bleRole;
    char            btName[32];
    char            localBtAddr[18];     // "XX:XX:XX:XX:XX:XX\0" (cached)
    char            remoteBtAddr[18];    // "XX:XX:XX:XX:XX:XX\0"
    bool            hasRemoteAddr;
    uint8_t         remoteAddrType;      // 0=public, 1=random

    // Telemetry output settings
    TelemetryOutput telemetryOutput;     // Where to forward S.PORT data
    uint16_t        udpPort;             // UDP broadcast port (default 5010)
    uint32_t        sportBaud;           // Baud for SPORT_MIRROR (57600 or 115200)

    void setDefaults() {
        serialMode      = OutputMode::FRSKY;
        bleRole         = BleRole::PERIPHERAL;
        strlcpy(btName, "BTWifiSerial", sizeof(btName));
        memset(localBtAddr, 0, sizeof(localBtAddr));
        memset(remoteBtAddr, 0, sizeof(remoteBtAddr));
        hasRemoteAddr   = false;
        remoteAddrType  = 0;
        telemetryOutput = TelemetryOutput::WIFI_UDP;
        udpPort         = 5010;
        sportBaud       = 57600;
    }
};

// ─── Public API ─────────────────────────────────────────────────────
void   configInit();
void   configSave();
void   configLoad();

extern Config g_config;
