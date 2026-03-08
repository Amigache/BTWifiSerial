/**
 * @file lua_serial.h
 * @brief EdgeTX LUA Serial mode — bidirectional channel + command protocol
 *
 * Wire the ESP32-C3 AUX serial port (GPIO21 TX / GPIO20 RX) to the EdgeTX
 * radio's AUX port configured as "LUA" at 115200 baud, 8N1.
 *
 * Protocol summary
 * ─────────────────
 * ESP32 → Radio  (channel frame, ~50 Hz):
 *   [0xAA] [0x43] [ch1_H ch1_L] … [ch8_H ch8_L] [XOR_CRC]   (19 bytes)
 *   Values are signed int16 in trainer units: −1024 … 0 (centre) … +1024
 *   XOR_CRC = XOR of bytes [1..17] (type byte + 16 channel bytes)
 *
 * ESP32 → Radio  (status frame, every 500 ms):
 *   [0xAA] [0x53] [STATUS] [XOR_CRC]                          (4 bytes)
 *   STATUS bit0 = BLE connected
 *   XOR_CRC = XOR of bytes [1..2]
 *
 * Radio → ESP32  (command frame from LUA script):
 *   [0xAA] [0x02] [CMD] [XOR_CRC]                             (4 bytes)
 *   CMD 0x01 = toggle AP mode (writes NVS flag + ESP.restart())
 *   CMD 0x02 = AP mode ON
 *   CMD 0x03 = AP mode OFF (back to normal)
 *   CMD 0x20 = Device mode → Trainer IN
 *   CMD 0x21 = Device mode → Trainer OUT
 *   CMD 0x22 = Device mode → Telemetry
 *   XOR_CRC = XOR of bytes [1..2]
 */

#pragma once

#include <Arduino.h>

void luaSerialInit();
void luaSerialLoop();
void luaSerialStop();
void luaSerialSetApMode(uint8_t mode);  // 0=normal, 1=AP-block (Lua overlay), 2=telemetry-AP (no block)
