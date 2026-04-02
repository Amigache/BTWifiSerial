/**
 * @file lua_serial.h
 * @brief EdgeTX LUA Serial — multi-channel binary protocol
 *
 * Physical layer: UART1, GPIO21 TX / GPIO20 RX, 115200 8N1.
 * EdgeTX AUX port must be set to "LUA" at 115200 baud.
 *
 * Frame format (all directions):
 *   [SYNC:0xAA] [CH:1] [TYPE:1] [LEN:1] [PAYLOAD:LEN] [CRC:1]
 *   CRC = XOR(CH, TYPE, LEN, payload[0..LEN-1])
 *
 * Logical channels:
 *   0x01  CH_PREF   — Preferences / configuration  (bidirectional)
 *   0x02  CH_INFO   — Status / channels / BLE scan  (bidirectional)
 *   0x03  CH_TRANS  — Transparent byte passthrough  (bidirectional)
 *
 * CH_PREF types (ESP32 → Lua):
 *   0x01  PT_PREF_BEGIN    payload: count(1)
 *   0x02  PT_PREF_ITEM     payload: id(1) type(1) flags(1) llen(1) label(N) <type data>
 *   0x03  PT_PREF_END      no payload
 *   0x04  PT_PREF_UPDATE   payload: id(1) type(1) value(var) — value-only update
 *   0x05  PT_PREF_ACK      payload: id(1) result(1)
 *
 * CH_PREF types (Lua → ESP32):
 *   0x10  PT_PREF_REQUEST  no payload — request full pref list
 *   0x11  PT_PREF_SET      payload: id(1) type(1) value(var)
 *
 * CH_INFO types (ESP32 → Lua):
 *   0x01  PT_INFO_CHANNELS    payload: 8×int16 BE = 16 bytes
 *   0x02  PT_INFO_STATUS      payload: status(1)  bit0=BLE bit1=WiFiClients bit2=Connecting
 *                                              bit3=restartPending
 *   0x03  PT_INFO_BEGIN       payload: count(1)
 *   0x04  PT_INFO_ITEM        payload: id(1) type(1) llen(1) label(N) value(var)
 *   0x05  PT_INFO_END         no payload
 *   0x06  PT_INFO_UPDATE      payload: id(1) type(1) value(var)
 *   0x07  PT_INFO_SCAN_STATUS payload: state(1) count(1)  state: 0=idle 1=scanning 2=done
 *   0x08  PT_INFO_SCAN_ITEM   payload: idx(1) rssi_s8(1) flags(1) nlen(1) name(N) addr(17)
 *
 * CH_INFO types (Lua → ESP32):
 *   0x10  PT_INFO_REQUEST        no payload
 *   0x11  PT_INFO_HEARTBEAT      no payload
 *   0x12  PT_INFO_BLE_SCAN       no payload
 *   0x13  PT_INFO_BLE_CONNECT    payload: idx(1)
 *   0x14  PT_INFO_BLE_DISCONNECT no payload
 *   0x15  PT_INFO_BLE_FORGET     no payload
 *   0x16  PT_INFO_BLE_RECONNECT  no payload
 *
 * CH_TRANS types (bidirectional):
 *   0x01  PT_TRANS_SBUS   — raw SBUS bytes
 *   0x02  PT_TRANS_SPORT  — raw S.PORT packet: physId(1) primId(1) dataId(2 LE) value(4 LE)
 *   0x03  PT_TRANS_FRSKY  — raw FrSky CC2540 bytes
 *
 * Preference field types (FT_*):
 *   0  FT_ENUM    options+curIdx
 *   1  FT_STRING  maxLen+value
 *   2  FT_INT     min+max+value (int16 LE)
 *   3  FT_BOOL    value(1)
 *
 * Pref flags (PF_*):
 *   bit0  PF_RESTART   — device restart required to apply
 *   bit1  PF_RDONLY    — read-only (PREF_SET ignored)
 *   bit2  PF_DASHBOARD — display on Dashboard System section
 *
 * Preference IDs:
 *   0x01  WIFI_MODE   ENUM  ["Off","AP","STA"]  PF_RESTART|PF_DASHBOARD
 *   0x02  DEV_MODE    ENUM  ["Trainer IN","Trainer OUT","Telemetry"]  PF_RESTART|PF_DASHBOARD
 *   0x03  TELEM_OUT   ENUM  ["WiFi UDP","BLE","Off"]  PF_RESTART|PF_DASHBOARD
 *   0x04  MIRROR_BAUD ENUM  ["57600","115200"]  PF_RESTART
 *   0x05  MAP_MODE    ENUM  ["GV","TR"]
 *   0x06  BT_NAME     STRING maxLen=15  PF_DASHBOARD
 *   0x07  AP_SSID     STRING maxLen=15  PF_RESTART
 *   0x08  UDP_PORT    STRING maxLen=5   PF_RESTART
 *   0x09  AP_PASS     STRING maxLen=15  PF_RESTART
 *   0x0A  STA_SSID    STRING maxLen=31  PF_RESTART
 *   0x0B  STA_PASS    STRING maxLen=63  PF_RESTART
 *   0x0C  ELRS_BIND   STRING maxLen=31  PF_RESTART — ELRS binding phrase
 *
 * Info IDs:
 *   0x01  FIRMWARE  STRING  build timestamp "DDMMYYYY HHMM"
 *   0x02  BT_ADDR   STRING  local BT MAC "XX:XX:XX:XX:XX:XX"
 *   0x03  REM_ADDR  STRING  saved remote MAC or "(none)"
 *   0x04  WIFI_IP   STRING  AP IP / STA DHCP IP / "(none)" when WiFi is off
 */

#pragma once

#include <Arduino.h>

// ── Physical layer ───────────────────────────────────────────────────
static constexpr uint32_t LUA_BAUD = 115200;

// ── Frame sync ───────────────────────────────────────────────────────
static constexpr uint8_t LUA_SYNC = 0xAA;

// ── Logical channels ────────────────────────────────────────────────
static constexpr uint8_t LUA_CH_PREF  = 0x01;
static constexpr uint8_t LUA_CH_INFO  = 0x02;
static constexpr uint8_t LUA_CH_TRANS = 0x03;

// ── CH_PREF types ── ESP32 → Lua ────────────────────────────────────
static constexpr uint8_t LUA_PT_PREF_BEGIN  = 0x01;
static constexpr uint8_t LUA_PT_PREF_ITEM   = 0x02;
static constexpr uint8_t LUA_PT_PREF_END    = 0x03;
static constexpr uint8_t LUA_PT_PREF_UPDATE = 0x04;
static constexpr uint8_t LUA_PT_PREF_ACK    = 0x05;

// ── CH_PREF types ── Lua → ESP32 ────────────────────────────────────
static constexpr uint8_t LUA_PT_PREF_REQUEST = 0x10;
static constexpr uint8_t LUA_PT_PREF_SET     = 0x11;

// ── CH_INFO types ── ESP32 → Lua ────────────────────────────────────
static constexpr uint8_t LUA_PT_INFO_CHANNELS    = 0x01;
static constexpr uint8_t LUA_PT_INFO_STATUS      = 0x02;
static constexpr uint8_t LUA_PT_INFO_BEGIN       = 0x03;
static constexpr uint8_t LUA_PT_INFO_ITEM        = 0x04;
static constexpr uint8_t LUA_PT_INFO_END         = 0x05;
static constexpr uint8_t LUA_PT_INFO_UPDATE      = 0x06;
static constexpr uint8_t LUA_PT_INFO_SCAN_STATUS = 0x07;
static constexpr uint8_t LUA_PT_INFO_SCAN_ITEM   = 0x08;

// ── CH_INFO types ── Lua → ESP32 ────────────────────────────────────
static constexpr uint8_t LUA_PT_INFO_REQUEST        = 0x10;
static constexpr uint8_t LUA_PT_INFO_HEARTBEAT      = 0x11;
static constexpr uint8_t LUA_PT_INFO_BLE_SCAN       = 0x12;
static constexpr uint8_t LUA_PT_INFO_BLE_CONNECT    = 0x13;
static constexpr uint8_t LUA_PT_INFO_BLE_DISCONNECT = 0x14;
static constexpr uint8_t LUA_PT_INFO_BLE_FORGET     = 0x15;
static constexpr uint8_t LUA_PT_INFO_BLE_RECONNECT  = 0x16;
// ── CH_INFO types ── WiFi scan ─────────────────────────────────────
static constexpr uint8_t LUA_PT_INFO_WIFI_SCAN_STATUS = 0x09;  ///< ESP32→Lua: state(1) count(1); state: 0=fail 1=scanning 2=done
static constexpr uint8_t LUA_PT_INFO_WIFI_SCAN_ITEM   = 0x0A;  ///< ESP32→Lua: idx(1) rssi_s8(1) ssid_len(1) ssid(N)
static constexpr uint8_t LUA_PT_INFO_WIFI_SCAN        = 0x17;  ///< Lua→ESP32: start WiFi scan; no payload
static constexpr uint8_t LUA_PT_INFO_RESTART          = 0x18;  ///< Lua→ESP32: apply pending config and restart

// ── CH_TRANS types ───────────────────────────────────────────────────
static constexpr uint8_t LUA_PT_TRANS_SBUS  = 0x01;
static constexpr uint8_t LUA_PT_TRANS_SPORT = 0x02;
static constexpr uint8_t LUA_PT_TRANS_FRSKY = 0x03;

// ── Field types ──────────────────────────────────────────────────────
static constexpr uint8_t LUA_FT_ENUM   = 0;
static constexpr uint8_t LUA_FT_STRING = 1;
static constexpr uint8_t LUA_FT_INT    = 2;
static constexpr uint8_t LUA_FT_BOOL   = 3;

// ── Pref flags ────────────────────────────────────────────────────────
static constexpr uint8_t LUA_PF_RESTART   = 0x01;
static constexpr uint8_t LUA_PF_RDONLY    = 0x02;
static constexpr uint8_t LUA_PF_DASHBOARD = 0x04;
static constexpr uint8_t LUA_PF_NUMERIC   = 0x08;  ///< FT_STRING pref: only digits 0-9 are valid

// ── Preference IDs ───────────────────────────────────────────────────
static constexpr uint8_t LUA_PREF_WIFI_MODE   = 0x01;  ///< ENUM ["Off","AP","STA"]
static constexpr uint8_t LUA_PREF_DEV_MODE    = 0x02;
static constexpr uint8_t LUA_PREF_TELEM_OUT   = 0x03;
static constexpr uint8_t LUA_PREF_MIRROR_BAUD = 0x04;
static constexpr uint8_t LUA_PREF_MAP_MODE    = 0x05;
static constexpr uint8_t LUA_PREF_BT_NAME     = 0x06;
static constexpr uint8_t LUA_PREF_AP_SSID     = 0x07;
static constexpr uint8_t LUA_PREF_UDP_PORT    = 0x08;
static constexpr uint8_t LUA_PREF_AP_PASS     = 0x09;
static constexpr uint8_t LUA_PREF_STA_SSID    = 0x0A;
static constexpr uint8_t LUA_PREF_STA_PASS    = 0x0B;
static constexpr uint8_t LUA_PREF_ELRS_BIND   = 0x0C;  ///< STRING maxLen=31 — ELRS binding phrase
static constexpr uint8_t LUA_PREF_COUNT       = 12;

// ── Info IDs ─────────────────────────────────────────────────────────
static constexpr uint8_t LUA_INFO_FIRMWARE = 0x01;
static constexpr uint8_t LUA_INFO_BT_ADDR  = 0x02;
static constexpr uint8_t LUA_INFO_REM_ADDR = 0x03;
static constexpr uint8_t LUA_INFO_WIFI_IP  = 0x04;
static constexpr uint8_t LUA_INFO_COUNT    = 4;

// ── Timing ───────────────────────────────────────────────────────────
static constexpr uint32_t LUA_CH_FRAME_INTERVAL  = 20;     // ms — 50 Hz channel TX
static constexpr uint32_t LUA_STATUS_INTERVAL    = 500;    // ms
static constexpr uint32_t LUA_CFG_INTERVAL       = 30000;  // ms — periodic full resync
static constexpr uint32_t LUA_TOOLS_IDLE_TIMEOUT = 15000;  // ms — stop heavy TX when idle

// ── Public API ───────────────────────────────────────────────────────
void luaSerialInit();
void luaSerialLoop();
void luaSerialStop();
void luaSerialSetApMode(uint8_t mode);  // 0=normal, 1=AP-block, 2=telemetry-AP
