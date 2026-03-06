/**
 * @file log.h
 * @brief Unified logging system for BTWifiSerial
 *
 * Usage:
 *   LOG_E("TAG", "error: %d", code);    // Error   - always shown
 *   LOG_W("TAG", "warning %s", msg);     // Warning - level >= 1
 *   LOG_I("TAG", "info: started");       // Info    - level >= 2
 *   LOG_D("TAG", "debug val=%d", val);   // Debug   - level >= 3
 *   LOG_V("TAG", "verbose byte=0x%02X"); // Verbose - level >= 4
 *
 * Compile-time control:
 *   -D LOG_LEVEL=3   in build_flags (default: 2 = INFO)
 *   -D DEBUG_MODE=1  enable extra runtime debug output (sets level to 3)
 *
 * Runtime control:
 *   logSetLevel(3);         // change level at runtime
 *   logGetLevel();          // query current level
 *
 * Output format:
 *   [   1234][TAG] message
 *   (millis)(tag) formatted text
 */

#pragma once

#include <Arduino.h>

// ─── Log levels (prefixed to avoid collision with NimBLE's log_common.h) ───
#define LOGLVL_NONE    0
#define LOGLVL_ERROR   1
#define LOGLVL_WARN    2
#define LOGLVL_INFO    3
#define LOGLVL_DEBUG   4
#define LOGLVL_VERBOSE 5

// ─── Default compile-time level ─────────────────────────────────────
// LOG_LEVEL is an integer set via build_flags (-D LOG_LEVEL=4).
// If not set, default to INFO(3) normally, DEBUG(4) when DEBUG_MODE=1.
#ifndef LOG_LEVEL
  #ifdef DEBUG_MODE
    #define LOG_LEVEL LOGLVL_DEBUG
  #else
    #define LOG_LEVEL LOGLVL_INFO
  #endif
#endif

// ─── Runtime level (can be changed dynamically) ─────────────────────
extern uint8_t g_logLevel;

inline void   logSetLevel(uint8_t level) { g_logLevel = level; }
inline uint8_t logGetLevel()             { return g_logLevel; }

// ─── Core log macro ────────────────────────────────────────────────
// Only compiles in calls at or below the compile-time LOG_LEVEL.
// At runtime, g_logLevel provides additional filtering.
#define LOG_PRINT(level, levelChar, tag, fmt, ...) \
    do { \
        if (g_logLevel >= (level)) { \
            Serial.printf("[%7lu][%c][%s] " fmt "\n", \
                          millis(), levelChar, tag, ##__VA_ARGS__); \
        } \
    } while (0)

// ─── Level macros ──────────────────────────────────────────────────
#if LOG_LEVEL >= LOGLVL_ERROR
  #define LOG_E(tag, fmt, ...) LOG_PRINT(LOGLVL_ERROR, 'E', tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_E(tag, fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOGLVL_WARN
  #define LOG_W(tag, fmt, ...) LOG_PRINT(LOGLVL_WARN, 'W', tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_W(tag, fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOGLVL_INFO
  #define LOG_I(tag, fmt, ...) LOG_PRINT(LOGLVL_INFO, 'I', tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_I(tag, fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOGLVL_DEBUG
  #define LOG_D(tag, fmt, ...) LOG_PRINT(LOGLVL_DEBUG, 'D', tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_D(tag, fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= LOGLVL_VERBOSE
  #define LOG_V(tag, fmt, ...) LOG_PRINT(LOGLVL_VERBOSE, 'V', tag, fmt, ##__VA_ARGS__)
#else
  #define LOG_V(tag, fmt, ...) do {} while(0)
#endif

// ─── Hex dump helper (verbose level) ───────────────────────────────
#if LOG_LEVEL >= LOGLVL_VERBOSE
  #define LOG_HEX(tag, label, data, len) \
    do { \
        if (g_logLevel >= LOGLVL_VERBOSE) { \
            Serial.printf("[%7lu][V][%s] %s (%u bytes): ", millis(), tag, label, (unsigned)(len)); \
            for (size_t _i = 0; _i < (size_t)(len); _i++) { \
                Serial.printf("%02X ", ((const uint8_t*)(data))[_i]); \
            } \
            Serial.println(); \
        } \
    } while(0)
#else
  #define LOG_HEX(tag, label, data, len) do {} while(0)
#endif
