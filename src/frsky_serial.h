/**
 * @file frsky_serial.h
 * @brief FrSky CC2540 serial emulation for EdgeTX radios
 *
 * Emulates the serial protocol used by the FrSky CC2540 Bluetooth module.
 * Communicates with the radio via UART at 115200 baud 8N1.
 *
 * The radio sends and receives FrSky trainer frames over UART.
 * This module reads channel data from the shared buffer and sends it
 * as FrSky frames, and also decodes incoming frames from the radio.
 */

#pragma once

#include <Arduino.h>

void frskySerialInit();
void frskySerialLoop();   // Non-blocking, call from main loop
void frskySerialStop();
