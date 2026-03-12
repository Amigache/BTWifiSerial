/**
 * @file config.h
 * @brief Persistent configuration management for BTWifiSerial
 *
 * Uses ESP32 Preferences (NVS) to store and retrieve settings.
 * All configurable parameters: serial output mode, device mode, BT name,
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
    SPORT_MIRROR = 3,  // S.PORT telemetry mirror from AUX2 (57600/115200, raw)
    LUA_SERIAL   = 4   // EdgeTX LUA serial: bidirectional channel + command protocol
};

// ─── WiFi mode ──────────────────────────────────────────────────────
enum class WifiMode : uint8_t {
    OFF = 0,  // WiFi disabled
    AP  = 1,  // Soft Access Point (portal / UDP broadcast)
    STA = 2,  // Station — connect to an existing network
};

// ─── Telemetry output destination ───────────────────────────────────
enum class TelemetryOutput : uint8_t {
    WIFI_UDP = 0,  // Broadcast via WiFi UDP
    BLE      = 1,  // Forward via BLE notifications
    NONE     = 2   // No telemetry output (discard packets)
};

// ─── Device mode ────────────────────────────────────────────────────
enum class DeviceMode : uint8_t {
    TRAINER_IN  = 0,  // BLE Central: receive channels from a remote device
    TRAINER_OUT = 1,  // BLE Peripheral: send radio channels to a remote device
    TELEMETRY   = 2   // Telemetry relay: forward S.PORT via WiFi UDP or BLE
};

// ─── Trainer channel mapping mode ───────────────────────────────────
enum class TrainerMapMode : uint8_t {
    MAP_GV = 0,  // Inject BLE channels into Global Variables (GV1–GV8)
    MAP_TR = 1   // Inject BLE channels into Trainer inputs via setTrainerChannels()
};

inline bool bleIsCentral(DeviceMode m) { return m == DeviceMode::TRAINER_IN; }

// ─── Configuration structure ────────────────────────────────────────
struct Config {
    OutputMode      serialMode;
    DeviceMode      deviceMode;
    char            btName[32];
    char            localBtAddr[18];     // "XX:XX:XX:XX:XX:XX\0" (cached)
    char            remoteBtAddr[18];    // "XX:XX:XX:XX:XX:XX\0"
    bool            hasRemoteAddr;
    uint8_t         remoteAddrType;      // 0=public, 1=random

    // Telemetry output settings
    TelemetryOutput telemetryOutput;     // Where to forward S.PORT data
    uint16_t        udpPort;             // UDP broadcast port (default 5010)
    uint32_t        sportBaud;           // Baud for SPORT_MIRROR (57600 or 115200)

    // Trainer channel mapping
    TrainerMapMode  trainerMapMode;      // GV (global vars) or TR (trainer channels)

    // WiFi mode + credentials
    WifiMode        wifiMode;            // Off / AP / STA
    char            apSsid[16];          // AP SSID (max 15 chars + null)
    char            apPass[16];          // AP password (max 15 chars + null)
    char            staSsid[32];         // STA SSID to connect to (max 31 + null)
    char            staPass[64];         // STA password (max 63 + null)

    void setDefaults() {
        serialMode      = OutputMode::LUA_SERIAL;
        deviceMode      = DeviceMode::TRAINER_IN;
        strlcpy(btName, "BTWifiSerial", sizeof(btName));
        memset(localBtAddr, 0, sizeof(localBtAddr));
        memset(remoteBtAddr, 0, sizeof(remoteBtAddr));
        hasRemoteAddr   = false;
        remoteAddrType  = 0;
        telemetryOutput = TelemetryOutput::NONE;
        udpPort         = 5010;
        sportBaud       = 57600;
        trainerMapMode  = TrainerMapMode::MAP_GV;
        wifiMode        = WifiMode::OFF;
        strlcpy(apSsid, "BTWifiSerial", sizeof(apSsid));
        strlcpy(apPass, "12345678", sizeof(apPass));
        memset(staSsid, 0, sizeof(staSsid));
        memset(staPass, 0, sizeof(staPass));
    }
};

// ─── Public API ─────────────────────────────────────────────────────
void   configInit();
void   configSave();
void   configLoad();

// ─── Build timestamp ────────────────────────────────────────────────
// Format: DDMMYYYYHHMM  e.g. "070320261021" for 07/03/2026 at 10:21
extern const char* BUILD_TIMESTAMP;

extern Config g_config;
