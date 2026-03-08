/**
 * @file frsky_serial.cpp
 * @brief FrSky CC2540 serial emulation for EdgeTX radios (with AT handler)
 *
 * Protocol: 115200 baud, 8N1
 * Frame format: [0x7E] [0x80] [ch_data...] [CRC] [0x7E]
 *
 * When EdgeTX is compiled with BLUETOOTH=ON, it sends HM-10/CC2540 AT commands
 * on the same UART to configure the BT module. This module intercepts AT
 * commands in the byte stream, responds appropriately, and passes non-AT
 * data to the FrSky trainer state machine.
 *
 * In Central role: receives BLE data -> sends FrSky frame out UART to radio.
 * In Peripheral role: receives UART frames from radio -> forwards via BLE.
 */

#include "frsky_serial.h"
#include "config.h"
#include "channel_data.h"
#include "ble_module.h"
#include "log.h"

// ─── Protocol constants ─────────────────────────────────────────────
static constexpr uint8_t  START_STOP     = 0x7E;
static constexpr uint8_t  BYTE_STUFF     = 0x7D;
static constexpr uint8_t  STUFF_MASK     = 0x20;
static constexpr uint8_t  TRAINER_FRAME  = 0x80;
static constexpr uint32_t FRSKY_BAUD     = 115200;
static constexpr uint32_t FRAME_INTERVAL_MS = 10;  // ~100 Hz like CC2540
static constexpr uint8_t  LINE_LENGTH    = 32;

// ─── UART instance ──────────────────────────────────────────────────
static HardwareSerial* s_serial = nullptr;
static bool s_running = false;
static uint32_t s_lastFrameMs = 0;

// ═══════════════════════════════════════════════════════════════════
// AT COMMAND HANDLER (HM-10 emulation for EdgeTX BLUETOOTH=ON)
// ═══════════════════════════════════════════════════════════════════

static constexpr uint8_t AT_BUF_SIZE = 64;
static char     s_atBuf[AT_BUF_SIZE];
static uint8_t  s_atLen       = 0;
static bool     s_atMode      = false;   // true = currently buffering an AT command
static char     s_prevChar    = 0;       // previous byte for AT detection

// Async scan state: AT+DISC? triggers a scan, results are sent in the loop
static bool     s_atScanPending    = false;
static bool     s_atScanComplete   = false;
static uint8_t  s_atScanSentCount  = 0;

// Connection event notifications
static bool     s_atNotifyConnect    = false;
static bool     s_atNotifyDisconnect = false;
static bool     s_atWasConnected     = false;   // track previous state

/**
 * @brief Send a string response to the radio over UART (with \r\n)
 */
static void atReply(const char* msg) {
    if (!s_serial) return;
    s_serial->print(msg);
    s_serial->print("\r\n");
    LOG_D("AT", ">> %s", msg);
}

/**
 * @brief Send a raw string (no trailing \r\n) — used for multi-line sequences
 */
static void atReplyRaw(const char* msg) {
    if (!s_serial) return;
    s_serial->print(msg);
}

/**
 * @brief Process a complete AT command (without the "AT" prefix)
 *
 * EdgeTX bluetooth.cpp sends these commands in sequence:
 *   AT+BAUD4   → set 115200 baud
 *   AT+NAME... → set BT advertised name
 *   AT+TXPW2   → set TX power
 *   AT+ROLE0/1 → set peripheral/central
 *   AT+DISC?   → discover nearby devices
 *   AT+CON...  → connect to address
 *   AT+CLEAR   → disconnect
 */
static void processAtCommand(const char* cmd) {
    LOG_I("AT", "<< AT%s", cmd);

    // ─── AT+BAUD ────────────────────────────────────────────────
    // EdgeTX sends AT+BAUD4 to switch from 57600 to 115200.
    // We're already at 115200, just acknowledge.
    if (strncmp(cmd, "+BAUD", 5) == 0) {
        char reply[24];
        snprintf(reply, sizeof(reply), "OK+Set:%c", cmd[5] ? cmd[5] : '4');
        atReply(reply);
    }
    // ─── AT+NAME ────────────────────────────────────────────────
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
    // ─── AT+TXPW ────────────────────────────────────────────────
    else if (strncmp(cmd, "+TXPW", 5) == 0) {
        char reply[16];
        snprintf(reply, sizeof(reply), "OK+Txpw:%c", cmd[5] ? cmd[5] : '0');
        atReply(reply);
    }
    // ─── AT+ROLE ────────────────────────────────────────────────
    else if (strncmp(cmd, "+ROLE", 5) == 0) {
        char roleChar = cmd[5];
        const char* localAddr = bleGetLocalAddress();
        if (!localAddr || !localAddr[0]) localAddr = "00:00:00:00:00:00";

        if (roleChar == '1') {
            // Central (master) → Trainer IN
            if (g_config.deviceMode != DeviceMode::TRAINER_IN) {
                g_config.deviceMode = DeviceMode::TRAINER_IN;
                configSave();
                // Note: full role switch requires restart on ESP32-C3 due to
                // NimBLE deinit limitations. For now, save config and respond.
            }
            atReply("OK+Role:1");
            char addr[32];
            snprintf(addr, sizeof(addr), "Central:%s", localAddr);
            atReply(addr);
        } else {
            // Peripheral (slave) → Trainer OUT
            if (g_config.deviceMode != DeviceMode::TRAINER_OUT) {
                g_config.deviceMode = DeviceMode::TRAINER_OUT;
                configSave();
            }
            atReply("OK+Role:0");
            char addr[32];
            snprintf(addr, sizeof(addr), "Peripheral:%s", localAddr);
            atReply(addr);
        }
    }
    // ─── AT+DISC? ───────────────────────────────────────────────
    // Start BLE scan. Results are sent asynchronously from the loop.
    else if (strcmp(cmd, "+DISC?") == 0) {
        atReply("OK+DISCS");
        // Only scan in central mode
        if (bleIsCentral(g_config.deviceMode)) {
            bleScanStart();
            s_atScanPending   = true;
            s_atScanComplete  = false;
            s_atScanSentCount = 0;
        } else {
            // Not in central mode — immediately end discovery
            atReply("OK+DISCE");
        }
    }
    // ─── AT+CON ─────────────────────────────────────────────────
    // Connect to a BLE address. Format: AT+CONxxxxxxxxxxxx (12 hex chars, no colons)
    // or AT+CONXX:XX:XX:XX:XX:XX (with colons)
    else if (strncmp(cmd, "+CON", 4) == 0) {
        const char* rawAddr = cmd + 4;
        atReply("OK+CONNA");

        // Convert raw address to colon format if needed
        char addr[18] = {0};
        size_t rawLen = strlen(rawAddr);

        if (rawLen == 12) {
            // No colons: "AABBCCDDEEFF" → "AA:BB:CC:DD:EE:FF"
            snprintf(addr, sizeof(addr), "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                     rawAddr, rawAddr+2, rawAddr+4, rawAddr+6, rawAddr+8, rawAddr+10);
        } else if (rawLen >= 17) {
            // Already has colons
            strlcpy(addr, rawAddr, sizeof(addr));
        } else {
            strlcpy(addr, rawAddr, sizeof(addr));
        }

        // Convert to lowercase (NimBLE expects lowercase hex)
        for (char* p = addr; *p; p++) {
            if (*p >= 'A' && *p <= 'F') *p = *p + 32;
        }

        LOG_I("AT", "Connecting to %s", addr);
        bleConnectTo(addr);
        // The connect event notification is handled asynchronously
    }
    // ─── AT+CLEAR ───────────────────────────────────────────────
    else if (strcmp(cmd, "+CLEAR") == 0) {
        bleDisconnect();
        atReply("OK+CLEAR");
    }
    // ─── Unknown AT command ─────────────────────────────────────
    else {
        LOG_W("AT", "Unknown AT command: AT%s", cmd);
        atReply("OK");
    }
}

/**
 * @brief Process a single byte, detecting AT commands vs FrSky data.
 *
 * AT detection follows the BTWifiModule approach: watch for 'A' followed
 * by 'T' in the byte stream. Once detected, buffer until \r or \n.
 * Non-AT bytes go to the FrSky trainer state machine.
 */
static void rxProcessByteWithAT(uint8_t c);

// ═══════════════════════════════════════════════════════════════════
// TX ENCODER
// ═══════════════════════════════════════════════════════════════════
static uint8_t s_txBuf[LINE_LENGTH + 2];
static uint8_t s_txIdx = 0;
static uint8_t s_txCrc = 0;

static void pushByte(uint8_t byte) {
    s_txCrc ^= byte;
    if (byte == START_STOP || byte == BYTE_STUFF) {
        s_txBuf[s_txIdx++] = BYTE_STUFF;
        byte ^= STUFF_MASK;
    }
    s_txBuf[s_txIdx++] = byte;
}

/**
 * @brief Build and send a FrSky trainer frame over UART
 */
static void sendTrainerFrame(const uint16_t* channels) {
    s_txIdx = 0;
    s_txCrc = 0;

    s_txBuf[s_txIdx++] = START_STOP;
    pushByte(TRAINER_FRAME);

    for (uint8_t ch = 0; ch < BT_CHANNELS; ch += 2) {
        uint16_t v1 = channels[ch];
        uint16_t v2 = channels[ch + 1];
        pushByte(v1 & 0xFF);
        pushByte(((v1 & 0x0F00) >> 4) | ((v2 & 0x00F0) >> 4));
        pushByte(((v2 & 0x000F) << 4) | ((v2 & 0x0F00) >> 8));
    }

    s_txBuf[s_txIdx++] = s_txCrc;
    s_txBuf[s_txIdx++] = START_STOP;

    s_serial->write(s_txBuf, s_txIdx);
}

// ─── RX decoder (for Peripheral mode: reading from radio UART) ──────
static uint8_t  s_rxBuf[LINE_LENGTH + 1];
static uint8_t  s_rxIdx = 0;

enum RxState : uint8_t { RX_IDLE, RX_START, RX_IN_FRAME, RX_XOR };
static RxState s_rxState = RX_IDLE;

static void processReceivedFrame() {
    if (s_rxIdx < 14) return;

    uint8_t crc = 0;
    for (uint8_t i = 0; i < 13; i++) crc ^= s_rxBuf[i];
    if (crc != s_rxBuf[13]) return;
    if (s_rxBuf[0] != TRAINER_FRAME) return;

    uint16_t channels[BT_CHANNELS];
    for (uint8_t ch = 0, i = 1; ch < BT_CHANNELS; ch += 2, i += 3) {
        channels[ch]     = s_rxBuf[i] + ((s_rxBuf[i + 1] & 0xF0) << 4);
        channels[ch + 1] = ((s_rxBuf[i + 1] & 0x0F) << 4) +
                           ((s_rxBuf[i + 2] & 0xF0) >> 4) +
                           ((s_rxBuf[i + 2] & 0x0F) << 8);
    }
    g_channelData.setChannels(channels, BT_CHANNELS);
}

static void rxProcessByte(uint8_t data) {
    switch (s_rxState) {
        case RX_START:
            if (data == START_STOP) {
                s_rxState = RX_IN_FRAME;
                s_rxIdx = 0;
            } else {
                if (s_rxIdx < LINE_LENGTH) s_rxBuf[s_rxIdx++] = data;
            }
            break;

        case RX_IN_FRAME:
            if (data == BYTE_STUFF) {
                s_rxState = RX_XOR;
            } else if (data == START_STOP) {
                // Frame complete or restart
                processReceivedFrame();
                s_rxIdx = 0;
                s_rxState = RX_IN_FRAME;
            } else {
                if (s_rxIdx < LINE_LENGTH) s_rxBuf[s_rxIdx++] = data;
            }
            break;

        case RX_XOR:
            if (s_rxIdx < LINE_LENGTH) s_rxBuf[s_rxIdx++] = data ^ STUFF_MASK;
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

// ═══════════════════════════════════════════════════════════════════
// RX BYTE ROUTER: AT detection + FrSky passthrough
// ═══════════════════════════════════════════════════════════════════

/**
 * @brief Process a single incoming byte.
 * Watches for 'A' followed by 'T' to enter AT command mode.
 * Otherwise feeds bytes to the FrSky trainer state machine.
 */
static void rxProcessByteWithAT(uint8_t c) {
    if (s_atMode) {
        // In AT mode: buffer chars until end-of-line
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
        // Watch for 'A' 'T' sequence
        if (s_prevChar == 'A' && c == 'T') {
            s_atMode = true;
            s_atLen  = 0;
        } else {
            // Not AT — feed to FrSky state machine
            // (The previous 'A' was already fed on its byte; harmless
            // to the FrSky SM because it only activates on 0x7E)
            rxProcessByte(c);
        }
    }
    s_prevChar = c;
}

// ═══════════════════════════════════════════════════════════════════
// ASYNC AT EVENT DELIVERY (scan results, connect/disconnect)
// ═══════════════════════════════════════════════════════════════════

/**
 * @brief Called from loop: deliver pending scan results and connection events.
 */
static void atPollEvents() {
    // ─── Scan results delivery ──────────────────────────────────
    if (s_atScanPending) {
        if (!bleIsScanning()) {
            // Scan finished — send all results
            BleScanResult results[MAX_SCAN_RESULTS];
            uint8_t count = bleGetScanResults(results, MAX_SCAN_RESULTS);

            for (uint8_t i = s_atScanSentCount; i < count; i++) {
                // EdgeTX expects: OK+DISC:AABBCCDDEEFF
                // Convert "aa:bb:cc:dd:ee:ff" → "AABBCCDDEEFF"
                char hexAddr[13] = {0};
                uint8_t h = 0;
                for (const char* p = results[i].address; *p && h < 12; p++) {
                    if (*p != ':') {
                        hexAddr[h++] = (*p >= 'a' && *p <= 'f') ? (*p - 32) : *p;
                    }
                }
                hexAddr[h] = '\0';

                char reply[48];
                snprintf(reply, sizeof(reply), "OK+DISC:%s", hexAddr);
                atReply(reply);
                s_atScanSentCount++;
            }

            atReply("OK+DISCE");
            s_atScanPending  = false;
            s_atScanComplete = true;
            LOG_D("AT", "Scan results sent: %u devices", count);
        }
    }

    // ─── Connection state change notifications ──────────────────
    bool nowConnected = bleIsConnected();
    if (nowConnected && !s_atWasConnected) {
        // Just connected
        const char* rAddr = bleGetRemoteAddress();
        if (rAddr && rAddr[0]) {
            char hexAddr[13] = {0};
            uint8_t h = 0;
            for (const char* p = rAddr; *p && h < 12; p++) {
                if (*p != ':') {
                    hexAddr[h++] = (*p >= 'a' && *p <= 'f') ? (*p - 32) : *p;
                }
            }
            hexAddr[h] = '\0';

            char msg[40];
            snprintf(msg, sizeof(msg), "Connected:%s", hexAddr);
            atReply(msg);
        }
        s_atWasConnected = true;
    } else if (!nowConnected && s_atWasConnected) {
        // Just disconnected
        atReplyRaw("DisConnected\r\n");
        s_atWasConnected = false;
    }
}

// ═══════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void frskySerialInit() {
    s_serial = &Serial1;
    s_serial->begin(FRSKY_BAUD, SERIAL_8N1, PIN_SERIAL_RX, PIN_SERIAL_TX);

    s_running     = true;
    s_lastFrameMs = 0;
    s_rxState     = RX_IDLE;
    s_rxIdx       = 0;

    // Reset AT state
    s_atMode         = false;
    s_atLen          = 0;
    s_prevChar       = 0;
    s_atScanPending  = false;
    s_atScanComplete = false;
    s_atWasConnected = bleIsConnected();

    LOG_I("FRSKY", "Serial initialized: TX=%d RX=%d @ %lu baud (AT handler enabled)",
          PIN_SERIAL_TX, PIN_SERIAL_RX, FRSKY_BAUD);
}

void frskySerialLoop() {
    if (!s_running) return;

    // RX: Read incoming data, routing AT commands vs FrSky trainer data
    while (s_serial->available()) {
        rxProcessByteWithAT(s_serial->read());
    }

    // AT async events (scan results, connect/disconnect notifications)
    atPollEvents();

    // TX: Send channel data to radio (Central/Telemetry mode — data from BLE)
    if (g_config.deviceMode == DeviceMode::TRAINER_IN || g_config.deviceMode == DeviceMode::TELEMETRY) {
        uint32_t now = millis();
        if (now - s_lastFrameMs >= FRAME_INTERVAL_MS) {
            s_lastFrameMs = now;

            if (!g_channelData.isStale()) {
                uint16_t channels[BT_CHANNELS];
                g_channelData.getChannels(channels, BT_CHANNELS);
                sendTrainerFrame(channels);
            }
        }
    }
}

void frskySerialStop() {
    if (s_serial) {
        s_serial->end();
    }
    s_running = false;
    LOG_I("FRSKY", "Serial stopped");
}
