/**
 * @file ble_module.cpp
 * @brief BLE Central & Peripheral implementation using NimBLE
 *
 * FrSky BLE Protocol:
 *   Service UUID:        0xFFF0
 *   Characteristic UUID: 0xFFF6 (trainer data, notify + write)
 *
 * Trainer frame format (from OpenTX/BTWifiModule):
 *   [0x7E] [0x80] [ch1_lo] [ch1_hi|ch2_lo] [ch2_hi] ... [CRC] [0x7E]
 *   8 channels packed as 3 bytes per 2 channels, 12-bit values (0-2047).
 */

#include "ble_module.h"
#include "config.h"
#include "channel_data.h"
#include "web_ui.h"
#include "log.h"

#include <NimBLEDevice.h>
#include <esp_wifi.h>

// ─── FrSky BLE UUIDs ────────────────────────────────────────────────
static const NimBLEUUID FRSKY_SERVICE_UUID((uint16_t)0xFFF0);
static const NimBLEUUID FRSKY_CHAR_UUID((uint16_t)0xFFF6);

// ─── FrSky protocol constants ───────────────────────────────────────
static constexpr uint8_t START_STOP      = 0x7E;
static constexpr uint8_t BYTE_STUFF      = 0x7D;
static constexpr uint8_t STUFF_MASK      = 0x20;
static constexpr uint8_t TRAINER_FRAME   = 0x80;
static constexpr uint8_t BT_PACKET_SIZE  = 14;
static constexpr uint8_t BT_LINE_LENGTH  = 32;

// ─── Module state ───────────────────────────────────────────────────
static bool          s_initialized   = false;
static bool          s_connected     = false;
static bool          s_scanning      = false;
static bool          s_autoReconnect = true;   // cleared by explicit bleDisconnect()
static uint16_t      s_connHandle    = BLE_HS_CONN_HANDLE_NONE;  // peripheral conn handle
static char          s_localAddr[18] = {0};
static char          s_remoteAddr[18] = {0};

// Scan results
static BleScanResult s_scanResults[MAX_SCAN_RESULTS];
static uint8_t       s_scanCount = 0;

// NimBLE objects
static NimBLEServer*         s_pServer    = nullptr;
static NimBLECharacteristic* s_pCharTx    = nullptr;   // Peripheral: notify char
static NimBLEClient*         s_pClient    = nullptr;
static NimBLERemoteCharacteristic* s_pRemoteChar = nullptr;  // Central: remote char

// Connection task (non-blocking connect to avoid blocking loopTask)
static TaskHandle_t      s_connectTaskHandle  = nullptr;
static char              s_pendingAddr[18]    = {0};
static uint8_t           s_pendingAddrType    = 0;
static volatile bool     s_connectInProgress  = false;

// Deferred BLE reinit flag — set from any task, consumed in bleLoop() (loop task)
static volatile bool     s_pendingReinit      = false;

// ─── FrSky frame decoder ────────────────────────────────────────────
static uint8_t s_rxBuffer[BT_LINE_LENGTH + 1];
static uint8_t s_rxIndex = 0;

enum RxState : uint8_t {
    STATE_IDLE,
    STATE_START,
    STATE_IN_FRAME,
    STATE_XOR
};

static RxState s_rxState = STATE_IDLE;

/**
 * @brief Decode a complete FrSky trainer frame and store channel data
 */
static void processTrainerFrame(const uint8_t* buf) {
    // HeadTracker BLE packing (3 bytes per 2 channels, PPM values 1000-2000):
    //   byte[i+0] = ch1[7:0]
    //   byte[i+1] = ch1[11:8] in HIGH nibble | ch2[3:0] in LOW nibble
    //   byte[i+2] = ch2[11:4]  (nibbles swapped relative to EdgeTX)
    uint16_t channels[BT_CHANNELS];
    for (uint8_t ch = 0, i = 1; ch < BT_CHANNELS; ch += 2, i += 3) {
        channels[ch]     = (uint16_t)buf[i]
                         | (((uint16_t)buf[i + 1] & 0xF0) << 4);
        channels[ch + 1] = (((uint16_t)buf[i + 1] & 0x0F) << 4)
                         | (((uint16_t)buf[i + 2] & 0xF0) >> 4)
                         | (((uint16_t)buf[i + 2] & 0x0F) << 8);
    }

    g_channelData.setChannels(channels, BT_CHANNELS);
}

/**
 * @brief Process a single byte through the FrSky state machine
 */
static void processByte(uint8_t data) {
    switch (s_rxState) {
        case STATE_START:
            if (data == START_STOP) {
                s_rxState = STATE_IN_FRAME;
                s_rxIndex = 0;
            } else {
                if (s_rxIndex < BT_LINE_LENGTH) s_rxBuffer[s_rxIndex++] = data;
            }
            break;

        case STATE_IN_FRAME:
            if (data == BYTE_STUFF) {
                s_rxState = STATE_XOR;
            } else if (data == START_STOP) {
                s_rxState = STATE_IN_FRAME;
                s_rxIndex = 0;
            } else {
                if (s_rxIndex < BT_LINE_LENGTH) s_rxBuffer[s_rxIndex++] = data;
            }
            break;

        case STATE_XOR:
            if (data == START_STOP) {
                // Illegal — restart
                s_rxIndex = 0;
                s_rxState = STATE_IN_FRAME;
            } else {
                if (s_rxIndex < BT_LINE_LENGTH) s_rxBuffer[s_rxIndex++] = data ^ STUFF_MASK;
                s_rxState = STATE_IN_FRAME;
            }
            break;

        case STATE_IDLE:
        default:
            if (data == START_STOP) {
                s_rxIndex = 0;
                s_rxState = STATE_START;
            }
            break;
    }

    // Check for complete packet
    if (s_rxIndex >= BT_PACKET_SIZE) {
        uint8_t crc = 0x00;
        for (uint8_t i = 0; i < BT_PACKET_SIZE - 1; i++) {
            crc ^= s_rxBuffer[i];
        }
        if (crc == s_rxBuffer[BT_PACKET_SIZE - 1]) {
            if (s_rxBuffer[0] == TRAINER_FRAME) {
                processTrainerFrame(s_rxBuffer);
            }
        }
        s_rxState = STATE_IDLE;
    }
}

/**
 * @brief Process a received BLE data frame (multiple bytes)
 */
static void processFrame(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        processByte(data[i]);
    }
}

// ─── FrSky frame encoder (for Peripheral mode: send to radio) ───────
static uint8_t s_txBuffer[BT_LINE_LENGTH + 1];
static uint8_t s_txIndex = 0;
static uint8_t s_txCrc   = 0;

static void txPushByte(uint8_t byte) {
    s_txCrc ^= byte;
    if (byte == START_STOP || byte == BYTE_STUFF) {
        s_txBuffer[s_txIndex++] = BYTE_STUFF;
        byte ^= STUFF_MASK;
    }
    s_txBuffer[s_txIndex++] = byte;
}

/**
 * @brief Build a FrSky trainer frame from channel values
 * @return Length of encoded frame
 */
static uint8_t buildTrainerFrame(uint8_t* out, const uint16_t* channels) {
    s_txIndex = 0;
    s_txCrc   = 0;

    s_txBuffer[s_txIndex++] = START_STOP;
    txPushByte(TRAINER_FRAME);

    for (uint8_t ch = 0; ch < BT_CHANNELS; ch += 2) {
        uint16_t v1 = channels[ch];
        uint16_t v2 = channels[ch + 1];
        txPushByte(v1 & 0xFF);
        txPushByte(((v1 & 0x0F00) >> 4) | ((v2 & 0x00F0) >> 4));
        txPushByte(((v2 & 0x000F) << 4) | ((v2 & 0x0F00) >> 8));
    }

    s_txBuffer[s_txIndex++] = s_txCrc;
    s_txBuffer[s_txIndex++] = START_STOP;

    memcpy(out, s_txBuffer, s_txIndex);
    return s_txIndex;
}

// ═══════════════════════════════════════════════════════════════════
// PERIPHERAL MODE CALLBACKS
// ═══════════════════════════════════════════════════════════════════

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        s_connected  = true;
        s_connHandle = desc->conn_handle;
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 desc->peer_ota_addr.val[5], desc->peer_ota_addr.val[4],
                 desc->peer_ota_addr.val[3], desc->peer_ota_addr.val[2],
                 desc->peer_ota_addr.val[1], desc->peer_ota_addr.val[0]);
        strlcpy(s_remoteAddr, addr, sizeof(s_remoteAddr));
        LOG_I("BLE", "Peripheral: client connected from %s", s_remoteAddr);
    }

    void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        s_connected = false;
        s_remoteAddr[0] = '\0';
        LOG_I("BLE", "Peripheral: client disconnected");

        // Restart advertising
        NimBLEDevice::startAdvertising();
    }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pChar) override {
        // Data received from radio (when radio is Central)
        const uint8_t* data = pChar->getValue().data();
        size_t len          = pChar->getValue().length();
        if (len > 0) {
            processFrame(data, len);
        }
    }
};

static ServerCallbacks s_serverCB;
static CharCallbacks   s_charCB;

// ═══════════════════════════════════════════════════════════════════
// CENTRAL MODE CALLBACKS
// ═══════════════════════════════════════════════════════════════════

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        s_connected = true;
        LOG_I("BLE", "Central: connected to %s",
              pClient->getPeerAddress().toString().c_str());
    }

    void onDisconnect(NimBLEClient* pClient) override {
        s_connected = false;
        s_remoteAddr[0] = '\0';
        LOG_I("BLE", "Central: disconnected");
    }
};

static ClientCallbacks s_clientCB;

/**
 * @brief Notification callback when Central receives data from remote device
 */
static void centralNotifyCB(NimBLERemoteCharacteristic* pChar,
                            uint8_t* data, size_t length, bool isNotify) {
    if (isNotify && length > 0) {
        processFrame(data, length);
    }
}

// ═══════════════════════════════════════════════════════════════════
// SCAN CALLBACK
// ═══════════════════════════════════════════════════════════════════

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        if (s_scanCount >= MAX_SCAN_RESULTS) return;

        // Check if already in list
        std::string addrStr = advertisedDevice->getAddress().toString();
        for (uint8_t i = 0; i < s_scanCount; i++) {
            if (strcmp(s_scanResults[i].address, addrStr.c_str()) == 0) return;
        }

        BleScanResult& r = s_scanResults[s_scanCount];
        strlcpy(r.address, addrStr.c_str(), sizeof(r.address));
        r.rssi = advertisedDevice->getRSSI();

        if (advertisedDevice->haveName()) {
            strlcpy(r.name, advertisedDevice->getName().c_str(), sizeof(r.name));
        } else {
            r.name[0] = '\0';
        }

        r.hasFrskyService = advertisedDevice->isAdvertisingService(FRSKY_SERVICE_UUID);
        r.addrType        = advertisedDevice->getAddress().getType();

        s_scanCount++;
        LOG_D("BLE", "Scan found: %s RSSI=%d name=%s frsky=%d",
              r.address, r.rssi, r.name, r.hasFrskyService);
    }
};

static ScanCallbacks s_scanCB;

/**
 * @brief Scan complete callback (function pointer for NimBLE v1.4)
 */
static void onScanComplete(NimBLEScanResults results) {
    s_scanning = false;
    LOG_I("BLE", "Scan complete, %d devices found", s_scanCount);
}

// ═══════════════════════════════════════════════════════════════════
// PERIPHERAL MODE INIT
// ═══════════════════════════════════════════════════════════════════

static void initPeripheral() {
    LOG_I("BLE", "Initializing Peripheral mode");

    s_pServer = NimBLEDevice::createServer();
    s_pServer->setCallbacks(&s_serverCB);

    // Create FrSky service
    NimBLEService* pService = s_pServer->createService(FRSKY_SERVICE_UUID);

    // Create trainer characteristic (read + write + notify)
    s_pCharTx = pService->createCharacteristic(
        FRSKY_CHAR_UUID,
        NIMBLE_PROPERTY::READ |
        NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::WRITE_NR |
        NIMBLE_PROPERTY::NOTIFY
    );
    s_pCharTx->setCallbacks(&s_charCB);

    pService->start();

    // Configure advertising
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(FRSKY_SERVICE_UUID);
    pAdv->setName(g_config.btName);
    pAdv->setScanResponse(true);
    pAdv->start();

    LOG_I("BLE", "Peripheral advertising as '%s'", g_config.btName);
}

// ═══════════════════════════════════════════════════════════════════
// CENTRAL MODE INIT
// ═══════════════════════════════════════════════════════════════════

static void initCentral() {
    LOG_I("BLE", "Initializing Central mode");
    // Central mode just initializes NimBLE; connection happens via bleConnectTo()
}

// ═══════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════

void bleInit() {
    if (s_initialized) return;

    NimBLEDevice::init(g_config.btName);
    NimBLEDevice::setMTU(87);  // Enough for trainer frames
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Get local address
    strlcpy(s_localAddr,
            NimBLEDevice::getAddress().toString().c_str(),
            sizeof(s_localAddr));
    LOG_I("BLE", "Local address: %s", s_localAddr);

    // Cache the local address in config (hardware-fixed, rarely changes)
    if (strcmp(g_config.localBtAddr, s_localAddr) != 0) {
        strlcpy(g_config.localBtAddr, s_localAddr, sizeof(g_config.localBtAddr));
        configSave();
    }

    if (bleIsCentral(g_config.deviceMode)) {
        initCentral();
        if (g_config.hasRemoteAddr && strlen(g_config.remoteBtAddr) > 0) {
            LOG_I("BLE", "Auto-connecting to saved address: %s",
                  g_config.remoteBtAddr);
        }
    } else {
        initPeripheral();
    }

    s_initialized = true;
}

bool bleIsInitialized() {
    return s_initialized;
}

void bleStop() {
    if (!s_initialized) return;

    // Kill the connect task before tearing down the stack.
    // vTaskDelete() stops it immediately; deinit() cleans NimBLE state.
    if (s_connectTaskHandle != nullptr) {
        vTaskDelete(s_connectTaskHandle);
        s_connectTaskHandle = nullptr;
        s_connectInProgress = false;
        LOG_D("BLE", "Connect task killed for reinit");
    }

    if (s_pClient && s_connected) {
        s_pClient->disconnect();
    }

    NimBLEDevice::deinit(true);

    // Clear all pointers — deinit() freed the underlying objects
    s_pServer     = nullptr;
    s_pCharTx     = nullptr;
    s_pClient     = nullptr;
    s_pRemoteChar = nullptr;

    s_initialized = false;
    s_connected   = false;
    s_scanning    = false;
    s_remoteAddr[0] = '\0';
    LOG_I("BLE", "Stopped");
}

void bleScheduleReinit() {
    s_pendingReinit = true;
}

void bleUpdateAdvertisingName() {
    if (!s_initialized) return;
    if (bleIsCentral(g_config.deviceMode)) return;  // only peripherals advertise
    if (webUiIsActive()) return;
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->stop();
    pAdv->setName(g_config.btName);
    pAdv->start();
    LOG_I("BLE", "Advertising name updated to '%s'", g_config.btName);
}

/**
 * @brief Minimal NimBLE init for on-demand operations in AP mode.
 *        Only starts the controller — no advertising, no role setup.
 *
 *        IMPORTANT: webUiInit() sets WIFI_PS_NONE for AP beacon stability.
 *        The BLE coex scheduler on ESP32-C3 needs to pause WiFi for BLE
 *        time slots, so we must switch to WIFI_PS_MIN_MODEM before init
 *        and allow the radio to settle before any BLE operations.
 */
static void ensureController() {
    if (s_initialized) return;

    // Switch from WIFI_PS_NONE to MIN_MODEM so the coex scheduler can
    // arbitrate WiFi/BLE on the shared radio without aborting.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    delay(100);  // let the radio mode transition settle

    NimBLEDevice::init(g_config.btName);
    NimBLEDevice::setMTU(87);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // full power — same as bleInit()
    strlcpy(s_localAddr,
            NimBLEDevice::getAddress().toString().c_str(),
            sizeof(s_localAddr));
    // Cache local address in config (hardware-fixed)
    if (strcmp(g_config.localBtAddr, s_localAddr) != 0) {
        strlcpy(g_config.localBtAddr, s_localAddr, sizeof(g_config.localBtAddr));
        configSave();
    }

    delay(200);  // let coex + NimBLE host stabilize before scan/connect
    LOG_I("BLE", "Controller started on-demand: %s", s_localAddr);
    s_initialized = true;
}

void bleLoop() {
    if (!s_initialized) return;

    // Deferred reinit (name or role changed from web UI)
    if (s_pendingReinit && !s_connectInProgress) {
        s_pendingReinit = false;
        LOG_I("BLE", "Reinit requested — restarting BLE stack");
        bleStop();
        bleInit();
        return;
    }

    // In Central mode: handle auto-reconnect.
    // Suppressed while the WebUI/AP is active: BLE connect attempts every 5s
    // monopolise the 2.4 GHz radio and cause the WiFi AP to disappear.
    // The user can connect manually from the WebUI instead.
    if (bleIsCentral(g_config.deviceMode) && !s_connected && !s_scanning &&
        !s_connectInProgress && s_autoReconnect && !s_pendingReinit &&
        !webUiIsActive()) {
        static uint32_t lastReconnectAttempt = 0;
        if (g_config.hasRemoteAddr && strlen(g_config.remoteBtAddr) > 0) {
            if (millis() - lastReconnectAttempt > 5000) {
                lastReconnectAttempt = millis();
                // s_pendingAddrType will be filled from config inside bleConnectTo
                bleConnectTo(g_config.remoteBtAddr);
            }
        }
    }

    // In Peripheral mode: send channel data as notifications if connected
    if (!bleIsCentral(g_config.deviceMode) && s_connected && s_pCharTx) {
        if (g_channelData.newData) {
            uint16_t channels[BT_CHANNELS];
            g_channelData.getChannels(channels, BT_CHANNELS);
            g_channelData.newData = false;

            uint8_t frame[BT_LINE_LENGTH + 1];
            uint8_t len = buildTrainerFrame(frame, channels);
            s_pCharTx->setValue(frame, len);
            s_pCharTx->notify();
        }
    }
}

void bleSendRawNotification(const uint8_t* data, size_t len) {
    if (!s_connected || !s_pCharTx || !data || len == 0) return;
    s_pCharTx->setValue(data, len);
    s_pCharTx->notify();
}

bool bleScanStart() {
    if (s_scanning) return false;

    // Lazy-init BLE controller if not already running (AP mode)
    ensureController();

    if (!bleIsCentral(g_config.deviceMode)) {
        LOG_I("BLE", "Scan only available in Central mode");
        return false;
    }

    s_scanCount = 0;
    s_scanning  = true;

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&s_scanCB);
    pScan->setActiveScan(true);   // Active scan: sends scan-req to get device names from scan response
    pScan->setInterval(100);
    pScan->setWindow(60);
    pScan->setMaxResults(0);           // Don't store in NimBLE, we handle it
    pScan->start(5, onScanComplete);   // 5 seconds, non-blocking with callback

    LOG_I("BLE", "Scan started (5s) interval=100 window=60");
    return true;
}

void bleScanStop() {
    NimBLEDevice::getScan()->stop();
    s_scanning = false;
}

bool bleIsScanning() {
    return s_scanning;
}

uint8_t bleGetScanResults(BleScanResult* results, uint8_t maxCount) {
    uint8_t count = min(s_scanCount, maxCount);
    memcpy(results, s_scanResults, count * sizeof(BleScanResult));
    return count;
}

/**
 * @brief FreeRTOS task that performs the blocking BLE connect + service discovery.
 *        Runs on a separate task so loopTask stays free for async_tcp.
 */
static void bleConnectTask(void* /*param*/) {
    const char* address = s_pendingAddr;

    if (!s_pClient) {
        s_pClient = NimBLEDevice::createClient();
        s_pClient->setClientCallbacks(&s_clientCB);
    }

    s_pClient->setConnectionParams(6, 12, 0, 100);
    s_pClient->setConnectTimeout(8);

    uint8_t usedAddrType = s_pendingAddrType;
    NimBLEAddress addr(address, usedAddrType);

    if (!s_pClient->connect(addr)) {
        // Retry once with the alternate address type (public <-> random).
        // Some peripherals advertise/connect with a type that may differ from
        // cached data or scanner reports depending on stack behavior.
        uint8_t altType = (usedAddrType == 0) ? 1 : 0;
        LOG_W("BLE", "Connect failed with addrType=%u, retrying with addrType=%u",
              usedAddrType, altType);

        // Let controller/link-layer settle before retrying with alternate type.
        if (s_pClient->isConnected()) {
            s_pClient->disconnect();
        }
        delay(120);

        NimBLEAddress altAddr(address, altType);
        if (!s_pClient->connect(altAddr)) {
            LOG_E("BLE", "Connection failed (addrType=%u and addrType=%u)",
                  usedAddrType, altType);
            s_connectInProgress = false;
            s_connectTaskHandle = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        usedAddrType = altType;
        LOG_I("BLE", "Connected using fallback addrType=%u", usedAddrType);
    }

    if (!s_pClient->isConnected()) {
        LOG_E("BLE", "Connection failed: client not connected after connect()") ;
        s_connectInProgress = false;
        s_connectTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Discover FrSky service
    NimBLERemoteService* pService = s_pClient->getService(FRSKY_SERVICE_UUID);
    if (!pService) {
        LOG_E("BLE", "FrSky service not found on remote device");
        s_pClient->disconnect();
        s_connectInProgress = false;
        s_connectTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Get trainer characteristic
    s_pRemoteChar = pService->getCharacteristic(FRSKY_CHAR_UUID);
    if (!s_pRemoteChar) {
        LOG_E("BLE", "FrSky characteristic not found");
        s_pClient->disconnect();
        s_connectInProgress = false;
        s_connectTaskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Subscribe to notifications
    if (s_pRemoteChar->canNotify()) {
        if (!s_pRemoteChar->subscribe(true, centralNotifyCB)) {
            LOG_E("BLE", "Failed to subscribe to notifications");
            s_pClient->disconnect();
            s_connectInProgress = false;
            s_connectTaskHandle = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        LOG_I("BLE", "Subscribed to trainer notifications");
    }

    strlcpy(s_remoteAddr, address, sizeof(s_remoteAddr));

    // Save connected address and type for auto-reconnect
    strlcpy(g_config.remoteBtAddr, address, sizeof(g_config.remoteBtAddr));
    g_config.hasRemoteAddr  = true;
    g_config.remoteAddrType = usedAddrType;
    configSave();

    LOG_I("BLE", "Connected to %s (addrType=%u) successfully", address, usedAddrType);
    // s_connected is set by ClientCallbacks::onConnect callback
    s_connectInProgress = false;
    s_connectTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool bleConnectTo(const char* address) {
    if (s_connected) {
        LOG_W("BLE", "Already connected, disconnect first");
        return false;
    }
    if (s_connectInProgress) {
        LOG_W("BLE", "Connection already in progress");
        return false;
    }

    // Lazy-init BLE controller if not already running (AP mode)
    ensureController();

    // Shared radio (WiFi + BLE): keep WiFi in MIN_MODEM while connecting
    // so coexistence scheduler can arbitrate time slices reliably.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    LOG_I("BLE", "Connecting to %s...", address);

    strlcpy(s_pendingAddr, address, sizeof(s_pendingAddr));
    s_autoReconnect = true;  // explicit connect restores auto-reconnect

    // Look up address type from recent scan results; fall back to saved config
    // only if the device was not found in the scan results.
    bool foundInScan = false;
    s_pendingAddrType = 0;
    for (uint8_t i = 0; i < s_scanCount; i++) {
        if (strcmp(s_scanResults[i].address, address) == 0) {
            s_pendingAddrType = s_scanResults[i].addrType;
            foundInScan = true;
            break;
        }
    }
    // If not in scan results (e.g. auto-reconnect), use saved config type
    if (!foundInScan && g_config.hasRemoteAddr &&
        strcmp(g_config.remoteBtAddr, address) == 0) {
        s_pendingAddrType = g_config.remoteAddrType;
    }
    LOG_D("BLE", "Address type resolved: %u", s_pendingAddrType);

    s_connectInProgress = true;

    // Spawn a dedicated task — connect() blocks for up to 5 s; running it here
    // instead of in loopTask prevents the async_tcp watchdog from firing.
    BaseType_t rc = xTaskCreate(bleConnectTask, "bleConnect", 4096,
                                nullptr, 2, &s_connectTaskHandle);
    if (rc != pdPASS) {
        LOG_E("BLE", "Failed to create connect task (rc=%d)", (int)rc);
        s_connectInProgress = false;
        return false;
    }
    return true;
}

void bleDisconnect() {
    s_autoReconnect = false;  // user explicitly disconnected — don't auto-reconnect
    if (s_pClient && s_connected) {
        s_pClient->disconnect();
    }
    s_connected = false;
}

void bleForget() {
    // Disconnect if connected, then erase the saved remote address
    if (s_pClient && s_connected) {
        s_pClient->disconnect();
    }
    s_autoReconnect = false;
    g_config.hasRemoteAddr = false;
    g_config.remoteBtAddr[0] = '\0';
    configSave();
    LOG_I("BLE", "Saved address forgotten");
}

void bleKickClient() {
    if (!bleIsCentral(g_config.deviceMode) && s_connected && s_pServer &&
        s_connHandle != BLE_HS_CONN_HANDLE_NONE) {
        s_pServer->disconnect(s_connHandle);
        LOG_I("BLE", "Peripheral: kicked connected client");
    }
}

bool bleIsConnected() {
    return s_connected;
}

bool bleIsConnecting() {
    return s_connectInProgress;
}

const char* bleGetLocalAddress() {
    return s_localAddr;
}

const char* bleGetRemoteAddress() {
    return s_remoteAddr;
}

int bleGetRSSI() {
    if (s_pClient && s_connected) {
        return s_pClient->getRssi();
    }
    return -127;
}
