/**
 * @file lua_serial.cpp
 * @brief EdgeTX LUA Serial mode — bidirectional channel + command protocol
 *
 * See lua_serial.h for full protocol documentation.
 *
 * UART configuration: 115200 baud, 8N1, no inversion (standard UART).
 * EdgeTX AUX port must be configured as "LUA" at 115200 baud.
 */

#include "lua_serial.h"
#include "config.h"
#include "channel_data.h"
#include "ble_module.h"
#include "sport_telemetry.h"
#include "log.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/uart.h>

// ─── Protocol constants ─────────────────────────────────────────────
static constexpr uint8_t  LUA_SYNC        = 0xAA;
static constexpr uint8_t  LUA_TYPE_CH     = 0x43;  // 'C' — channel frame
static constexpr uint8_t  LUA_TYPE_STATUS = 0x53;  // 'S' — status frame
static constexpr uint8_t  LUA_TYPE_ACK    = 0x41;  // 'A' — ACK frame (response to commands)
static constexpr uint8_t  LUA_TYPE_CFG    = 0x47;  // 'G' — config push frame (ESP32 → Lua)
static constexpr uint8_t  LUA_TYPE_INF    = 0x49;  // 'I' — info frame: 12-byte build timestamp
static constexpr uint8_t  LUA_TYPE_SYS    = 0x59;  // 'Y' — system info frame (ESP32 → Lua)
static constexpr uint8_t  LUA_TYPE_SCAN_STATUS = 0x44;  // 'D' — scan state notification
static constexpr uint8_t  LUA_TYPE_SCAN_ENTRY  = 0x52;  // 'R' — scan result entry
static constexpr uint8_t  LUA_TYPE_CMD    = 0x02;  // command frame (incoming)
static constexpr uint8_t  LUA_TYPE_STR_SET = 0x4E; // 'N' — string set frame (incoming: subCmd + 16 data bytes)
static constexpr uint8_t  STR_SUB_BT_NAME  = 0x01; // subcommand: set BT name
static constexpr uint8_t  STR_SUB_SSID     = 0x02; // subcommand: set AP SSID
static constexpr uint8_t  STR_SUB_UDP_PORT = 0x03; // subcommand: set UDP port
static constexpr uint8_t  STR_SUB_AP_PASS  = 0x04; // subcommand: set AP password
static constexpr uint8_t  LUA_CMD_BAUD_57600     = 0x0F; // set mirror baud → 57600
static constexpr uint8_t  LUA_CMD_BAUD_115200    = 0x23; // set mirror baud → 115200
static constexpr uint8_t  LUA_CMD_TOGGLE_AP     = 0x01;
static constexpr uint8_t  LUA_CMD_AP_ON          = 0x02;
static constexpr uint8_t  LUA_CMD_AP_OFF         = 0x03;
static constexpr uint8_t  LUA_CMD_DEV_TRAINER_IN  = 0x20;  // set device mode → Trainer IN
static constexpr uint8_t  LUA_CMD_DEV_TRAINER_OUT = 0x21;  // set device mode → Trainer OUT
static constexpr uint8_t  LUA_CMD_DEV_TELEMETRY   = 0x22;  // set device mode → Telemetry
static constexpr uint8_t  LUA_CMD_REQUEST_INFO   = 0x06;  // Lua requests CFG+INF immediately
static constexpr uint8_t  LUA_CMD_BLE_SCAN       = 0x07;  // start BLE scan (Central mode)
static constexpr uint8_t  LUA_CMD_HEARTBEAT       = 0x08;  // Tools-active heartbeat (no-op, updates idle timer)
static constexpr uint8_t  LUA_CMD_BLE_DISCONNECT  = 0x09;  // disconnect BLE (keep saved addr)
static constexpr uint8_t  LUA_CMD_BLE_FORGET      = 0x0A;  // forget saved BLE device
static constexpr uint8_t  LUA_CMD_BLE_RECONNECT   = 0x0B;  // reconnect to saved BLE device
static constexpr uint8_t  LUA_CMD_BLE_CONNECT_0   = 0x10;  // 0x10..0x1F = connect scan[0..15]
static constexpr uint8_t  LUA_CMD_TELEM_WIFI       = 0x0C;  // set telemetry output → WiFi UDP (save+restart)
static constexpr uint8_t  LUA_CMD_TELEM_BLE        = 0x0D;  // set telemetry output → BLE (save+restart)
static constexpr uint8_t  LUA_CMD_TELEM_OFF        = 0x0E;  // set telemetry output → None/Off (save+restart)
static constexpr uint8_t  LUA_TYPE_TLM            = 0x54;  // 'T' — telemetry forward frame (Radio → ESP32)

static constexpr uint32_t LUA_BAUD             = 115200;
static constexpr uint32_t CH_FRAME_INTERVAL    = 10;     // ms (~100 Hz)
static constexpr uint32_t STATUS_INTERVAL      = 500;    // ms
static constexpr uint32_t CFG_INTERVAL         = 30000;  // ms — periodic resync
static constexpr uint32_t TOOLS_IDLE_TIMEOUT   = 15000;  // ms — stop heavy TX if no CMD received

// Channel frame: sync(1) + type(1) + 8×2 bytes channels(16) + CRC(1) = 19 bytes
static constexpr uint8_t  CH_FRAME_LEN = 19;
// Status frame: sync(1) + type(1) + status(1) + CRC(1) = 4 bytes
static constexpr uint8_t  ST_FRAME_LEN = 4;
// ACK frame: sync(1) + type(1) + result(1) + CRC(1) = 4 bytes
static constexpr uint8_t  ACK_FRAME_LEN = 4;
// Config frame: sync(1) + type(1) + apMode(1) + deviceMode(1) + tlmOutput(1) + CRC(1) = 6 bytes
static constexpr uint8_t  CFG_FRAME_LEN = 6;
// Info frame: sync(1) + type(1) + 12 ASCII timestamp bytes + CRC(1) = 15 bytes
static constexpr uint8_t  INF_PAYLOAD_LEN = 12;
static constexpr uint8_t  INF_FRAME_LEN   = 15;
// Sys frame: sync(1)+type(1)+serialMode(1)+btName(16)+localAddr(18)+remoteAddr(18)+apSsid(16)+udpPort(2)+apPass(16)+baudIdx(1)+CRC(1)=91
static constexpr uint8_t  SYS_FRAME_LEN   = 91;
// Rx command frame: sync(1) + type(1) + cmd(1) + CRC(1) = 4 bytes
static constexpr uint8_t  CMD_FRAME_LEN = 4;
// Scan status: sync(1)+type(1)+state(1)+count(1)+CRC(1) = 5
static constexpr uint8_t  SCAN_STATUS_FRAME_LEN = 5;
// Scan entry: sync(1)+type(1)+idx(1)+rssi(1)+hasFrsky(1)+name(16)+addr(18)+CRC(1) = 40
static constexpr uint8_t  SCAN_ENTRY_FRAME_LEN  = 40;
// TLM frame (Radio → ESP32): sync(1)+type(1)+8-byte SportPacket+CRC(1) = 12 bytes
static constexpr uint8_t  TLM_FRAME_PAYLOAD_LEN = 9;   // type(1) + SportPacket(8), CRC follows

// ─── Module state ───────────────────────────────────────────────────
static bool     s_running       = false;
static uint8_t  s_apMode        = 0;  // 0=normal, 1=AP-block (Lua overlay), 2=telemetry-AP (no block)
static uint32_t s_lastChMs      = 0;
static uint32_t s_lastStatusMs  = 0;
static uint32_t s_lastCfgMs     = 0;
static uint32_t s_lastToolsCmdMs = 0; // millis() of last CMD/STR_SET from Tools script

// BLE scan drip-feed state
static bool                s_scanWasActive   = false;
static bool                s_scanSending     = false;
static uint8_t             s_scanSendIdx     = 0;
static uint8_t             s_scanSendTotal   = 0;
static uint32_t            s_lastScanEntryMs = 0;
static BleScanResult       s_scanCache[MAX_SCAN_RESULTS];
// BLE connection change detection
static bool                s_lastBleConnected = false;

// RX state machine
static uint8_t  s_rxState  = 0;   // 0=wait sync, 1=got sync, 2=accumulating
static uint8_t  s_rxType   = 0;
static uint8_t  s_rxBuf[20];      // max incoming: T_STR_SET = type(1)+subCmd(1)+data(16)+CRC(1) = 19
static uint8_t  s_rxPos    = 0;
static uint8_t  s_rxNeeded = 0;

// Telemetry output (Lua proxy) state
static bool     s_tlmOutputActive = false;

// ─── Functions in main.cpp ──────────────────────────────────────────
// ─── Functions in main.cpp ──────────────────────────────────────────────
extern void mainRequestApMode();
extern void mainRequestNormalMode();
extern void mainSetDeviceMode(uint8_t mode);
extern void mainSetTelemOutput(uint8_t output);  // save telemetryOutput config + restart into appropriate boot mode
extern void mainSetMirrorBaud(uint32_t baud);    // save sportBaud config + restart

// ─── T_TLM handler ─────────────────────────────────────────────────────

/**
 * @brief Process a validated T_TLM frame: parse SportPacket and forward to output.
 *
 * s_rxBuf layout after accumulation:
 *   [0] = TYPE (0x54)
 *   [1] = physId
 *   [2] = primId
 *   [3] = dataId_lo
 *   [4] = dataId_hi
 *   [5..8] = value (little-endian uint32)
 *   [9] = XOR_CRC  (XOR of s_rxBuf[0..8])
 */
static void handleTlmFrame() {
    // Verify XOR CRC over bytes [0..8]
    uint8_t crc = 0;
    for (uint8_t i = 0; i < 9; i++) crc ^= s_rxBuf[i];
    if (crc != s_rxBuf[9]) {
        LOG_W("LUA", "T_TLM CRC error: calc=0x%02X got=0x%02X", crc, s_rxBuf[9]);
        return;
    }

    // NONE mode: discard packet silently (no output configured)
    if (g_config.telemetryOutput == TelemetryOutput::NONE) return;

    // Init output on first arriving packet
    if (!s_tlmOutputActive) {
        sportOutputInit();
        s_tlmOutputActive = true;
    }

    SportPacket pkt;
    pkt.physId = s_rxBuf[1];
    pkt.primId = s_rxBuf[2];
    pkt.dataId = s_rxBuf[3] | ((uint16_t)s_rxBuf[4] << 8);
    pkt.value  = s_rxBuf[5] | ((uint32_t)s_rxBuf[6] << 8) |
                 ((uint32_t)s_rxBuf[7] << 16) | ((uint32_t)s_rxBuf[8] << 24);

    sportOutputForwardPacket(&pkt);
}

// ─── Helpers ────────────────────────────────────────────────────────

/**
 * @brief Convert a HeadTracker PPM channel value to a signed trainer int16.
 *
 * Input:  1050 (-100%) … 1500 (centre) … 1950 (+100%)
 * Output: -1024        …    0          …  +1024
 */
static inline int16_t ppmToTrainer(uint16_t ppm) {
    // (ppm - 1500) * 1024 / 450
    int32_t v = ((int32_t)(ppm - CHANNEL_CENTER) * 1024) / (CHANNEL_RANGE / 2);
    if (v < -1024) v = -1024;
    if (v >  1024) v =  1024;
    return (int16_t)v;
}

// ─── Frame builders ─────────────────────────────────────────────────

/**
 * @brief Send a channel frame (19 bytes) for all 8 channels.
 */
static void sendChannelFrame() {
    uint16_t raw[BT_CHANNELS];
    g_channelData.getChannels(raw, BT_CHANNELS);

    uint8_t frame[CH_FRAME_LEN];
    frame[0] = LUA_SYNC;
    frame[1] = LUA_TYPE_CH;

    uint8_t crc = LUA_TYPE_CH;
    for (uint8_t ch = 0; ch < BT_CHANNELS; ch++) {
        int16_t val = ppmToTrainer(raw[ch]);
        uint8_t hi = (uint8_t)((val >> 8) & 0xFF);
        uint8_t lo = (uint8_t)(val & 0xFF);
        frame[2 + ch * 2]     = hi;
        frame[2 + ch * 2 + 1] = lo;
        crc ^= hi;
        crc ^= lo;
    }
    frame[CH_FRAME_LEN - 1] = crc;

    uart_write_bytes(UART_NUM_1, (const char*)frame, CH_FRAME_LEN);
}

/**
 * @brief Send a status frame (4 bytes).
 *
 * STATUS bit0 = BLE connected  (1 = connected)
 * STATUS bit1 = AP has clients  (1 = ≥1 STA connected)
 * STATUS bit2 = BLE connecting  (1 = connection attempt in progress)
 */
static void sendStatusFrame() {
    uint8_t status = (bleIsConnected() ? 0x01 : 0x00)
                   | (WiFi.softAPgetStationNum() > 0 ? 0x02 : 0x00)
                   | (bleIsConnecting() ? 0x04 : 0x00);
    uint8_t crc    = LUA_TYPE_STATUS ^ status;

    uint8_t frame[ST_FRAME_LEN] = {LUA_SYNC, LUA_TYPE_STATUS, status, crc};
    uart_write_bytes(UART_NUM_1, (const char*)frame, ST_FRAME_LEN);
}

/**
 * @brief Send an info frame (15 bytes) with the 12-digit build timestamp (DDMMYYYYHHMM).
 */
static void sendInfoFrame() {
    uint8_t frame[INF_FRAME_LEN];
    frame[0] = LUA_SYNC;
    frame[1] = LUA_TYPE_INF;
    uint8_t crc = LUA_TYPE_INF;
    for (uint8_t i = 0; i < INF_PAYLOAD_LEN; i++) {
        uint8_t b = (uint8_t)BUILD_TIMESTAMP[i];
        frame[2 + i] = b;
        crc ^= b;
    }
    frame[INF_FRAME_LEN - 1] = crc;
    uart_write_bytes(UART_NUM_1, (const char*)frame, INF_FRAME_LEN);
}

/**
 * @brief Send a config push frame (6 bytes) with current AP mode, BLE role, and telemetry output.
 *
 * apMode byte semantics (Lua side):
 *   0 = AP active, Lua input blocked (regular AP/web-config mode)
 *   1 = normal operation (BLE active, no AP)
 *   2 = telemetry AP (WiFi AP active but Lua continues normally)
 */
static void sendConfigFrame() {
    // Map internal s_apMode → protocol byte
    uint8_t apByte = (s_apMode == 1) ? 0 :   // AP-block  → 0
                     (s_apMode == 2) ? 2 : 1; // telem-AP  → 2; normal → 1
    uint8_t dev  = static_cast<uint8_t>(g_config.deviceMode);
    uint8_t tout = static_cast<uint8_t>(g_config.telemetryOutput);
    uint8_t crc  = LUA_TYPE_CFG ^ apByte ^ dev ^ tout;
    uint8_t frame[CFG_FRAME_LEN] = {LUA_SYNC, LUA_TYPE_CFG, apByte, dev, tout, crc};
    uart_write_bytes(UART_NUM_1, (const char*)frame, CFG_FRAME_LEN);
}

/**
 * @brief Send a system info frame (72 bytes): serialMode, btName, localAddr, remoteAddr, apSsid.
 */
static void sendSysFrame() {
    uint8_t frame[SYS_FRAME_LEN];
    frame[0] = LUA_SYNC;
    frame[1] = LUA_TYPE_SYS;
    frame[2] = (uint8_t)g_config.serialMode;
    uint8_t crc = LUA_TYPE_SYS ^ frame[2];
    // btName: 16 bytes (max 15 chars + null-pad)
    const char* name = g_config.btName;
    size_t nameLen = strlen(name);
    for (uint8_t i = 0; i < 16; i++) {
        frame[3 + i] = (i < nameLen) ? (uint8_t)name[i] : 0;
        crc ^= frame[3 + i];
    }
    // localAddr: 18 bytes (17-char MAC + null-pad)
    const char* local = bleGetLocalAddress();
    size_t localLen = local ? strlen(local) : 0;
    for (uint8_t i = 0; i < 18; i++) {
        frame[19 + i] = (i < localLen) ? (uint8_t)local[i] : 0;
        crc ^= frame[19 + i];
    }
    // remoteAddr: 18 bytes (saved remote MAC or zeros)
    const char* remote = g_config.hasRemoteAddr ? g_config.remoteBtAddr : nullptr;
    size_t remoteLen = remote ? strlen(remote) : 0;
    for (uint8_t i = 0; i < 18; i++) {
        frame[37 + i] = (i < remoteLen) ? (uint8_t)remote[i] : 0;
        crc ^= frame[37 + i];
    }
    // apSsid: 16 bytes (max 15 chars + null-pad)
    const char* ssid = g_config.apSsid;
    size_t ssidLen = strlen(ssid);
    for (uint8_t i = 0; i < 16; i++) {
        frame[55 + i] = (i < ssidLen) ? (uint8_t)ssid[i] : 0;
        crc ^= frame[55 + i];
    }
    // udpPort: 2 bytes big-endian
    frame[71] = (uint8_t)(g_config.udpPort >> 8);
    frame[72] = (uint8_t)(g_config.udpPort & 0xFF);
    crc ^= frame[71]; crc ^= frame[72];
    // apPass: 16 bytes (max 15 chars + null-pad)
    const char* pass = g_config.apPass;
    size_t passLen = strlen(pass);
    for (uint8_t i = 0; i < 16; i++) {
        frame[73 + i] = (i < passLen) ? (uint8_t)pass[i] : 0;
        crc ^= frame[73 + i];
    }
    // baudIdx: 0=57600, 1=115200
    frame[89] = (g_config.sportBaud == 115200) ? 1 : 0;
    crc ^= frame[89];
    frame[90] = crc;
    uart_write_bytes(UART_NUM_1, (const char*)frame, SYS_FRAME_LEN);
}

/**
 * @brief Send an ACK frame (4 bytes) in response to a received command.
 * @param result  0x00 = success, 0x01 = error / unknown command
 */
static void sendAckFrame(uint8_t result) {
    uint8_t crc = LUA_TYPE_ACK ^ result;
    uint8_t frame[ACK_FRAME_LEN] = {LUA_SYNC, LUA_TYPE_ACK, result, crc};
    uart_write_bytes(UART_NUM_1, (const char*)frame, ACK_FRAME_LEN);
    // Flush before any potential restart so the radio receives the ACK
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
}

/**
 * @brief Send a scan status frame (5 bytes).
 * @param state  0=idle, 1=scanning, 2=complete
 * @param count  number of results (valid when state==2)
 */
static void sendScanStatusFrame(uint8_t state, uint8_t count) {
    uint8_t crc = LUA_TYPE_SCAN_STATUS ^ state ^ count;
    uint8_t frame[SCAN_STATUS_FRAME_LEN] = {
        LUA_SYNC, LUA_TYPE_SCAN_STATUS, state, count, crc
    };
    uart_write_bytes(UART_NUM_1, (const char*)frame, SCAN_STATUS_FRAME_LEN);
}

/**
 * @brief Send a single scan result entry frame (40 bytes).
 * @param index  index into s_scanCache[]
 */
static void sendScanEntryFrame(uint8_t index) {
    if (index >= s_scanSendTotal) return;
    const BleScanResult& dev = s_scanCache[index];

    uint8_t frame[SCAN_ENTRY_FRAME_LEN];
    frame[0] = LUA_SYNC;
    frame[1] = LUA_TYPE_SCAN_ENTRY;
    frame[2] = index;
    frame[3] = (uint8_t)(int8_t)dev.rssi;  // signed → unsigned byte
    frame[4] = dev.hasFrskyService ? 1 : 0;

    uint8_t crc = LUA_TYPE_SCAN_ENTRY ^ frame[2] ^ frame[3] ^ frame[4];

    // name: 16 bytes null-padded
    size_t nameLen = strlen(dev.name);
    for (uint8_t i = 0; i < 16; i++) {
        frame[5 + i] = (i < nameLen) ? (uint8_t)dev.name[i] : 0;
        crc ^= frame[5 + i];
    }
    // addr: 18 bytes null-padded
    size_t addrLen = strlen(dev.address);
    for (uint8_t i = 0; i < 18; i++) {
        frame[21 + i] = (i < addrLen) ? (uint8_t)dev.address[i] : 0;
        crc ^= frame[21 + i];
    }

    frame[39] = crc;
    uart_write_bytes(UART_NUM_1, (const char*)frame, SCAN_ENTRY_FRAME_LEN);
}

// ─── RX parser ──────────────────────────────────────────────────────

static void executeCommand(uint8_t cmd) {
    switch (cmd) {
        case LUA_CMD_TOGGLE_AP:
            LOG_I("LUA", "Received CMD: Toggle AP mode");
            sendAckFrame(0x00);
            mainRequestApMode();
            break;
        case LUA_CMD_AP_ON:
            LOG_I("LUA", "Received CMD: AP mode ON");
            if (g_config.telemetryOutput == TelemetryOutput::BLE) {
                // BLE and WiFi AP share the same radio — clear BLE telemetry before entering AP mode
                g_config.telemetryOutput = TelemetryOutput::NONE;
                configSave();
            }
            sendAckFrame(0x00);
            mainRequestApMode();
            break;
        case LUA_CMD_AP_OFF:
            LOG_I("LUA", "Received CMD: AP mode OFF (normal)");
            // If Telemetry AP was active (WiFi UDP output), clear it to NONE so
            // the board doesn't redirect back to Telemetry AP mode on next boot.
            if (g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
                g_config.telemetryOutput = TelemetryOutput::NONE;
                configSave();
            }
            sendAckFrame(0x00);
            mainRequestNormalMode();
            break;
        case LUA_CMD_DEV_TRAINER_IN:
            LOG_I("LUA", "Received CMD: Device mode -> Trainer IN");
            if (g_config.telemetryOutput != TelemetryOutput::NONE) {
                g_config.telemetryOutput = TelemetryOutput::NONE;
                configSave();
            }
            sendAckFrame(0x00);
            mainSetDeviceMode(0);
            break;
        case LUA_CMD_DEV_TRAINER_OUT:
            LOG_I("LUA", "Received CMD: Device mode -> Trainer OUT");
            if (g_config.telemetryOutput != TelemetryOutput::NONE) {
                g_config.telemetryOutput = TelemetryOutput::NONE;
                configSave();
            }
            sendAckFrame(0x00);
            mainSetDeviceMode(1);
            break;
        case LUA_CMD_DEV_TELEMETRY:
            LOG_I("LUA", "Received CMD: Device mode -> Telemetry");
            sendAckFrame(0x00);
            mainSetDeviceMode(2);
            break;
        case LUA_CMD_REQUEST_INFO:
            LOG_I("LUA", "Received CMD: Request info");
            sendConfigFrame();
            sendInfoFrame();
            sendSysFrame();
            s_lastCfgMs = millis();  // reset timer so periodic resync doesn't double-fire
            break;
        case LUA_CMD_HEARTBEAT:
            // No-op: purpose is to update s_lastToolsCmdMs (done in processRxByte)
            sendAckFrame(0x00);
            break;
        case LUA_CMD_BLE_SCAN:
            LOG_I("LUA", "Received CMD: BLE scan start");
            if (bleScanStart()) {
                sendAckFrame(0x00);
                sendScanStatusFrame(1, 0);  // notify: scanning started
            } else {
                sendAckFrame(0x01);  // scan could not start
                sendScanStatusFrame(0, 0);  // notify: idle (not scanning)
            }
            break;
        case LUA_CMD_BLE_DISCONNECT:
            LOG_I("LUA", "Received CMD: BLE disconnect");
            bleDisconnect();
            sendAckFrame(0x00);
            break;
        case LUA_CMD_BLE_FORGET:
            LOG_I("LUA", "Received CMD: BLE forget");
            bleForget();
            sendAckFrame(0x00);
            sendSysFrame();  // re-push SYS with cleared remoteAddr
            break;
        case LUA_CMD_BLE_RECONNECT:
            LOG_I("LUA", "Received CMD: BLE reconnect");
            if (g_config.hasRemoteAddr) {
                bleConnectTo(g_config.remoteBtAddr);
                sendAckFrame(0x00);
            } else {
                sendAckFrame(0x01);  // no saved address
            }
            break;
        case LUA_CMD_TELEM_WIFI:
            LOG_I("LUA", "Received CMD: Telemetry output -> WiFi UDP");
            sendAckFrame(0x00);
            mainSetTelemOutput(0);
            break;
        case LUA_CMD_TELEM_BLE:
            LOG_I("LUA", "Received CMD: Telemetry output -> BLE");
            sendAckFrame(0x00);
            mainSetTelemOutput(1);
            break;
        case LUA_CMD_TELEM_OFF:
            LOG_I("LUA", "Received CMD: Telemetry output -> Off");
            sendAckFrame(0x00);
            mainSetTelemOutput(2);
            break;
        case LUA_CMD_BAUD_57600:
            LOG_I("LUA", "Received CMD: Mirror baud -> 57600");
            sendAckFrame(0x00);
            mainSetMirrorBaud(57600);
            break;
        case LUA_CMD_BAUD_115200:
            LOG_I("LUA", "Received CMD: Mirror baud -> 115200");
            sendAckFrame(0x00);
            mainSetMirrorBaud(115200);
            break;
        default:
            if (cmd >= LUA_CMD_BLE_CONNECT_0 && cmd <= (LUA_CMD_BLE_CONNECT_0 + 15)) {
                uint8_t idx = cmd - LUA_CMD_BLE_CONNECT_0;
                LOG_I("LUA", "Received CMD: BLE connect to scan[%u]", idx);
                if (idx < s_scanSendTotal) {
                    bleConnectTo(s_scanCache[idx].address);
                    sendAckFrame(0x00);
                } else {
                    LOG_W("LUA", "Scan index %u out of range (have %u)", idx, s_scanSendTotal);
                    sendAckFrame(0x01);
                }
            } else {
                LOG_W("LUA", "Unknown command: 0x%02X", cmd);
                sendAckFrame(0x01);
            }
            break;
    }
}

/**
 * @brief Process one byte from the RX ring buffer via a state machine.
 *
 * We only need to handle incoming command frames (4 bytes):
 *   [0xAA] [0x02] [CMD] [XOR_CRC]
 * CRC = type ^ cmd
 */
static void processRxByte(uint8_t b) {
    switch (s_rxState) {
        case 0:  // wait for sync
            if (b == LUA_SYNC) s_rxState = 1;
            break;

        case 1:  // got sync — read type
            if (b == LUA_TYPE_CMD) {
                s_rxType   = b;
                s_rxBuf[0] = b;   // store type for CRC
                s_rxPos    = 1;
                s_rxNeeded = 2;   // still need: cmd(1) + crc(1)
                s_rxState  = 2;
            } else if (b == LUA_TYPE_TLM) {
                s_rxType   = b;
                s_rxBuf[0] = b;   // store type for CRC
                s_rxPos    = 1;
                s_rxNeeded = 9;   // still need: 8 SportPacket bytes + crc(1)
                s_rxState  = 2;
            } else if (b == LUA_TYPE_STR_SET) {
                s_rxType   = b;
                s_rxBuf[0] = b;   // store type for CRC
                s_rxPos    = 1;
                s_rxNeeded = 18;  // subCmd(1) + data(16) + crc(1)
                s_rxState  = 2;
            } else {
                // Unknown type; if it's another sync, stay in state 1
                s_rxState = (b == LUA_SYNC) ? 1 : 0;
            }
            break;

        case 2:  // accumulating payload bytes
            s_rxBuf[s_rxPos++] = b;
            if (--s_rxNeeded == 0) {
                if (s_rxType == LUA_TYPE_CMD) {
                    // s_rxBuf[0]=type, [1]=cmd, [2]=CRC
                    uint8_t expected_crc = s_rxBuf[0] ^ s_rxBuf[1];
                    if (s_rxBuf[2] == expected_crc) {
                        s_lastToolsCmdMs = millis();  // Tools is active
                        executeCommand(s_rxBuf[1]);
                    } else {
                        LOG_W("LUA", "CMD CRC error: got 0x%02X expected 0x%02X",
                              s_rxBuf[2], expected_crc);
                    }
                } else if (s_rxType == LUA_TYPE_TLM) {
                    handleTlmFrame();
                } else if (s_rxType == LUA_TYPE_STR_SET) {
                    // s_rxBuf: [0]=type, [1]=subCmd, [2..17]=data(16), [18]=CRC
                    uint8_t crc = 0;
                    for (uint8_t i = 0; i < 18; i++) crc ^= s_rxBuf[i];
                    if (crc != s_rxBuf[18]) {
                        LOG_W("LUA", "STR_SET CRC error");
                    } else {
                        s_lastToolsCmdMs = millis();  // Tools is active
                        // Extract null-terminated string from data field
                        char str[17];
                        memcpy(str, &s_rxBuf[2], 16);
                        str[16] = '\0';
                        uint8_t subCmd = s_rxBuf[1];
                        if (subCmd == STR_SUB_BT_NAME && strlen(str) > 0) {
                            strlcpy(g_config.btName, str, sizeof(g_config.btName));
                            LOG_I("LUA", "BT name set to %s", str);
                            configSave();
                            bleUpdateAdvertisingName();
                            sendAckFrame(0x00);
                            sendSysFrame();
                        } else if (subCmd == STR_SUB_SSID && strlen(str) > 0) {
                            strlcpy(g_config.apSsid, str, sizeof(g_config.apSsid));
                            LOG_I("LUA", "AP SSID set to %s — restarting", str);
                            configSave();
                            sendAckFrame(0x00);
                            delay(100);
                            ESP.restart();
                        } else if (subCmd == STR_SUB_UDP_PORT) {
                            uint16_t port = (uint16_t)atoi(str);
                            if (port >= 1024 && port <= 65535) {
                                g_config.udpPort = port;
                                LOG_I("LUA", "UDP port set to %u — restarting", port);
                                configSave();
                                sendAckFrame(0x00);
                                delay(100);
                                ESP.restart();
                            } else {
                                sendAckFrame(0x01);
                            }
                        } else if (subCmd == STR_SUB_AP_PASS) {
                            size_t plen = strlen(str);
                            if (plen >= 8 && plen <= 15) {
                                strlcpy(g_config.apPass, str, sizeof(g_config.apPass));
                                LOG_I("LUA", "AP pass updated — restarting");
                                configSave();
                                sendAckFrame(0x00);
                                delay(100);
                                ESP.restart();
                            } else {
                                sendAckFrame(0x01);
                            }
                        } else {
                            sendAckFrame(0x01);
                        }
                    }
                }
                s_rxState = 0;
            }
            break;

        default:
            s_rxState = 0;
            break;
    }
}

static void readIncoming() {
    uint8_t tmp[64];
    int len = uart_read_bytes(UART_NUM_1, tmp, sizeof(tmp), 0);
    for (int i = 0; i < len; i++) {
        processRxByte(tmp[i]);
    }
}

// ─── Public API ─────────────────────────────────────────────────────

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
    // 256-byte TX buffer, 256-byte RX buffer, no queue, no interrupts
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 256, 0, NULL, 0));

    s_running      = true;
    s_rxState      = 0;
    s_lastChMs     = 0;
    s_lastStatusMs = 0;
    s_apMode       = 0;  // caller sets via luaSerialSetApMode() if needed
    s_lastCfgMs    = millis() - CFG_INTERVAL;  // fire on first loop tick
    // Reset scan state
    s_scanWasActive   = false;
    s_scanSending     = false;
    s_scanSendIdx     = 0;
    s_scanSendTotal   = 0;
    s_lastScanEntryMs = 0;
    s_lastBleConnected = bleIsConnected();
    s_lastToolsCmdMs   = 0;  // no Tools activity yet

    LOG_I("LUA", "Initialized: TX=%d RX=%d @ %lu baud (8N1)",
          PIN_SERIAL_TX, PIN_SERIAL_RX, LUA_BAUD);
}

void luaSerialLoop() {
    if (!s_running) return;

    // Process any incoming bytes first so CMD_REQ_INFO is handled before
    // this iteration's outgoing frames are appended to the TX FIFO.
    readIncoming();

    uint32_t now = millis();

    // Send channel frame at ~50 Hz only when BLE is connected and data is fresh
    if (now - s_lastChMs >= CH_FRAME_INTERVAL) {
        s_lastChMs = now;
        if (bleIsConnected() && !g_channelData.isStale(500)) {
            sendChannelFrame();
        }
    }

    // Send status frame every 500 ms
    if (now - s_lastStatusMs >= STATUS_INTERVAL) {
        s_lastStatusMs = now;
        bool connected = bleIsConnected();
        sendStatusFrame();
        // If BLE connection state changed, resend SYS+CFG so Lua updates immediately
        if (connected != s_lastBleConnected) {
            s_lastBleConnected = connected;
            sendSysFrame();
            sendConfigFrame();
        }
    }

    // Periodic resync only while Tools script is active (heartbeat/CMD received recently).
    // btwfs.lua discards these frames anyway, so no need to send them when Tools is idle.
    if ((now - s_lastCfgMs >= CFG_INTERVAL) && (now - s_lastToolsCmdMs < TOOLS_IDLE_TIMEOUT)) {
        s_lastCfgMs = now;
        sendConfigFrame();
        sendInfoFrame();
        sendSysFrame();
    }

    // ─── BLE scan state tracking ────────────────────────────────────
    bool scanning = bleIsScanning();
    if (s_scanWasActive && !scanning) {
        // Scan just completed → cache results and start drip-feed
        s_scanSendTotal = bleGetScanResults(s_scanCache, MAX_SCAN_RESULTS);
        sendScanStatusFrame(2, s_scanSendTotal);
        s_scanSendIdx   = 0;
        s_scanSending   = (s_scanSendTotal > 0);
    }
    s_scanWasActive = scanning;

    // Drip-feed scan results (one per 20 ms to avoid UART overflow)
    if (s_scanSending && s_scanSendIdx < s_scanSendTotal) {
        if (now - s_lastScanEntryMs >= 20) {
            s_lastScanEntryMs = now;
            sendScanEntryFrame(s_scanSendIdx++);
            if (s_scanSendIdx >= s_scanSendTotal) {
                s_scanSending = false;
            }
        }
    }

    // (incoming bytes already read at the top of this function)
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
