/**
 * @file elrs_espnow.cpp
 * @brief ELRS Backpack ESP-NOW head tracking receiver
 *
 * Ported from standalone ELRS-Headtracker-to-SBUS project.
 * Receives MSP_ELRS_SET_PTR packets via ESP-NOW and converts
 * CRSF 11-bit values to PPM range for g_channelData.
 */

#include "elrs_espnow.h"
#include "msp.h"
#include "config.h"
#include "channel_data.h"
#include "log.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/md5.h>

// ─── Constants ──────────────────────────────────────────────────────
static constexpr uint32_t PTR_TIMEOUT_MS   = 1000;  // Failsafe after 1s silence
static constexpr uint32_t ENABLE_INTERVAL  = 2000;  // Re-send enable every 2s

// CRSF channel range (11-bit)
static constexpr uint16_t CRSF_MIN = 191;
static constexpr uint16_t CRSF_MAX = 1792;

// ─── Module state ───────────────────────────────────────────────────
static bool     s_running       = false;
static uint8_t  s_uid[6]        = {};
static uint32_t s_lastEnableMs  = 0;
static bool     s_wasActive     = false;

// MSP parser (used only in callback context)
static MspParser s_msp;

// Shared state: written from ESP-NOW callback, read from loop()
static portMUX_TYPE s_ptrMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t s_ptrCh[3]       = {};
static volatile uint32_t s_lastPtrMs      = 0;
static volatile bool     s_sendHTEnable   = false;
static volatile uint32_t s_ptrPacketCount = 0;

// ─── UID generation from binding phrase ─────────────────────────────
// Replicates ELRS build system: MD5('-DMY_BINDING_PHRASE="<phrase>"')[0:6]
static void generateUID(const char* phrase, uint8_t* out) {
    char buf[128];
    snprintf(buf, sizeof(buf), "-DMY_BINDING_PHRASE=\"%s\"", phrase);

    uint8_t hash[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts_ret(&ctx);
    mbedtls_md5_update_ret(&ctx, (const uint8_t*)buf, strlen(buf));
    mbedtls_md5_finish_ret(&ctx, hash);
    mbedtls_md5_free(&ctx);

    memcpy(out, hash, 6);
    out[0] &= ~0x01;  // Ensure unicast (clear LSB of first byte)
}

// ─── CRSF to PPM conversion ────────────────────────────────────────
// CRSF (191..1792, center 992) → PPM (1050..1950, center 1500)
static inline uint16_t crsfToPpm(uint16_t crsf) {
    int32_t v = (int32_t)crsf - (int32_t)CRSF_MIN;
    v = (int32_t)CHANNEL_MIN + v * (int32_t)CHANNEL_RANGE / (int32_t)(CRSF_MAX - CRSF_MIN);
    if (v < (int32_t)CHANNEL_MIN) v = CHANNEL_MIN;
    if (v > (int32_t)CHANNEL_MAX) v = CHANNEL_MAX;
    return (uint16_t)v;
}

// ─── Send MSP via ESP-NOW ───────────────────────────────────────────
static void sendMspEspNow(uint16_t function, const uint8_t* data, uint16_t len) {
    uint8_t frame[32];
    uint8_t frameLen = mspBuildCommand(frame, sizeof(frame), function, data, len);
    if (frameLen) esp_now_send(s_uid, frame, frameLen);
}

static void sendHeadTrackingEnable() {
    uint8_t enable = 1;
    sendMspEspNow(MSP_ELRS_SET_HEAD_TRACKING, &enable, 1);
    LOG_I("ELRS", "Sent SET_HEAD_TRACKING enable");
}

// ─── ESP-NOW receive callback (runs in WiFi task context) ───────────
static void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (memcmp(mac, s_uid, 6) != 0) return;

    s_msp.reset();
    for (int i = 0; i < len; i++) {
        if (s_msp.feed(data[i])) {
            if (s_msp.function == MSP_ELRS_SET_PTR && s_msp.size == 6) {
                portENTER_CRITICAL(&s_ptrMux);
                s_ptrCh[0] = min(s_msp.payloadU16(0), (uint16_t)2047);
                s_ptrCh[1] = min(s_msp.payloadU16(2), (uint16_t)2047);
                s_ptrCh[2] = min(s_msp.payloadU16(4), (uint16_t)2047);
                s_lastPtrMs = millis();
                portEXIT_CRITICAL(&s_ptrMux);
                s_ptrPacketCount++;
            }
            else if (s_msp.function == MSP_ELRS_REQU_VTX_PKT) {
                s_sendHTEnable = true;
            }
        }
    }
}

// ─── Public API ─────────────────────────────────────────────────────

void elrsInit() {
    if (s_running) return;

    LOG_I("ELRS", "Initializing ESP-NOW head tracking receiver");

    // Generate UID from binding phrase
    generateUID(g_config.elrsBindPhrase, s_uid);
    LOG_I("ELRS", "Bind phrase: \"%s\"  UID: %02X:%02X:%02X:%02X:%02X:%02X",
          g_config.elrsBindPhrase,
          s_uid[0], s_uid[1], s_uid[2], s_uid[3], s_uid[4], s_uid[5]);
    LOG_I("ELRS", "PTR channels: CH1/CH2/CH3 (Pan/Tilt/Roll)");

    // Reset shared state
    portENTER_CRITICAL(&s_ptrMux);
    s_ptrCh[0] = 0; s_ptrCh[1] = 0; s_ptrCh[2] = 0;
    s_lastPtrMs = 0;
    s_sendHTEnable = false;
    portEXIT_CRITICAL(&s_ptrMux);
    s_ptrPacketCount = 0;
    s_lastEnableMs = 0;
    s_wasActive = false;

    // WiFi STA mode for ESP-NOW on channel 1
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    WiFi.begin("", "", 1);
    WiFi.disconnect();

    // Set MAC to UID (required for ESP-NOW addressing)
    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_STA, s_uid);
    if (macErr != ESP_OK) {
        LOG_E("ELRS", "Failed to set MAC: %s", esp_err_to_name(macErr));
    }

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        LOG_E("ELRS", "ESP-NOW init failed — restarting");
        ESP.restart();
    }

    // Add VRx as peer (addressed by same UID)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, s_uid, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_err_t peerErr = esp_now_add_peer(&peer);
    if (peerErr != ESP_OK) {
        LOG_E("ELRS", "ESP-NOW add peer failed: %s", esp_err_to_name(peerErr));
    }

    esp_now_register_recv_cb(onEspNowRecv);

    s_running = true;
    LOG_I("ELRS", "ESP-NOW receiver active, waiting for head tracker...");
}

void elrsLoop() {
    if (!s_running) return;

    uint32_t now = millis();

    // Respond to VRx requesting cached state
    if (s_sendHTEnable) {
        s_sendHTEnable = false;
        sendHeadTrackingEnable();
        s_lastEnableMs = now;
    }

    // Snapshot shared state under lock
    portENTER_CRITICAL(&s_ptrMux);
    uint16_t localPtr[3] = { s_ptrCh[0], s_ptrCh[1], s_ptrCh[2] };
    uint32_t localPtrMs  = s_lastPtrMs;
    portEXIT_CRITICAL(&s_ptrMux);

    bool active = localPtrMs > 0 && (now - localPtrMs) < PTR_TIMEOUT_MS;

    // Periodically send enable until PTR data arrives
    if (!active && (now - s_lastEnableMs >= ENABLE_INTERVAL)) {
        sendHeadTrackingEnable();
        s_lastEnableMs = now;
    }

    // Log state transitions
    if (active != s_wasActive) {
        s_wasActive = active;
        if (active)
            LOG_I("ELRS", "Head tracker connected (packets: %lu)", s_ptrPacketCount);
        else if (localPtrMs > 0)
            LOG_I("ELRS", "Head tracker lost — failsafe (last packet %lums ago)",
                  now - localPtrMs);
    }

    // Write channel data when active
    if (active) {
        uint16_t channels[BT_CHANNELS];
        g_channelData.getChannels(channels, BT_CHANNELS);

        channels[0] = crsfToPpm(localPtr[0]);  // Pan  → CH1
        channels[1] = crsfToPpm(localPtr[1]);  // Tilt → CH2
        channels[2] = crsfToPpm(localPtr[2]);  // Roll → CH3

        g_channelData.setChannels(channels, BT_CHANNELS);
    }
}

bool elrsIsReceiving() {
    if (!s_running) return false;
    uint32_t lastMs;
    portENTER_CRITICAL(&s_ptrMux);
    lastMs = s_lastPtrMs;
    portEXIT_CRITICAL(&s_ptrMux);
    if (lastMs == 0) return false;
    return (millis() - lastMs) < PTR_TIMEOUT_MS;
}
