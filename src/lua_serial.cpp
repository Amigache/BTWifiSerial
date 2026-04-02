/**
 * @file lua_serial.cpp
 * @brief EdgeTX LUA Serial — multi-channel binary protocol implementation.
 *
 * See lua_serial.h for full protocol documentation.
 *
 * UART: 115200 baud, 8N1, UART1 (GPIO21 TX / GPIO20 RX).
 */

#include "lua_serial.h"
#include "config.h"
#include "channel_data.h"
#include "ble_module.h"
#include "elrs_espnow.h"
#include "sport_telemetry.h"
#include "build_ts_gen.h"
#include "log.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/uart.h>
#include <algorithm>

// ── Module state ────────────────────────────────────────────────────
static bool     s_running          = false;
static uint8_t  s_apMode           = 0;    // 0=normal, 1=AP-block, 2=telemetry-AP, 3=STA
static uint32_t s_lastChMs         = 0;
static uint32_t s_lastStatusMs     = 0;
static uint32_t s_lastCfgMs        = 0;
static uint32_t s_lastToolsCmdMs   = 0;
static bool     s_lastBleConnected = false;

// BLE scan drip-feed state
static bool          s_scanWasActive   = false;
static bool          s_scanSending     = false;
static uint8_t       s_scanSendIdx     = 0;
static uint8_t       s_scanSendTotal   = 0;
static uint32_t      s_lastScanEntryMs = 0;
static BleScanResult s_scanCache[MAX_SCAN_RESULTS];

// WiFi scan task state
static volatile bool    s_wifiScanDone    = false;
static volatile int16_t s_wifiScanCount   = 0;
static bool             s_wifiScanActive  = false;  // true: task running or drip-feed in progress
static bool             s_wifiScanSending = false;
static uint8_t          s_wifiScanSendIdx = 0;
static uint32_t         s_lastWifiScanMs  = 0;

// Telemetry output tracking
static bool s_tlmOutputActive = false;
static bool s_restartPending  = false;
static char s_lastWifiIp[32]  = "";

// ── RX state machine ────────────────────────────────────────────────
// States: 0=wait-sync 1=ch 2=type 3=len 4=accumulate 5=crc
static uint8_t  s_rxState  = 0;
static uint8_t  s_rxCh     = 0;
static uint8_t  s_rxType   = 0;
static uint8_t  s_rxLen    = 0;
static uint8_t  s_rxBuf[80];   // max incoming payload: PREF_SET STA_PASS = 1+1+1+63 = 66 bytes
static uint8_t  s_rxPos    = 0;
static uint8_t  s_rxCrcAcc = 0;

// ── Functions in main.cpp ────────────────────────────────────────────
extern void mainRequestApMode();
extern void mainRequestNormalMode();
extern void mainSetDeviceMode(uint8_t mode);
extern void mainSetTelemOutput(uint8_t output);
extern void mainSetMirrorBaud(uint32_t baud);
extern void mainSetWifiMode(uint8_t mode);
extern void mainRequestConfigRestart();
// ── Generic frame sender ─────────────────────────────────────────────
// Builds and sends: [SYNC][CH][TYPE][LEN][payload...][CRC]
// CRC = XOR(CH ^ TYPE ^ LEN ^ payload[0..len-1])

static void sendFrame(uint8_t ch, uint8_t typ, const uint8_t* payload, uint8_t len) {
    // Max frame = 5 overhead + 255 payload = 260 bytes (payload limited to 48 here)
    uint8_t buf[260];
    buf[0] = LUA_SYNC;
    buf[1] = ch;
    buf[2] = typ;
    buf[3] = len;
    uint8_t crc = ch ^ typ ^ len;
    for (uint8_t i = 0; i < len; i++) {
        buf[4 + i] = payload[i];
        crc ^= payload[i];
    }
    buf[4 + len] = crc;
    uart_write_bytes(UART_NUM_1, (const char*)buf, (size_t)(5 + len));
}

// ── Helpers for PREF_ITEM / PREF_UPDATE payload building ─────────────

// Convert the current s_apMode → WiFi Mode enum index seen by Lua
// 0=normal → 0 ("Off"),  1=AP-block or 2=telem-AP → 1 ("AP"),  3=STA → 2 ("STA")
static inline uint8_t wifiModeIdx() {
    if (s_apMode == 3) return 2;  // STA
    return (s_apMode == 0) ? 0 : 1;  // Off / AP
}

// Append a length-prefixed string to a buffer. Returns new pos.
static uint8_t appendLenStr(uint8_t* buf, uint8_t pos, const char* str) {
    uint8_t n = (uint8_t)strlen(str);
    buf[pos++] = n;
    memcpy(&buf[pos], str, n);
    return pos + n;
}

// ── PREF channel senders ─────────────────────────────────────────────

static void sendPrefBegin(uint8_t count) {
    sendFrame(LUA_CH_PREF, LUA_PT_PREF_BEGIN, &count, 1);
}

static void sendPrefEnd() {
    sendFrame(LUA_CH_PREF, LUA_PT_PREF_END, nullptr, 0);
}

static void sendPrefItem(uint8_t id) {
    uint8_t buf[128];
    uint8_t pos = 0;

    buf[pos++] = id;

    const char* label = nullptr;
    uint8_t ftype = LUA_FT_ENUM;
    uint8_t flags = 0;

    switch (id) {
        case LUA_PREF_WIFI_MODE:    label = "WiFi Mode";     ftype = LUA_FT_ENUM;   flags = LUA_PF_RESTART | LUA_PF_DASHBOARD; break;
        case LUA_PREF_DEV_MODE:    label = "Device Mode";   ftype = LUA_FT_ENUM;   flags = LUA_PF_RESTART | LUA_PF_DASHBOARD; break;
        case LUA_PREF_TELEM_OUT:   label = "Telem Out";     ftype = LUA_FT_ENUM;   flags = LUA_PF_RESTART | LUA_PF_DASHBOARD; break;
        case LUA_PREF_MIRROR_BAUD: label = "Mirror Baud";   ftype = LUA_FT_ENUM;   flags = LUA_PF_RESTART;                    break;
        case LUA_PREF_MAP_MODE:    label = "Trainer Map";   ftype = LUA_FT_ENUM;   flags = 0;                                 break;
        case LUA_PREF_BT_NAME:     label = "BT Name";       ftype = LUA_FT_STRING; flags = LUA_PF_DASHBOARD;                  break;
        case LUA_PREF_AP_SSID:     label = "AP SSID";       ftype = LUA_FT_STRING; flags = LUA_PF_RESTART;                    break;
        case LUA_PREF_UDP_PORT:    label = "UDP Port";       ftype = LUA_FT_STRING; flags = LUA_PF_RESTART | LUA_PF_NUMERIC;   break;
        case LUA_PREF_AP_PASS:     label = "AP Password";   ftype = LUA_FT_STRING; flags = LUA_PF_RESTART;                    break;
        case LUA_PREF_STA_SSID:    label = "STA SSID";      ftype = LUA_FT_STRING; flags = 0;                                  break;
        case LUA_PREF_STA_PASS:    label = "STA Password";  ftype = LUA_FT_STRING; flags = LUA_PF_RESTART;                    break;
        case LUA_PREF_ELRS_BIND:   label = "ELRS Phrase";   ftype = LUA_FT_STRING; flags = LUA_PF_RESTART;                    break;
        default: return;
    }

    buf[pos++] = ftype;
    buf[pos++] = flags;
    pos = appendLenStr(buf, pos, label);

    // Type-specific payload
    switch (id) {
        case LUA_PREF_WIFI_MODE: {
            static const char* opts[] = { "Off", "AP", "STA" };
            buf[pos++] = 3;
            buf[pos++] = wifiModeIdx();
            for (int i = 0; i < 3; i++) pos = appendLenStr(buf, pos, opts[i]);
            break;
        }
        case LUA_PREF_DEV_MODE: {
            static const char* opts[] = { "Trainer IN", "Trainer OUT", "Telemetry", "ELRS HT" };
            buf[pos++] = 4;
            buf[pos++] = (uint8_t)g_config.deviceMode;
            for (int i = 0; i < 4; i++) pos = appendLenStr(buf, pos, opts[i]);
            break;
        }
        case LUA_PREF_TELEM_OUT: {
            static const char* opts[] = { "WiFi UDP", "BLE", "Off" };
            buf[pos++] = 3;
            buf[pos++] = (uint8_t)g_config.telemetryOutput;
            for (int i = 0; i < 3; i++) pos = appendLenStr(buf, pos, opts[i]);
            break;
        }
        case LUA_PREF_MIRROR_BAUD: {
            static const char* opts[] = { "57600", "115200" };
            buf[pos++] = 2;
            buf[pos++] = (g_config.sportBaud == 115200) ? 1 : 0;
            for (int i = 0; i < 2; i++) pos = appendLenStr(buf, pos, opts[i]);
            break;
        }
        case LUA_PREF_MAP_MODE: {
            static const char* opts[] = { "GV", "TR" };
            buf[pos++] = 2;
            buf[pos++] = (uint8_t)g_config.trainerMapMode;
            for (int i = 0; i < 2; i++) pos = appendLenStr(buf, pos, opts[i]);
            break;
        }
        case LUA_PREF_BT_NAME: {
            buf[pos++] = 15;   // maxLen
            pos = appendLenStr(buf, pos, g_config.btName);
            break;
        }
        case LUA_PREF_AP_SSID: {
            buf[pos++] = 15;
            pos = appendLenStr(buf, pos, g_config.apSsid);
            break;
        }
        case LUA_PREF_UDP_PORT: {
            char portStr[6];
            snprintf(portStr, sizeof(portStr), "%u", g_config.udpPort);
            buf[pos++] = 5;    // maxLen (5 digits)
            pos = appendLenStr(buf, pos, portStr);
            break;
        }
        case LUA_PREF_AP_PASS: {
            buf[pos++] = 15;
            pos = appendLenStr(buf, pos, g_config.apPass);
            break;
        }
        case LUA_PREF_STA_SSID: {
            buf[pos++] = 31;
            pos = appendLenStr(buf, pos, g_config.staSsid);
            break;
        }
        case LUA_PREF_STA_PASS: {
            buf[pos++] = 63;
            pos = appendLenStr(buf, pos, g_config.staPass);
            break;
        }
        case LUA_PREF_ELRS_BIND: {
            buf[pos++] = 31;   // maxLen
            pos = appendLenStr(buf, pos, g_config.elrsBindPhrase);
            break;
        }
    }

    sendFrame(LUA_CH_PREF, LUA_PT_PREF_ITEM, buf, pos);
}

static void sendPrefAll() {
    static const uint8_t ids[] = {
        LUA_PREF_WIFI_MODE, LUA_PREF_DEV_MODE, LUA_PREF_TELEM_OUT,
        LUA_PREF_MIRROR_BAUD, LUA_PREF_MAP_MODE,
        LUA_PREF_BT_NAME, LUA_PREF_AP_SSID, LUA_PREF_UDP_PORT, LUA_PREF_AP_PASS,
        LUA_PREF_STA_SSID, LUA_PREF_STA_PASS,
        LUA_PREF_ELRS_BIND
    };
    sendPrefBegin(LUA_PREF_COUNT);
    for (uint8_t id : ids) sendPrefItem(id);
    sendPrefEnd();
}

// ── Pref / Info drip-feed ─────────────────────────────────────────────
// Sends PREF and/or INFO frames one-at-a-time with gaps to prevent
// overflowing the radio’s small serial RX buffer.  Similar pattern
// to the existing BLE / WiFi scan drip-feeds.
// Forward declarations for functions used in processDrip()
static void sendInfoBegin(uint8_t count);
static void sendInfoEnd();
static void sendInfoItem(uint8_t id);
static void sendStatusFrame();


static constexpr uint32_t DRIP_INTERVAL_MS = 8;  // ms between frames

enum DripPhase : uint8_t {
    DRIP_IDLE = 0,
    DRIP_PREF_BEGIN,
    DRIP_PREF_ITEMS,
    DRIP_PREF_END,
    DRIP_INFO_BEGIN,
    DRIP_INFO_ITEMS,
    DRIP_INFO_END,
    DRIP_STATUS,
};

static DripPhase s_dripPhase   = DRIP_IDLE;
static uint8_t   s_dripIdx     = 0;
static uint32_t  s_lastDripMs  = 0;
static bool      s_dripInfo    = false;   // include info+status after prefs

static const uint8_t PREF_ORDER[] = {
    LUA_PREF_WIFI_MODE, LUA_PREF_DEV_MODE, LUA_PREF_TELEM_OUT,
    LUA_PREF_MIRROR_BAUD, LUA_PREF_MAP_MODE,
    LUA_PREF_BT_NAME, LUA_PREF_AP_SSID, LUA_PREF_UDP_PORT, LUA_PREF_AP_PASS,
    LUA_PREF_STA_SSID, LUA_PREF_STA_PASS,
    LUA_PREF_ELRS_BIND
};

// Start a drip-feed.  includeInfo=true: prefs + info + status.
//                     includeInfo=false: prefs only.
static void startDrip(bool includeInfo) {
    s_dripPhase = DRIP_PREF_BEGIN;
    s_dripIdx   = 0;
    s_dripInfo  = includeInfo;
    s_lastDripMs = millis() - DRIP_INTERVAL_MS;   // fire first frame immediately
}

// Called each iteration of luaSerialLoop().
static void processDrip() {
    if (s_dripPhase == DRIP_IDLE) return;
    uint32_t now = millis();
    if (now - s_lastDripMs < DRIP_INTERVAL_MS) return;
    s_lastDripMs = now;

    switch (s_dripPhase) {
        case DRIP_PREF_BEGIN:
            sendPrefBegin(LUA_PREF_COUNT);
            s_dripIdx   = 0;
            s_dripPhase = DRIP_PREF_ITEMS;
            break;

        case DRIP_PREF_ITEMS:
            sendPrefItem(PREF_ORDER[s_dripIdx++]);
            if (s_dripIdx >= LUA_PREF_COUNT)
                s_dripPhase = DRIP_PREF_END;
            break;

        case DRIP_PREF_END:
            sendPrefEnd();
            s_dripPhase = s_dripInfo ? DRIP_INFO_BEGIN : DRIP_IDLE;
            s_dripIdx   = 0;
            break;

        case DRIP_INFO_BEGIN:
            sendInfoBegin(LUA_INFO_COUNT);
            s_dripIdx   = LUA_INFO_FIRMWARE;
            s_dripPhase = DRIP_INFO_ITEMS;
            break;

        case DRIP_INFO_ITEMS:
            sendInfoItem(s_dripIdx++);
            if (s_dripIdx > LUA_INFO_WIFI_IP)
                s_dripPhase = DRIP_INFO_END;
            break;

        case DRIP_INFO_END:
            sendInfoEnd();
            s_dripPhase = DRIP_STATUS;
            break;

        case DRIP_STATUS:
            sendStatusFrame();
            s_dripPhase = DRIP_IDLE;
            break;

        default:
            s_dripPhase = DRIP_IDLE;
            break;
    }
}

// Send value-only update for one pref (after cascade or MAP_MODE save).
static void sendPrefUpdate(uint8_t id) {
    uint8_t buf[32];
    uint8_t pos = 0;
    buf[pos++] = id;

    switch (id) {
        case LUA_PREF_WIFI_MODE:
            buf[pos++] = LUA_FT_ENUM;
            buf[pos++] = wifiModeIdx();
            break;
        case LUA_PREF_DEV_MODE:
            buf[pos++] = LUA_FT_ENUM;
            buf[pos++] = (uint8_t)g_config.deviceMode;
            break;
        case LUA_PREF_TELEM_OUT:
            buf[pos++] = LUA_FT_ENUM;
            buf[pos++] = (uint8_t)g_config.telemetryOutput;
            break;
        case LUA_PREF_MIRROR_BAUD:
            buf[pos++] = LUA_FT_ENUM;
            buf[pos++] = (g_config.sportBaud == 115200) ? 1 : 0;
            break;
        case LUA_PREF_MAP_MODE:
            buf[pos++] = LUA_FT_ENUM;
            buf[pos++] = (uint8_t)g_config.trainerMapMode;
            break;
        case LUA_PREF_BT_NAME:
            buf[pos++] = LUA_FT_STRING;
            pos = appendLenStr(buf, pos, g_config.btName);
            break;
        case LUA_PREF_ELRS_BIND:
            buf[pos++] = LUA_FT_STRING;
            pos = appendLenStr(buf, pos, g_config.elrsBindPhrase);
            break;
        default:
            return;
    }

    sendFrame(LUA_CH_PREF, LUA_PT_PREF_UPDATE, buf, pos);
}

// Send ACK and flush (so Lua receives it before any restart).
static void sendPrefAck(uint8_t id, uint8_t result) {
    uint8_t p[2] = { id, result };
    sendFrame(LUA_CH_PREF, LUA_PT_PREF_ACK, p, 2);
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
}

// ── INFO channel senders ─────────────────────────────────────────────

static void sendInfoBegin(uint8_t count) {
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_BEGIN, &count, 1);
}

static void sendInfoEnd() {
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_END, nullptr, 0);
}

static void makeWifiIpText(char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';

    if (s_apMode == 1 || s_apMode == 2) {
        IPAddress ip = WiFi.softAPIP();
        String s = ip.toString();
        if (s.length() > 0 && s != "0.0.0.0") {
            snprintf(out, outSize, "%s (STATIC)", s.c_str());
        } else {
            strlcpy(out, "(none)", outSize);
        }
        return;
    }

    if (s_apMode == 3) {
        if (WiFi.isConnected()) {
            IPAddress ip = WiFi.localIP();
            String s = ip.toString();
            if (s.length() > 0 && s != "0.0.0.0") {
                snprintf(out, outSize, "%s (DHCP)", s.c_str());
            } else {
                strlcpy(out, "(none)", outSize);
            }
        } else {
            strlcpy(out, "(none)", outSize);
        }
        return;
    }

    strlcpy(out, "(none)", outSize);
}

static void sendInfoItem(uint8_t id) {
    uint8_t buf[48];
    uint8_t pos = 0;
    buf[pos++] = id;
    buf[pos++] = LUA_FT_STRING;   // all info items are strings

    const char* label = nullptr;
    char val[32] = {};

    switch (id) {
        case LUA_INFO_FIRMWARE: {
            label = "Firmware";
            // BUILD_TIMESTAMP is "DDMMYYYYHHMM" (12 chars).  Display as "DDMMYYYY HHMM".
            snprintf(val, sizeof(val), "%.8s %.4s",
                     BUILD_TIMESTAMP, BUILD_TIMESTAMP + 8);
            break;
        }
        case LUA_INFO_BT_ADDR: {
            label = "BT Addr";
            if (g_config.deviceMode == DeviceMode::ELRS_HT) {
                strlcpy(val, "N/A", sizeof(val));
            } else {
                const char* addr = bleGetLocalAddress();
                // Fall back to the cached address persisted from the last BLE init.
                // This ensures the field is populated even before BLE finishes starting.
                if (!addr || addr[0] == '\0') addr = g_config.localBtAddr;
                strlcpy(val, (addr && addr[0]) ? addr : "?", sizeof(val));
            }
            break;
        }
        case LUA_INFO_REM_ADDR: {
            label = "Remote Addr";
            if (g_config.deviceMode == DeviceMode::ELRS_HT) {
                strlcpy(val, "N/A", sizeof(val));
            } else {
                strlcpy(val, g_config.hasRemoteAddr ? g_config.remoteBtAddr : "(none)",
                        sizeof(val));
            }
            break;
        }
        case LUA_INFO_WIFI_IP: {
            label = "WiFi IP";
            makeWifiIpText(val, sizeof(val));
            break;
        }
        default: return;
    }

    pos = appendLenStr(buf, pos, label);
    pos = appendLenStr(buf, pos, val);
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_ITEM, buf, pos);
}

static void sendInfoAll() {
    sendInfoBegin(LUA_INFO_COUNT);
    for (uint8_t id = LUA_INFO_FIRMWARE; id <= LUA_INFO_WIFI_IP; id++) {
        sendInfoItem(id);
    }
    sendInfoEnd();
}

// Send value-only update for one info field.
static void sendInfoUpdate(uint8_t id) {
    uint8_t buf[32];
    uint8_t pos = 0;
    buf[pos++] = id;
    buf[pos++] = LUA_FT_STRING;

    char val[32] = {};
    switch (id) {
        case LUA_INFO_FIRMWARE:
            snprintf(val, sizeof(val), "%.8s %.4s",
                     BUILD_TIMESTAMP, BUILD_TIMESTAMP + 8);
            break;
        case LUA_INFO_BT_ADDR:
            if (g_config.deviceMode == DeviceMode::ELRS_HT) {
                strlcpy(val, "N/A", sizeof(val));
            } else {
                const char* addr = bleGetLocalAddress();
                if (!addr || addr[0] == '\0') addr = g_config.localBtAddr;
                strlcpy(val, (addr && addr[0]) ? addr : "?", sizeof(val));
            }
            break;
        case LUA_INFO_REM_ADDR:
            if (g_config.deviceMode == DeviceMode::ELRS_HT) {
                strlcpy(val, "N/A", sizeof(val));
            } else {
                strlcpy(val, g_config.hasRemoteAddr ? g_config.remoteBtAddr : "(none)",
                        sizeof(val));
            }
            break;
        case LUA_INFO_WIFI_IP:
            makeWifiIpText(val, sizeof(val));
            break;
        default: return;
    }

    pos = appendLenStr(buf, pos, val);
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_UPDATE, buf, pos);
}

// ── Periodic TX senders ───────────────────────────────────────────────

// Convert PPM (1050–1950 µs) to signed trainer int16 (-1024..+1024).
static inline int16_t ppmToTrainer(uint16_t ppm) {
    int32_t v = ((int32_t)(ppm - CHANNEL_CENTER) * 1024) / (CHANNEL_RANGE / 2);
    if (v < -1024) v = -1024;
    if (v >  1024) v =  1024;
    return (int16_t)v;
}

static void sendChannelFrame() {
    uint16_t raw[BT_CHANNELS];
    g_channelData.getChannels(raw, BT_CHANNELS);

    uint8_t payload[16];
    for (uint8_t ch = 0; ch < 8; ch++) {
        int16_t val = ppmToTrainer(raw[ch]);
        payload[ch * 2]     = (uint8_t)((val >> 8) & 0xFF);
        payload[ch * 2 + 1] = (uint8_t)(val & 0xFF);
    }
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_CHANNELS, payload, 16);
}

// True when the upstream input source is actively providing data
static bool isSourceConnected() {
    return (g_config.deviceMode == DeviceMode::ELRS_HT)
           ? elrsIsReceiving()
           : bleIsConnected();
}

static void sendStatusFrame() {
    // bit0 = BLE connected,  bit1 = WiFi active (AP running OR STA connected),
    // bit2 = BLE connecting, bit3 = config changes pending restart
    uint8_t wifiActive = 0;
    if (s_apMode == 1 || s_apMode == 2) {
        wifiActive = 0x02;  // AP is up whenever we're in AP mode
    } else if (s_apMode == 3) {
        wifiActive = WiFi.isConnected() ? 0x02 : 0x00;
    }
    // bit0: connected (BLE or ELRS receiving)
    bool connected = isSourceConnected();
    uint8_t status = (connected           ? 0x01 : 0x00)
                   | wifiActive
                   | (bleIsConnecting()   ? 0x04 : 0x00)
                   | (s_restartPending    ? 0x08 : 0x00);
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_STATUS, &status, 1);
}

static void sendScanStatusFrame(uint8_t state, uint8_t count) {
    uint8_t p[2] = { state, count };
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_SCAN_STATUS, p, 2);
}

static void sendScanEntryFrame(uint8_t index) {
    if (index >= s_scanSendTotal) return;
    const BleScanResult& dev = s_scanCache[index];

    uint8_t buf[64];
    uint8_t pos = 0;

    buf[pos++] = index;
    buf[pos++] = (uint8_t)(int8_t)dev.rssi;
    buf[pos++] = dev.hasFrskyService ? 0x01 : 0x00;

    uint8_t nlen = (uint8_t)strlen(dev.name);
    buf[pos++] = nlen;
    memcpy(&buf[pos], dev.name, nlen);
    pos += nlen;

    // addr: 17 bytes, null-padded
    size_t alen = strlen(dev.address);
    for (uint8_t i = 0; i < 17; i++) {
        buf[pos++] = (i < alen) ? (uint8_t)dev.address[i] : 0;
    }

    sendFrame(LUA_CH_INFO, LUA_PT_INFO_SCAN_ITEM, buf, pos);
}

static void wifiScanTask(void*) {
    int16_t n       = WiFi.scanNetworks(/*async=*/false);
    LOG_I("LUA", "WiFi scan complete: %d networks", n);
    s_wifiScanCount = (n < 0) ? 0 : n;
    s_wifiScanDone  = true;
    vTaskDelete(nullptr);
}

static void sendWifiScanStatusFrame(uint8_t state, uint8_t count) {
    uint8_t p[2] = { state, count };
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_WIFI_SCAN_STATUS, p, 2);
}

static void sendWifiScanItemFrame(uint8_t index) {
    String  ssid  = WiFi.SSID(index);
    int8_t  rssi  = (int8_t)WiFi.RSSI(index);
    uint8_t slen  = (uint8_t)std::min(ssid.length(), (size_t)32);
    uint8_t buf[36];
    uint8_t pos = 0;
    buf[pos++] = index;
    buf[pos++] = (uint8_t)rssi;
    buf[pos++] = slen;
    for (uint8_t i = 0; i < slen; i++) buf[pos++] = (uint8_t)ssid[i];
    sendFrame(LUA_CH_INFO, LUA_PT_INFO_WIFI_SCAN_ITEM, buf, pos);
}

// ── TRANS handler (CH_TRANS, PT_TRANS_SPORT) ─────────────────────────
// payload: physId(1) primId(1) dataId_lo(1) dataId_hi(1) value(4 LE) = 8 bytes

static void handleTransFrame(const uint8_t* payload, uint8_t len) {
    if (len < 8) return;
    if (g_config.telemetryOutput == TelemetryOutput::NONE) return;

    if (!s_tlmOutputActive) {
        sportOutputInit();
        s_tlmOutputActive = true;
    }

    SportPacket pkt;
    pkt.physId = payload[0];
    pkt.primId = payload[1];
    pkt.dataId = payload[2] | ((uint16_t)payload[3] << 8);
    pkt.value  = payload[4] | ((uint32_t)payload[5] << 8)
               | ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);

    sportOutputForwardPacket(&pkt);
}

// ── PREF_SET handler ─────────────────────────────────────────────────

static void handlePrefSet(const uint8_t* payload, uint8_t len) {
    if (len < 2) { sendPrefAck(0, 0x01); return; }

    uint8_t id    = payload[0];
    // uint8_t ftype = payload[1];  // informational, not used for routing
    uint8_t pos   = 2;

    switch (id) {
        case LUA_PREF_WIFI_MODE: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t newIdx = payload[pos];
            if (newIdx > 2)  { sendPrefAck(id, 0x01); return; }
            bool telemChanged = false;

            // Cascade: switching away from WiFi mode (Off) clears WiFi telem output
            if (newIdx == 0 && g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
                g_config.telemetryOutput = TelemetryOutput::NONE;
                telemChanged = true;
                configSave();
            }
            // Cascade: AP/STA mode can't coexist with BLE telem
            if (newIdx != 0 && g_config.telemetryOutput == TelemetryOutput::BLE) {
                g_config.telemetryOutput = TelemetryOutput::NONE;
                telemChanged = true;
                configSave();
            }

            sendPrefAck(id, 0x00);
            g_config.wifiMode = static_cast<WifiMode>(newIdx);
            configSave();
            sendPrefUpdate(LUA_PREF_WIFI_MODE);
            if (telemChanged) {
                sendPrefUpdate(LUA_PREF_TELEM_OUT);
            }
            s_restartPending = true;
            break;
        }

        case LUA_PREF_DEV_MODE: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t newIdx = payload[pos];
            if (newIdx > 3) { sendPrefAck(id, 0x01); return; }

            // Cascade: non-Telemetry modes clear telemetry output
            if (newIdx != 2 && g_config.telemetryOutput != TelemetryOutput::NONE) {
                g_config.telemetryOutput = TelemetryOutput::NONE;
                configSave();
                sendPrefUpdate(LUA_PREF_TELEM_OUT);
            }
            // Cascade: ELRS_HT forces WiFi off and Trainer Map to GV
            if (newIdx == 3) {
                if (g_config.wifiMode != WifiMode::OFF) {
                    g_config.wifiMode = WifiMode::OFF;
                    configSave();
                    sendPrefUpdate(LUA_PREF_WIFI_MODE);
                }
                if (g_config.trainerMapMode != TrainerMapMode::MAP_GV) {
                    g_config.trainerMapMode = TrainerMapMode::MAP_GV;
                    configSave();
                    sendPrefUpdate(LUA_PREF_MAP_MODE);
                }
            }
            g_config.deviceMode = static_cast<DeviceMode>(newIdx);
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_DEV_MODE);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_TELEM_OUT: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t newIdx = payload[pos];
            if (newIdx > 2) { sendPrefAck(id, 0x01); return; }
            g_config.telemetryOutput = static_cast<TelemetryOutput>(newIdx);
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_TELEM_OUT);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_MIRROR_BAUD: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint32_t baud = (payload[pos] == 1) ? 115200 : 57600;
            g_config.sportBaud = baud;
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_MIRROR_BAUD);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_MAP_MODE: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            g_config.trainerMapMode = (payload[pos] == 0)
                                      ? TrainerMapMode::MAP_GV
                                      : TrainerMapMode::MAP_TR;
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_MAP_MODE);
            break;
        }

        case LUA_PREF_BT_NAME: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            if (vlen == 0 || pos + vlen > len) { sendPrefAck(id, 0x01); return; }
            char str[16] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 15));
            strlcpy(g_config.btName, str, sizeof(g_config.btName));
            configSave();
            bleUpdateAdvertisingName();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_BT_NAME);
            sendInfoUpdate(LUA_INFO_BT_ADDR);  // BT name change may affect discovery
            break;
        }

        case LUA_PREF_AP_SSID: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            if (vlen == 0 || pos + vlen > len) { sendPrefAck(id, 0x01); return; }
            char str[16] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 15));
            strlcpy(g_config.apSsid, str, sizeof(g_config.apSsid));
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_AP_SSID);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_UDP_PORT: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            char str[6] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 5));
            uint16_t port = (uint16_t)atoi(str);
            if (port < 1024 || port > 65535) { sendPrefAck(id, 0x01); return; }
            g_config.udpPort = port;
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_UDP_PORT);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_AP_PASS: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            if (vlen < 8 || pos + vlen > len) { sendPrefAck(id, 0x01); return; }
            char str[16] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 15));
            strlcpy(g_config.apPass, str, sizeof(g_config.apPass));
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_AP_PASS);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_STA_SSID: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            if (vlen == 0 || pos + vlen > len) { sendPrefAck(id, 0x01); return; }
            char str[32] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 31));
            strlcpy(g_config.staSsid, str, sizeof(g_config.staSsid));
            configSave();
            sendPrefAck(id, 0x00);
            // No restart — password editor will follow; restart on STA_PASS save.
            break;
        }

        case LUA_PREF_STA_PASS: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            if (pos + vlen > len) { sendPrefAck(id, 0x01); return; }
            char str[64] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 63));
            strlcpy(g_config.staPass, str, sizeof(g_config.staPass));
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_STA_PASS);
            s_restartPending = true;
            break;
        }

        case LUA_PREF_ELRS_BIND: {
            if (pos >= len) { sendPrefAck(id, 0x01); return; }
            uint8_t vlen = payload[pos++];
            if (vlen == 0 || pos + vlen > len) { sendPrefAck(id, 0x01); return; }
            char str[32] = {};
            memcpy(str, &payload[pos], std::min((int)vlen, 31));
            strlcpy(g_config.elrsBindPhrase, str, sizeof(g_config.elrsBindPhrase));
            configSave();
            sendPrefAck(id, 0x00);
            sendPrefUpdate(LUA_PREF_ELRS_BIND);
            s_restartPending = true;
            break;
        }

        default:
            LOG_W("LUA", "PREF_SET: unknown pref id 0x%02X", id);
            sendPrefAck(id, 0x01);
            break;
    }
}

// ── Frame dispatcher (after CRC validation) ──────────────────────────

static void dispatchRxFrame(uint8_t ch, uint8_t typ, const uint8_t* payload, uint8_t len) {
    if (ch == LUA_CH_PREF) {
        if (typ == LUA_PT_PREF_REQUEST) {
            LOG_I("LUA", "PREF_REQUEST");
            startDrip(false);
        } else if (typ == LUA_PT_PREF_SET) {
            handlePrefSet(payload, len);
        }

    } else if (ch == LUA_CH_INFO) {
        if (typ == LUA_PT_INFO_REQUEST) {
            LOG_I("LUA", "INFO_REQUEST");
            startDrip(true);
            s_lastCfgMs = millis();

        } else if (typ == LUA_PT_INFO_HEARTBEAT) {
            // s_lastToolsCmdMs already bumped by processRxByte

        } else if (typ == LUA_PT_INFO_BLE_SCAN) {
            LOG_I("LUA", "BLE scan start");
            if (bleScanStart()) {
                sendScanStatusFrame(1, 0);
            } else {
                sendScanStatusFrame(0, 0);
            }

        } else if (typ == LUA_PT_INFO_BLE_CONNECT && len >= 1) {
            uint8_t idx = payload[0];
            if (idx < s_scanSendTotal) {
                LOG_I("LUA", "BLE connect to scan[%u]", idx);
                bleConnectTo(s_scanCache[idx].address);
            } else {
                LOG_W("LUA", "BLE connect: scan index %u out of range", idx);
            }

        } else if (typ == LUA_PT_INFO_BLE_DISCONNECT) {
            LOG_I("LUA", "BLE disconnect");
            bleDisconnect();

        } else if (typ == LUA_PT_INFO_BLE_FORGET) {
            LOG_I("LUA", "BLE forget");
            bleForget();
            sendInfoUpdate(LUA_INFO_REM_ADDR);

        } else if (typ == LUA_PT_INFO_BLE_RECONNECT) {
            LOG_I("LUA", "BLE reconnect");
            if (g_config.hasRemoteAddr) {
                bleConnectTo(g_config.remoteBtAddr);
            }

        } else if (typ == LUA_PT_INFO_WIFI_SCAN) {
            LOG_I("LUA", "WiFi scan start");
            if (s_apMode != 0 && !s_wifiScanActive) {
                s_wifiScanActive  = true;
                s_wifiScanDone    = false;
                s_wifiScanCount   = 0;
                s_wifiScanSendIdx = 0;
                xTaskCreate(wifiScanTask, "wifiScan", 8192, nullptr, 1, nullptr);
                sendWifiScanStatusFrame(1, 0);
            } else if (s_apMode == 0) {
                LOG_W("LUA", "WiFi scan: not in WiFi mode (apMode=%u)", s_apMode);
                sendWifiScanStatusFrame(0, 0);
            }
            // else: scan already in progress, ignore duplicate request

        } else if (typ == LUA_PT_INFO_RESTART) {
            LOG_I("LUA", "Restart requested by tools");
            s_restartPending = false;
            mainRequestConfigRestart();
        }

    } else if (ch == LUA_CH_TRANS) {
        if (typ == LUA_PT_TRANS_SPORT) {
            handleTransFrame(payload, len);
        }
    }
}

// ── RX state machine ─────────────────────────────────────────────────

static void processRxByte(uint8_t b) {
    switch (s_rxState) {
        case 0:  // wait for sync
            if (b == LUA_SYNC) s_rxState = 1;
            break;

        case 1:  // CH
            s_rxCh     = b;
            s_rxCrcAcc = b;
            s_rxState  = 2;
            break;

        case 2:  // TYPE
            s_rxType    = b;
            s_rxCrcAcc ^= b;
            s_rxState   = 3;
            break;

        case 3:  // LEN
            s_rxLen    = b;
            s_rxCrcAcc ^= b;
            s_rxPos    = 0;
            if (s_rxLen > sizeof(s_rxBuf)) {
                LOG_W("LUA", "RX oversized LEN=%u (max %u), frame dropped",
                      s_rxLen, (unsigned)sizeof(s_rxBuf));
                s_rxState = 0;
            } else {
                s_rxState = (s_rxLen == 0) ? 5 : 4;
            }
            break;

        case 4:  // accumulate payload
            if (s_rxPos < sizeof(s_rxBuf)) {
                s_rxBuf[s_rxPos] = b;
            }
            s_rxPos++;
            s_rxCrcAcc ^= b;
            if (s_rxPos >= s_rxLen) s_rxState = 5;
            break;

        case 5:  // CRC byte
            if (b == s_rxCrcAcc) {
                s_lastToolsCmdMs = millis();
                dispatchRxFrame(s_rxCh, s_rxType, s_rxBuf,
                                std::min(s_rxLen, (uint8_t)sizeof(s_rxBuf)));
            } else {
                LOG_W("LUA", "CRC error ch=0x%02X typ=0x%02X", s_rxCh, s_rxType);
            }
            s_rxState = 0;
            break;

        default:
            s_rxState = 0;
            break;
    }
}

static void readIncoming() {
    uint8_t tmp[64];
    int n = uart_read_bytes(UART_NUM_1, tmp, sizeof(tmp), 0);
    for (int i = 0; i < n; i++) {
        processRxByte(tmp[i]);
    }
}

// ── Public API ───────────────────────────────────────────────────────

void luaSerialSetApMode(uint8_t mode) {
    s_apMode = mode;
}

void luaSerialInit() {
    uart_config_t cfg = {
        .baud_rate  = (int)LUA_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, PIN_SERIAL_TX, PIN_SERIAL_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 256, 0, NULL, 0));

    s_running          = true;
    s_rxState          = 0;
    s_lastChMs         = 0;
    s_lastStatusMs     = 0;
    s_apMode           = 0;
    s_lastCfgMs        = millis() - LUA_CFG_INTERVAL;  // fire on first loop
    s_scanWasActive    = false;
    s_scanSending      = false;
    s_scanSendIdx      = 0;
    s_scanSendTotal    = 0;
    s_lastScanEntryMs  = 0;
    s_wifiScanDone     = false;
    s_wifiScanCount    = 0;
    s_wifiScanActive   = false;
    s_wifiScanSending  = false;
    s_wifiScanSendIdx  = 0;
    s_lastWifiScanMs   = 0;
    s_lastBleConnected = bleIsConnected();
    s_lastToolsCmdMs   = 0;
    s_tlmOutputActive  = false;
    s_dripPhase        = DRIP_IDLE;
    s_dripIdx          = 0;

    LOG_I("LUA", "Init: TX=%d RX=%d @ %lu baud (new CH/TYPE/LEN protocol)",
          PIN_SERIAL_TX, PIN_SERIAL_RX, LUA_BAUD);
}

void luaSerialLoop() {
    if (!s_running) return;

    readIncoming();

    uint32_t now = millis();

    // Channel frame — 50 Hz for all modes (Lua scripts consume at 20-30 Hz)
    if (now - s_lastChMs >= LUA_CH_FRAME_INTERVAL) {
        s_lastChMs = now;
        bool active = isSourceConnected() && !bleIsScanning();
        if (active && !g_channelData.isStale(500)) {
            sendChannelFrame();
        }
    }

    // Status frame every 500 ms
    if (now - s_lastStatusMs >= LUA_STATUS_INTERVAL) {
        s_lastStatusMs = now;
        bool connected = isSourceConnected();
        sendStatusFrame();
        // Connection state changed → push updated info immediately
        if (connected != s_lastBleConnected) {
            if (g_config.deviceMode == DeviceMode::ELRS_HT ||
                !connected || !bleIsConnecting()) {
                s_lastBleConnected = connected;
                sendInfoUpdate(LUA_INFO_REM_ADDR);
            }
        }

        char wifiIp[32] = {};
        makeWifiIpText(wifiIp, sizeof(wifiIp));
        if (strncmp(wifiIp, s_lastWifiIp, sizeof(s_lastWifiIp)) != 0) {
            strlcpy(s_lastWifiIp, wifiIp, sizeof(s_lastWifiIp));
            sendInfoUpdate(LUA_INFO_WIFI_IP);
        }
    }

    // Periodic full resync — only while Tools script is active
    if ((now - s_lastCfgMs >= LUA_CFG_INTERVAL) &&
        (now - s_lastToolsCmdMs < LUA_TOOLS_IDLE_TIMEOUT)) {
        s_lastCfgMs = now;
        startDrip(true);
    }

    // Process pref/info drip-feed
    processDrip();

    // BLE scan state tracking + drip-feed
    bool scanning = bleIsScanning();
    if (s_scanWasActive && !scanning) {
        s_scanSendTotal = bleGetScanResults(s_scanCache, MAX_SCAN_RESULTS);
        sendScanStatusFrame(2, s_scanSendTotal);
        s_scanSendIdx = 0;
        s_scanSending = (s_scanSendTotal > 0);
    }
    s_scanWasActive = scanning;

    if (s_scanSending && s_scanSendIdx < s_scanSendTotal) {
        if (now - s_lastScanEntryMs >= 20) {
            s_lastScanEntryMs = now;
            sendScanEntryFrame(s_scanSendIdx++);
            if (s_scanSendIdx >= s_scanSendTotal) s_scanSending = false;
        }
    }

    // WiFi scan task completion + drip-feed (items sent before status=2)
    if (s_wifiScanDone && !s_wifiScanSending) {
        uint8_t total = (uint8_t)std::min((int16_t)s_wifiScanCount, (int16_t)20);
        s_wifiScanDone    = false;
        s_wifiScanSendIdx = 0;
        if (total > 0) {
            s_wifiScanSending = true;
            s_lastWifiScanMs  = now - 30;  // trigger first send immediately
        } else {
            sendWifiScanStatusFrame(2, 0);
            WiFi.scanDelete();
            s_wifiScanActive = false;
        }
    }
    if (s_wifiScanSending) {
        uint8_t total = (uint8_t)std::min((int16_t)s_wifiScanCount, (int16_t)20);
        if (now - s_lastWifiScanMs >= 20) {
            s_lastWifiScanMs = now;
            sendWifiScanItemFrame(s_wifiScanSendIdx);
            s_wifiScanSendIdx++;
            if (s_wifiScanSendIdx >= total) {
                s_wifiScanSending = false;
                s_wifiScanActive  = false;
                sendWifiScanStatusFrame(2, total);
                WiFi.scanDelete();
            }
        }
    }
}

void luaSerialStop() {
    if (s_running) {
        uart_driver_delete(UART_NUM_1);
        s_running = false;
        if (s_tlmOutputActive) {
            sportOutputStop();
            s_tlmOutputActive = false;
        }
        LOG_I("LUA", "Stopped");
    }
}

