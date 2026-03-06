/**
 * @file sbus_output.h
 * @brief SBUS serial output for EdgeTX radios (TX16S MK3+)
 *
 * SBUS Protocol:
 *   - 100000 baud, 8E2 (8 data bits, even parity, 2 stop bits)
 *   - Inverted serial signal
 *   - 25-byte frame every ~14ms
 *   - Frame: [0x0F] [16×11-bit channels in 22 bytes] [flags] [0x00]
 */

#pragma once

#include <Arduino.h>

void sbusInit();
void sbusLoop();    // Non-blocking, call from main loop
void sbusStop();
