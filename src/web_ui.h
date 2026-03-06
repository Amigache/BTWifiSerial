/**
 * @file web_ui.h
 * @brief WiFi AP mode with WebSocket-based configuration UI and OTA
 *
 * Activated by a short press on the BOOT button.
 * Provides:
 *   - mDNS at btwifiserial.local
 *   - WebSocket for real-time configuration
 *   - Embedded HTML/CSS/JS UI
 *   - OTA firmware update
 */

#pragma once

#include <Arduino.h>

void webUiInit();          // Start WiFi AP + web server
void webUiStop();          // Stop WiFi AP + web server
void webUiLoop();          // Non-blocking, handle WebSocket events
bool webUiIsActive();      // Is the WebUI currently running?
