/**
 * @file elrs_espnow.h
 * @brief ELRS Backpack ESP-NOW head tracking receiver
 *
 * Emulates an ELRS Backpack device to receive head tracking data
 * (pan/tilt/roll) from a VRx module via ESP-NOW. Writes converted
 * channel values into g_channelData for output via SBUS/FrSky/LuaSerial.
 *
 * Uses WiFi STA mode for ESP-NOW — no BLE or WiFi AP while active.
 */

#pragma once

#include <Arduino.h>

void elrsInit();          // WiFi STA + ESP-NOW init + start enable handshake
void elrsLoop();          // Periodic enable, failsafe check, feed g_channelData
bool elrsIsReceiving();   // True if PTR packets arriving within timeout
