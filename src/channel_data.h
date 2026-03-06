/**
 * @file channel_data.h
 * @brief Thread-safe shared channel data between BLE and serial output
 *
 * This module provides a lock-free channel data buffer shared between
 * the BLE receive task and the serial output (FrSky/SBUS) task.
 * Channel values use the FrSky/OpenTX convention: 0-2048 center at 1024.
 */

#pragma once

#include <Arduino.h>

// ─── Channel definitions ────────────────────────────────────────────
static constexpr uint8_t  BT_CHANNELS       = 8;
// HeadTracker BLE encodes channels as PPM-style microsecond values.
// Real hardware range observed: 1050 (-100%) .. 1500 (centre) .. 1950 (+100%)
static constexpr uint16_t CHANNEL_CENTER     = 1500;
static constexpr uint16_t CHANNEL_MIN        = 1050;
static constexpr uint16_t CHANNEL_MAX        = 1950;
static constexpr uint16_t CHANNEL_RANGE      = 900;   // CHANNEL_MAX - CHANNEL_MIN

// SBUS uses a different range: 172..1811 center 992
static constexpr uint16_t SBUS_CENTER        = 992;
static constexpr uint16_t SBUS_MIN           = 172;
static constexpr uint16_t SBUS_MAX           = 1811;
static constexpr uint8_t  SBUS_NUM_CHANNELS  = 16;

/**
 * @brief Shared channel data structure with atomic flag for new data
 */
struct ChannelData {
    volatile uint16_t channels[BT_CHANNELS];
    volatile bool     newData;
    volatile uint32_t lastUpdateMs;

    void init() {
        for (uint8_t i = 0; i < BT_CHANNELS; i++) {
            channels[i] = CHANNEL_CENTER;  // 1500 = PPM centre
        }
        newData      = false;
        lastUpdateMs = 0;
    }

    /**
     * @brief Write channel values (called from BLE task)
     */
    void setChannels(const uint16_t* vals, uint8_t count) {
        uint8_t n = min(count, BT_CHANNELS);
        for (uint8_t i = 0; i < n; i++) {
            channels[i] = vals[i];
        }
        lastUpdateMs = millis();
        newData      = true;
    }

    /**
     * @brief Read channel values (called from serial output task)
     */
    void getChannels(uint16_t* out, uint8_t count) const {
        uint8_t n = min(count, BT_CHANNELS);
        for (uint8_t i = 0; i < n; i++) {
            out[i] = channels[i];
        }
    }

    /**
     * @brief Check if data is stale (no update in timeoutMs)
     */
    bool isStale(uint32_t timeoutMs = 1000) const {
        if (lastUpdateMs == 0) return true;
        return (millis() - lastUpdateMs) > timeoutMs;
    }

    /**
     * @brief Map a single channel from HeadTracker PPM range to SBUS range
     *
     * HeadTracker encodes: 1000 (-100%) .. 1500 (centre) .. 2000 (+100%)
     * SBUS:                 172 (-100%) ..  992 (centre) .. 1811 (+100%)
     */
    static uint16_t frskyToSbus(uint16_t frskyVal) {
        int32_t v = (int32_t)frskyVal - (int32_t)CHANNEL_MIN;
        v = (int32_t)SBUS_MIN + v * (int32_t)(SBUS_MAX - SBUS_MIN) / (int32_t)CHANNEL_RANGE;
        if (v < SBUS_MIN) v = SBUS_MIN;
        if (v > SBUS_MAX) v = SBUS_MAX;
        return (uint16_t)v;
    }
};

// Global shared instance
extern ChannelData g_channelData;
