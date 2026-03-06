/**
 * @file sport_telemetry.cpp
 * @brief S.PORT telemetry receiver and forwarder
 *
 * Two input modes:
 * 1) BT-framed (SPORT_BT): EdgeTX bluetooth.cpp sends re-framed S.PORT:
 *    [0x7E] [8 data bytes, byte-stuffed] [XOR CRC, byte-stuffed] [0x7E]
 *    at 115200 8N1. Packets batched 2 per write. AT commands handled by
 *    the same AT state machine from frsky_serial.cpp (shared byte stream).
 *
 * 2) Raw mirror (SPORT_MIRROR): EdgeTX AUX2 Telemetry Mirror sends raw
 *    S.PORT bytes at 57600 (or 115200 on TX16S/F16):
 *    [0x7E] [physID] [primID] [dataID_lo] [dataID_hi] [val0-3] [CRC]
 *    Includes all polling frames; we filter for primID == 0x10 (DATA_FRAME).
 *
 * Two output modes:
 * a) WiFi UDP: ESP32 starts its own AP and broadcasts parsed S.PORT
 *    packets as UDP datagrams to connected clients.
 * b) BLE: Forward re-framed S.PORT packets as BLE notifications
 *    on the FrSky characteristic (0xFFF6).
 */

#include "sport_telemetry.h"
#include "config.h"
#include "ble_module.h"
#include "log.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>

// ═══════════════════════════════════════════════════════════════════
// PROTOCOL CONSTANTS
// ═══════════════════════════════════════════════════════════════════

static constexpr uint8_t  START_STOP    = 0x7E;
static constexpr uint8_t  BYTE_STUFF    = 0x7D;
static constexpr uint8_t  STUFF_MASK    = 0x20;
static constexpr uint8_t  SPORT_FRAME_LEN = 9;   // 8 data + 1 CRC
static constexpr uint8_t  BT_FRAME_LEN   = 9;    // 8 data + 1 XOR CRC (inside 0x7E framing)

// AT detection (for BT-framed mode — AT commands on same UART)
static constexpr uint8_t  AT_BUF_SIZE = 64;

// ═══════════════════════════════════════════════════════════════════
// UART & STATE
// ═══════════════════════════════════════════════════════════════════

static HardwareSerial* s_serial = nullptr;
static bool s_running = false;

// RX frame buffer (de-stuffed)
static uint8_t s_rxBuf[SPORT_FRAME_LEN + 2];
static uint8_t s_rxIdx = 0;

enum RxState : uint8_t { RX_IDLE, RX_START, RX_IN_FRAME, RX_XOR };
static RxState s_rxState = RX_IDLE;

// AT command handler state (BT-framed mode reuses AT handler)
static char    s_atBuf[AT_BUF_SIZE];
static uint8_t s_atLen    = 0;
static bool    s_atMode   = false;
static char    s_prevChar = 0;

// ═══════════════════════════════════════════════════════════════════
// OUTPUT: WiFi UDP
// ═══════════════════════════════════════════════════════════════════

static WiFiUDP  s_udp;
static bool     s_udpActive   = false;
static bool     s_wifiStarted = false;

// ═══════════════════════════════════════════════════════════════════
// STATS
// ═══════════════════════════════════════════════════════════════════

static uint32_t s_packetCount  = 0;
static uint32_t s_ppsCount     = 0;     // counter for current second
static uint32_t s_ppsValue     = 0;     // last completed second's count
static uint32_t s_lastPpsSec   = 0;

// ═══════════════════════════════════════════════════════════════════
// AT COMMAND HANDLER (reused from frsky_serial for BT telemetry)
// ═══════════════════════════════════════════════════════════════════

// Simplified AT responses — telemetry mode uses AT+ROLE0 (peripheral)
static void atReply(const char* msg) {
    if (!s_serial) return;
    s_serial->print(msg);
    s_serial->print("\r\n");
    LOG_D("AT-T", ">> %s", msg);
}

/**
 * @brief Minimal AT command handler for telemetry mode.
 * Same command set as frsky_serial but stripped to essentials.
 */
static void processAtCommand(const char* cmd) {
    LOG_I("AT-T", "<< AT%s", cmd);

    if (strncmp(cmd, "+BAUD", 5) == 0) {
        char reply[24];
        snprintf(reply, sizeof(reply), "OK+Set:%c", cmd[5] ? cmd[5] : '4');
        atReply(reply);
    }
    else if (strncmp(cmd, "+NAME", 5) == 0) {
        const char* name = cmd + 5;
        if (name[0]) {
            strlcpy(g_config.btName, name, sizeof(g_config.btName));
            configSave();
            bleUpdateAdvertisingName();
        }
        char reply[48];
        snprintf(reply, sizeof(reply), "OK+Name:%s", g_config.btName);
        atReply(reply);
    }
    else if (strncmp(cmd, "+TXPW", 5) == 0) {
        char reply[16];
        snprintf(reply, sizeof(reply), "OK+Txpw:%c", cmd[5] ? cmd[5] : '0');
        atReply(reply);
    }
    else if (strncmp(cmd, "+ROLE", 5) == 0) {
        const char* localAddr = bleGetLocalAddress();
        if (!localAddr || !localAddr[0]) localAddr = "00:00:00:00:00:00";
        // Telemetry mode always responds as peripheral
        atReply("OK+Role:0");
        char addr[32];
        snprintf(addr, sizeof(addr), "Peripheral:%s", localAddr);
        atReply(addr);
    }
    else if (strcmp(cmd, "+CLEAR") == 0) {
        atReply("OK+CLEAR");
    }
    else {
        LOG_W("AT-T", "Unknown: AT%s", cmd);
        atReply("OK");
    }
}

// ═══════════════════════════════════════════════════════════════════
// S.PORT FRAME PROCESSING
// ═══════════════════════════════════════════════════════════════════

/**
 * @brief Forward a parsed S.PORT packet to the selected output.
 */
static void forwardPacket(const SportPacket* pkt) {
    s_packetCount++;
    s_ppsCount++;

    if (g_config.telemetryOutput == TelemetryOutput::WIFI_UDP && s_udpActive) {
        // Send raw 8-byte packet as UDP datagram
        s_udp.beginPacket(IPAddress(255, 255, 255, 255), g_config.udpPort);
        s_udp.write((const uint8_t*)pkt, sizeof(SportPacket));
        s_udp.endPacket();
    }
    else if (g_config.telemetryOutput == TelemetryOutput::BLE) {
        // Forward as BLE notification — build framed packet
        // Frame: [0x7E] [8 data bytes with byte-stuffing] [XOR CRC] [0x7E]
        uint8_t frame[24];  // max: 2 + 8*2 + 2 + 2 = 22
        uint8_t fi = 0;
        uint8_t crc = 0;

        frame[fi++] = START_STOP;
        const uint8_t* raw = (const uint8_t*)pkt;
        for (uint8_t i = 0; i < 8; i++) {
            crc ^= raw[i];
            if (raw[i] == START_STOP || raw[i] == BYTE_STUFF) {
                frame[fi++] = BYTE_STUFF;
                frame[fi++] = raw[i] ^ STUFF_MASK;
            } else {
                frame[fi++] = raw[i];
            }
        }
        // Append CRC (byte-stuffed)
        if (crc == START_STOP || crc == BYTE_STUFF) {
            frame[fi++] = BYTE_STUFF;
            frame[fi++] = crc ^ STUFF_MASK;
        } else {
            frame[fi++] = crc;
        }
        frame[fi++] = START_STOP;

        // Use BLE module to send notification
        // This requires the peripheral to be initialized and a client connected
        extern void bleSendRawNotification(const uint8_t* data, size_t len);
        bleSendRawNotification(frame, fi);
    }
}

// ═══════════════════════════════════════════════════════════════════
// BT-FRAMED RX DECODER (0x7E framing, XOR CRC)
// ═══════════════════════════════════════════════════════════════════

/**
 * @brief Process a completed BT-framed S.PORT frame.
 * Frame payload (de-stuffed): [physID] [primID] [dataID_lo] [dataID_hi]
 *                              [val0] [val1] [val2] [val3] [XOR_CRC]
 */
static void processBtFrame() {
    if (s_rxIdx < BT_FRAME_LEN) return;  // need 9 bytes (8 data + 1 CRC)

    // Verify XOR CRC: XOR of first 8 bytes should equal byte 8
    uint8_t crc = 0;
    for (uint8_t i = 0; i < 8; i++) crc ^= s_rxBuf[i];
    if (crc != s_rxBuf[8]) {
        LOG_D("SPORT", "BT frame CRC fail: calc=0x%02X got=0x%02X", crc, s_rxBuf[8]);
        return;
    }

    // Check for DATA_FRAME
    if (s_rxBuf[1] != SPORT_DATA_FRAME) return;

    SportPacket pkt;
    pkt.physId = s_rxBuf[0];
    pkt.primId = s_rxBuf[1];
    pkt.dataId = s_rxBuf[2] | (s_rxBuf[3] << 8);
    pkt.value  = s_rxBuf[4] | (s_rxBuf[5] << 8) |
                 ((uint32_t)s_rxBuf[6] << 16) | ((uint32_t)s_rxBuf[7] << 24);

    forwardPacket(&pkt);
}

/**
 * @brief Feed one byte into the BT-framed state machine.
 * Identical framing to FrSky trainer (0x7E start/stop, byte-stuffing).
 */
static void btRxProcessByte(uint8_t data) {
    switch (s_rxState) {
        case RX_START:
            if (data == START_STOP) {
                s_rxState = RX_IN_FRAME;
                s_rxIdx = 0;
            } else {
                if (s_rxIdx < sizeof(s_rxBuf)) s_rxBuf[s_rxIdx++] = data;
            }
            break;

        case RX_IN_FRAME:
            if (data == BYTE_STUFF) {
                s_rxState = RX_XOR;
            } else if (data == START_STOP) {
                processBtFrame();
                s_rxIdx = 0;
                s_rxState = RX_IN_FRAME;
            } else {
                if (s_rxIdx < sizeof(s_rxBuf)) s_rxBuf[s_rxIdx++] = data;
            }
            break;

        case RX_XOR:
            if (s_rxIdx < sizeof(s_rxBuf)) s_rxBuf[s_rxIdx++] = data ^ STUFF_MASK;
            s_rxState = RX_IN_FRAME;
            break;

        case RX_IDLE:
        default:
            if (data == START_STOP) {
                s_rxIdx = 0;
                s_rxState = RX_START;
            }
            break;
    }
}

/**
 * @brief BT-framed mode: route bytes through AT detection then S.PORT decoder.
 */
static void btRxProcessByteWithAT(uint8_t c) {
    if (s_atMode) {
        if (c == '\r' || c == '\n') {
            if (s_atLen > 0) {
                s_atBuf[s_atLen] = '\0';
                processAtCommand(s_atBuf);
                s_atLen = 0;
            }
            s_atMode = false;
        } else {
            if (s_atLen < AT_BUF_SIZE - 1)
                s_atBuf[s_atLen++] = c;
        }
    } else {
        if (s_prevChar == 'A' && c == 'T') {
            s_atMode = true;
            s_atLen  = 0;
        } else {
            btRxProcessByte(c);
        }
    }
    s_prevChar = c;
}

// ═══════════════════════════════════════════════════════════════════
// RAW MIRROR RX DECODER (native S.PORT)
// ═══════════════════════════════════════════════════════════════════

/**
 * @brief Process a completed raw S.PORT frame.
 * Raw S.PORT: [0x7E] [physID] [primID] [dataID_lo] [dataID_hi]
 *             [val0] [val1] [val2] [val3] [additive CRC]
 * After 0x7E, we buffer 9 bytes (physID through CRC).
 */
static void processRawFrame() {
    if (s_rxIdx < SPORT_FRAME_LEN) return;

    // Additive CRC: sum of bytes 1..8 (primID through val3), result + CRC = 0xFF
    // But we stored physID at [0], primID at [1]... CRC at [8]
    // S.PORT CRC is over bytes 1-7 (primID, dataID, value), byte [8] is CRC
    // Actually: checkSportPacket sums bytes [1..8] and checks == 0xFF
    uint16_t sum = 0;
    for (uint8_t i = 1; i < SPORT_FRAME_LEN; i++) {
        sum += s_rxBuf[i];
        sum += sum >> 8;
        sum &= 0xFF;
    }
    if (sum != 0xFF) {
        return;  // CRC failure — ignore
    }

    // Filter for DATA_FRAME only
    if (s_rxBuf[1] != SPORT_DATA_FRAME) return;

    SportPacket pkt;
    pkt.physId = s_rxBuf[0];
    pkt.primId = s_rxBuf[1];
    pkt.dataId = s_rxBuf[2] | (s_rxBuf[3] << 8);
    pkt.value  = s_rxBuf[4] | (s_rxBuf[5] << 8) |
                 ((uint32_t)s_rxBuf[6] << 16) | ((uint32_t)s_rxBuf[7] << 24);

    forwardPacket(&pkt);
}

/**
 * @brief Feed a byte into the raw S.PORT mirror state machine.
 *
 * Raw S.PORT stream: 0x7E marks start of each poll cycle.
 * After 0x7E, physID byte, then 8 response bytes (if sensor responds).
 * Byte-stuffing also applies in native S.PORT.
 */
static void rawRxProcessByte(uint8_t data) {
    switch (s_rxState) {
        case RX_IDLE:
            if (data == START_STOP) {
                s_rxIdx = 0;
                s_rxState = RX_START;
            }
            break;

        case RX_START:
            if (data == START_STOP) {
                // Double 0x7E — re-sync
                s_rxIdx = 0;
            } else {
                // First byte after 0x7E = physID
                s_rxBuf[s_rxIdx++] = data;
                s_rxState = RX_IN_FRAME;
            }
            break;

        case RX_IN_FRAME:
            if (data == START_STOP) {
                // End of frame / start of next
                processRawFrame();
                s_rxIdx = 0;
                s_rxState = RX_START;
            } else if (data == BYTE_STUFF) {
                s_rxState = RX_XOR;
            } else {
                if (s_rxIdx < sizeof(s_rxBuf)) s_rxBuf[s_rxIdx++] = data;
            }
            break;

        case RX_XOR:
            if (s_rxIdx < sizeof(s_rxBuf)) s_rxBuf[s_rxIdx++] = data ^ STUFF_MASK;
            s_rxState = RX_IN_FRAME;
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════
// WiFi AP + UDP MANAGEMENT
// ═══════════════════════════════════════════════════════════════════

static const char* SPORT_AP_SSID = "BTWifiSerial";
static const char* SPORT_AP_PASS = "12345678";

static void startWiFiAP() {
    if (s_wifiStarted) return;

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(100);

    if (!WiFi.softAP(SPORT_AP_SSID, SPORT_AP_PASS, 1, 0, 4)) {
        LOG_E("SPORT", "softAP() failed!");
        WiFi.mode(WIFI_OFF);
        return;
    }

    // Wait until the AP interface has a valid IP
    {
        uint32_t t0 = millis();
        while (WiFi.softAPIP() == IPAddress(0, 0, 0, 0) && millis() - t0 < 3000) {
            delay(50);
        }
    }
    delay(200);
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_udp.begin(g_config.udpPort);
    s_udpActive  = true;
    s_wifiStarted = true;

    LOG_I("SPORT", "AP started: SSID=%s IP=%s UDP port=%u",
          SPORT_AP_SSID, WiFi.softAPIP().toString().c_str(), g_config.udpPort);
}

static void stopWiFiAP() {
    if (s_udpActive) {
        s_udp.stop();
        s_udpActive = false;
    }
    if (s_wifiStarted) {
        WiFi.softAPdisconnect(true);
        delay(50);
        WiFi.mode(WIFI_OFF);
        s_wifiStarted = false;
    }
}

// ═══════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void sportTelemetryInit() {
    s_serial = &Serial1;

    uint32_t baud;
    if (g_config.serialMode == OutputMode::SPORT_BT) {
        baud = 115200;  // BT mode always 115200
    } else {
        baud = g_config.sportBaud;  // Mirror: user-configurable (57600 or 115200)
    }

    s_serial->begin(baud, SERIAL_8N1, PIN_SERIAL_RX, PIN_SERIAL_TX);

    s_running  = true;
    s_rxState  = RX_IDLE;
    s_rxIdx    = 0;
    s_atMode   = false;
    s_atLen    = 0;
    s_prevChar = 0;

    s_packetCount = 0;
    s_ppsCount    = 0;
    s_ppsValue    = 0;
    s_lastPpsSec  = millis() / 1000;

    // Start appropriate output
    if (g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
        startWiFiAP();
    }
    // BLE output: BLE init is handled by main.cpp (bleInit)

    const char* inputMode = (g_config.serialMode == OutputMode::SPORT_BT)
                            ? "BT-framed" : "Raw-mirror";
    const char* outputMode = (g_config.telemetryOutput == TelemetryOutput::WIFI_UDP)
                             ? "WiFi-UDP" : "BLE";
    LOG_I("SPORT", "Telemetry init: input=%s @ %lu baud, output=%s",
          inputMode, baud, outputMode);
}

void sportTelemetryLoop() {
    if (!s_running) return;

    // Read incoming UART data
    while (s_serial->available()) {
        uint8_t c = s_serial->read();
        if (g_config.serialMode == OutputMode::SPORT_BT) {
            btRxProcessByteWithAT(c);
        } else {
            rawRxProcessByte(c);
        }
    }

    // WiFi AP doesn't need polling — it's always available

    // Update packets-per-second counter
    uint32_t nowSec = millis() / 1000;
    if (nowSec != s_lastPpsSec) {
        s_ppsValue  = s_ppsCount;
        s_ppsCount  = 0;
        s_lastPpsSec = nowSec;
    }
}

void sportTelemetryStop() {
    if (s_serial) {
        s_serial->end();
    }
    stopWiFiAP();
    s_running = false;
    LOG_I("SPORT", "Telemetry stopped");
}

bool sportUdpIsActive() {
    return s_udpActive;
}

bool sportBleIsForwarding() {
    return s_running &&
           g_config.telemetryOutput == TelemetryOutput::BLE &&
           bleIsConnected();
}

uint32_t sportGetPacketCount() {
    return s_packetCount;
}

uint32_t sportGetPacketsPerSec() {
    return s_ppsValue;
}
