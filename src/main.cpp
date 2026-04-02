/**
 * @file main.cpp
 * @brief BTWifiSerial - Main firmware entry point
 *
 * Mode switching uses ESP.restart() + RTC_DATA_ATTR to avoid the
 * NimBLEDevice::deinit() heap crash caused by calling it from the
 * Arduino loop task while the NimBLE FreeRTOS task is still running.
 *
 * Boot button (GPIO9, active LOW):
 *   - Short press (<1s): save desired mode to RTC RAM â†’ restart
 *
 * LED (GPIO8, active LOW on SuperMini):
 *   - OFF:            Normal mode, no BLE connection
 *   - Solid ON:       Normal mode, BLE connected
 *   - Blink (500ms):  AP mode active (with or without BLE)
 *   - 3 rapid blinks: Mode toggle confirmed (before restart)
 */

#include <Arduino.h>
#include <Preferences.h>
#include "log.h"
#include "config.h"
#include "channel_data.h"
#include "ble_module.h"
#include "frsky_serial.h"
#include "sbus_output.h"
#include "sport_telemetry.h"
#include "lua_serial.h"
#include "web_ui.h"
#include "elrs_espnow.h"

// ───────── Runtime log level (defined here, declared extern in log.h) ─────────
uint8_t g_logLevel = LOG_LEVEL;

// ───────── Global channel data ────────────────────────────────────────────────
ChannelData g_channelData;

// ───────── Boot mode via NVS (RTC_DATA_ATTR does NOT survive ESP.restart on ESP32-C3) ─
static constexpr uint8_t BOOT_NORMAL   = 0;
static constexpr uint8_t BOOT_AP_MODE  = 1;
static constexpr uint8_t BOOT_TELEM_AP = 2;  // WiFi AP + web UI + LuaSerial (no BLE, Lua not blocked)
static constexpr uint8_t BOOT_STA_MODE = 3;  // WiFi STA + web UI + LuaSerial

static uint8_t readBootMode() {
    Preferences p;
    p.begin("btwboot", true);
    uint8_t mode = p.getUChar("mode", BOOT_NORMAL);
    p.end();
    return mode;
}

static void writeBootMode(uint8_t mode) {
    Preferences p;
    p.begin("btwboot", false);
    p.putUChar("mode", mode);
    p.end();
}

// ───────── Application state ──────────────────────────────────────────────────
enum class AppMode : uint8_t { NORMAL, AP_MODE, TELEM_AP, STA_MODE };
static AppMode s_appMode      = AppMode::NORMAL;
static bool    s_serialActive = false;

// ───────── Boot button debounce ───────────────────────────────────────────────
static constexpr uint32_t DEBOUNCE_MS     = 50;
static constexpr uint32_t SHORT_PRESS_MAX = 1000;

static bool     s_lastButtonState  = HIGH;
static bool     s_buttonState      = HIGH;
static uint32_t s_lastDebounceTime = 0;
static uint32_t s_buttonPressTime  = 0;
static bool     s_buttonHandled    = false;

// ───────── LED ────────────────────────────────────────────────────────────────
static uint32_t s_lastLedToggle = 0;
static bool     s_ledState      = false;

// ───────── Forward declarations ───────────────────────────────────────────────
static void startNormalMode();
static void startApMode();
static void startTelemetryApMode();
static void startStaMode();
static void stopSerialOutput();
static void startSerialOutput();
static void handleButton();
static void updateLed();
static void blinkLed(uint8_t times, uint32_t onMs, uint32_t offMs);
static void switchModeTo(AppMode next);
void mainSetTelemOutput(uint8_t output);   // save config + restart into telemetry AP or normal
void mainSetDeviceMode(uint8_t mode);      // save device mode + restart
void mainSetMirrorBaud(uint32_t baud);     // save mirror baud + restart
void mainSetWifiMode(uint8_t mode);        // save wifi mode + restart
void mainRequestConfigRestart();           // apply current config and restart

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void setup() {
    // USB CDC: wait up to 5s for host enumeration after reset.
    // After ESP.restart() Windows re-enumerates the CDC port which
    // takes 1-3s on most systems. Increase if you still miss early logs.
    Serial.begin(115200);
    {
        uint32_t t0 = millis();
        while (!Serial && (millis() - t0 < 5000)) {
            delay(10);
        }
        delay(300);  // extra stabilisation
    }

    Serial.println();
    LOG_I("MAIN", "========================================");
    LOG_I("MAIN", "  BTWifiSerial v1.0.0 - ESP32-C3");
    LOG_I("MAIN", "  Log level: %d  (E=1 W=2 I=3 D=4 V=5)", g_logLevel);
    LOG_I("MAIN", "========================================");

    // GPIO
    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);  // off (active LOW)

    // Channel data
    g_channelData.init();

    // Config
    configInit();

    // Determine boot mode from NVS flag
    uint8_t bootMode = readBootMode();
    // Always reset flag to NORMAL so next cold reboot starts in normal mode
    writeBootMode(BOOT_NORMAL);

    if (bootMode == BOOT_AP_MODE) {
        LOG_I("MAIN", "NVS: booting into AP mode");
        startApMode();
    } else if (bootMode == BOOT_TELEM_AP) {
        LOG_I("MAIN", "NVS: booting into Telemetry AP mode");
        startTelemetryApMode();
    } else if (bootMode == BOOT_STA_MODE) {
        LOG_I("MAIN", "NVS: booting into STA mode");
        startStaMode();
    } else {
        LOG_I("MAIN", "NVS: booting into NORMAL mode");
        startNormalMode();
    }

    LOG_I("MAIN", "Setup complete. Short press BOOT to toggle mode.");
    LOG_I("MAIN", "Free heap: %u bytes", ESP.getFreeHeap());
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

void loop() {
    handleButton();

    switch (s_appMode) {
        case AppMode::NORMAL:
            if (g_config.deviceMode == DeviceMode::ELRS_HT) {
                elrsLoop();
            } else {
                bleLoop();
            }
            if (s_serialActive) {
                switch (g_config.serialMode) {
                    case OutputMode::FRSKY:        frskySerialLoop();    break;
                    case OutputMode::SBUS:         sbusLoop();           break;
                    case OutputMode::SPORT_BT:
                    case OutputMode::SPORT_MIRROR: sportTelemetryLoop(); break;
                    case OutputMode::LUA_SERIAL:   luaSerialLoop();      break;
                }
            }
            break;

        case AppMode::AP_MODE:
            // Web UI always runs in AP mode.
            webUiLoop();
            // If LUA serial is active (serial mode = LUA_SERIAL), keep running
            // so the Lua script can detect AP mode and show the overlay modal.
            if (s_serialActive) luaSerialLoop();
            break;

        case AppMode::TELEM_AP:
            // Telemetry AP: WiFi AP + web UI available + LuaSerial active.
            // BLE is NOT running; Lua is not blocked (apMode=2 in T_CFG).
            webUiLoop();
            if (s_serialActive) luaSerialLoop();
            break;

        case AppMode::STA_MODE:
            // STA mode: connected to an existing WiFi network + web UI + LuaSerial.
            webUiLoop();
            if (s_serialActive) luaSerialLoop();
            break;
    }

    updateLed();
    yield();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// MODE MANAGEMENT
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

static void startNormalMode() {
    LOG_I("MAIN", ">>> NORMAL MODE <<<");
    LOG_D("MAIN", "Free heap: %u", ESP.getFreeHeap());

    // ELRS_HT mode: uses WiFi for ESP-NOW — no BLE, no WiFi AP/STA
    if (g_config.deviceMode == DeviceMode::ELRS_HT) {
        LOG_I("MAIN", "ELRS_HT mode: starting ESP-NOW receiver");
        elrsInit();
        startSerialOutput();
        s_appMode = AppMode::NORMAL;
        LOG_D("MAIN", "ELRS_HT mode running, heap: %u", ESP.getFreeHeap());
        return;
    }

    // LUA_SERIAL + WiFi UDP output → must run Telemetry AP mode
    // (WiFi AP + web UI + LuaSerial, no BLE).  Redirect here so the Lua script
    // always sees a consistent state regardless of whether BOOT_TELEM_AP was
    // written (e.g. after a web-UI Reboot, power cycle, or first-time flash).
    // Device mode is irrelevant: in LUA_SERIAL mode the channel data comes via
    // UART AUX, not BLE, so the BLE stack is not needed.
    if (g_config.serialMode == OutputMode::LUA_SERIAL &&
        g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
        LOG_I("MAIN", "LUA Serial + WiFi UDP: redirecting to Telemetry AP mode");
        startTelemetryApMode();
        return;
    }

    // WiFi Mode configured → redirect to the matching boot mode so the
    // device automatically enters AP or STA on every normal boot.
    if (g_config.wifiMode == WifiMode::AP) {
        LOG_I("MAIN", "WiFi Mode=AP: redirecting to AP mode");
        startApMode();
        return;
    }
    if (g_config.wifiMode == WifiMode::STA) {
        LOG_I("MAIN", "WiFi Mode=STA: redirecting to STA mode");
        startStaMode();
        return;
    }

    // The ESP32-C3 has a single shared radio: BLE and WiFi AP cannot run
    // simultaneously. Skip BLE init when the telemetry mode uses WiFi UDP
    // output — sportTelemetryInit() will start the AP instead.
    bool wifiTelemetry =
        (g_config.serialMode == OutputMode::SPORT_BT ||
         g_config.serialMode == OutputMode::SPORT_MIRROR) &&
        g_config.telemetryOutput == TelemetryOutput::WIFI_UDP;

    if (!wifiTelemetry) {
        bleInit();
    }
    startSerialOutput();

    s_appMode = AppMode::NORMAL;
    LOG_D("MAIN", "Normal mode running, heap: %u", ESP.getFreeHeap());
}

static void startApMode() {
    LOG_I("MAIN", ">>> AP MODE <<<  SSID=BTWifiSerial  pass=12345678");
    LOG_I("MAIN", "Browse to http://192.168.4.1  or  http://btwifiserial.local");
    LOG_D("MAIN", "Free heap: %u", ESP.getFreeHeap());

    // NO BLE initialisation here.  NimBLEDevice::init() activates the
    // WiFi/BLE coexistence scheduler on the ESP32-C3's single radio,
    // which forces WiFi into modem-sleep and kills AP beacons.
    // BLE is started on-demand when the user triggers a Scan or Connect
    // from the WebUI (see ensureController() in ble_module.cpp).

    webUiInit();

    // If the serial mode is LUA_SERIAL, keep the UART running so the
    // Lua script can detect AP mode and display the modal overlay.
    if (g_config.serialMode == OutputMode::LUA_SERIAL) {
        luaSerialInit();
        luaSerialSetApMode(1);  // 1 = AP-block mode: Lua shows orange overlay
        s_serialActive = true;
    }

    s_appMode = AppMode::AP_MODE;
    LOG_D("MAIN", "AP mode running, heap: %u", ESP.getFreeHeap());
}

static void startTelemetryApMode() {
    LOG_I("MAIN", ">>> TELEMETRY AP MODE <<<  SSID=BTWifiSerial  pass=12345678");
    LOG_I("MAIN", "Browse to http://192.168.4.1  or  http://btwifiserial.local");
    LOG_D("MAIN", "Free heap: %u", ESP.getFreeHeap());

    // WiFi AP starts via webUiInit(); no BLE (single-radio constraint).
    // LuaSerial runs so the Lua script can continue normally while the
    // UDP telemetry is forwarded over the WiFi AP.
    webUiInit();

    if (g_config.serialMode == OutputMode::LUA_SERIAL) {
        luaSerialInit();
        luaSerialSetApMode(2);  // 2 = telemetry AP: Lua keeps running, no overlay
        s_serialActive = true;
    }

    s_appMode = AppMode::TELEM_AP;
    LOG_D("MAIN", "Telemetry AP mode running, heap: %u", ESP.getFreeHeap());
}

static void startStaMode() {
    LOG_I("MAIN", ">>> STA MODE <<<  SSID=%s", g_config.staSsid);
    LOG_D("MAIN", "Free heap: %u", ESP.getFreeHeap());

    // WiFi STA starts inside webUiInit() which checks g_config.wifiMode.
    // No BLE: single-radio constraint; WiFi takes the radio.
    webUiInit();

    if (g_config.serialMode == OutputMode::LUA_SERIAL) {
        luaSerialInit();
        luaSerialSetApMode(3);  // 3 = STA mode: Lua keeps running, no overlay
        s_serialActive = true;
    }

    s_appMode = AppMode::STA_MODE;
    LOG_D("MAIN", "STA mode running, heap: %u", ESP.getFreeHeap());
}

static void startSerialOutput() {
    stopSerialOutput();
    switch (g_config.serialMode) {
        case OutputMode::FRSKY:        frskySerialInit();      break;
        case OutputMode::SBUS:         sbusInit();             break;
        case OutputMode::SPORT_BT:
        case OutputMode::SPORT_MIRROR: sportTelemetryInit();   break;
        case OutputMode::LUA_SERIAL:   luaSerialInit();        break;
    }
    s_serialActive = true;
}

static void stopSerialOutput() {
    if (!s_serialActive) return;
    frskySerialStop();
    sbusStop();
    sportTelemetryStop();
    luaSerialStop();
    s_serialActive = false;
}

/**
 * @brief Blink LED n times, then restore off state.
 *        Blocks for (times*(onMs+offMs)) ms â€” only call before restart.
 */
static void blinkLed(uint8_t times, uint32_t onMs, uint32_t offMs) {
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(PIN_LED, LOW);   // on
        delay(onMs);
        digitalWrite(PIN_LED, HIGH);  // off
        delay(offMs);
    }
}

/**
 * @brief Signal the mode switch, save to NVS, restart.
 *        BLE deinit is intentionally skipped — the restart clears all state.
 */
static void switchModeTo(AppMode next) {
    if (next == AppMode::AP_MODE) {
        LOG_I("MAIN", "Switching to AP mode (restarting...)");
        writeBootMode(BOOT_AP_MODE);
    } else if (next == AppMode::STA_MODE) {
        LOG_I("MAIN", "Switching to STA mode (restarting...)");
        writeBootMode(BOOT_STA_MODE);
    } else {
        LOG_I("MAIN", "Switching to Normal mode (restarting...)");
        writeBootMode(BOOT_NORMAL);
    }

    blinkLed(3, 80, 80);   // 3 fast blinks -> visual confirmation
    delay(100);
    ESP.restart();
}
void mainSetTelemOutput(uint8_t output) {
    g_config.telemetryOutput = static_cast<TelemetryOutput>(output);
    configSave();
    // WiFi UDP output requires the Telemetry AP boot mode;
    // BLE output restores the normal boot mode.
    if (output == static_cast<uint8_t>(TelemetryOutput::WIFI_UDP)) {
        writeBootMode(BOOT_TELEM_AP);
    }
    // No explicit writeBootMode for BLE: setup() always resets to BOOT_NORMAL
    // on boot, so if BOOT_TELEM_AP was previously set it will be cleared.
    blinkLed(3, 80, 80);
    delay(100);
    ESP.restart();
}
/**
 * @brief Called by lua_serial.cpp when it receives a "toggle AP" command.
 *        Switches to AP mode via the normal NVS flag + restart path.
 */
void mainRequestApMode() {
    switchModeTo(AppMode::AP_MODE);
}

/**
 * @brief Called by lua_serial.cpp — switch to normal mode + restart.
 */
void mainRequestNormalMode() {
    switchModeTo(AppMode::NORMAL);
}

/**
 * @brief Called by lua_serial.cpp — set device mode, save config, restart.
 */
void mainSetDeviceMode(uint8_t mode) {
    g_config.deviceMode = static_cast<DeviceMode>(mode);

    // ELRS_HT uses WiFi radio exclusively — force WiFi/telemetry off, trainer map to GV
    if (g_config.deviceMode == DeviceMode::ELRS_HT) {
        g_config.wifiMode = WifiMode::OFF;
        g_config.telemetryOutput = TelemetryOutput::NONE;
        g_config.trainerMapMode = TrainerMapMode::MAP_GV;
    }

    configSave();

    // If Telemetry + WiFi UDP, need Telemetry AP boot mode
    if (g_config.deviceMode == DeviceMode::TELEMETRY &&
        g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
        writeBootMode(BOOT_TELEM_AP);
    }

    blinkLed(3, 80, 80);
    delay(100);
    ESP.restart();
}

/**
 * @brief Called by lua_serial.cpp — set mirror baud rate, save config, restart.
 */
void mainSetMirrorBaud(uint32_t baud) {
    g_config.sportBaud = baud;
    configSave();
    blinkLed(3, 80, 80);
    delay(100);
    ESP.restart();
}

/**
 * @brief Called by lua_serial.cpp — set WiFi mode (0=Off 1=AP 2=STA), save config, restart.
 */
void mainSetWifiMode(uint8_t mode) {
    g_config.wifiMode = static_cast<WifiMode>(mode);
    configSave();
    uint8_t bootMode = BOOT_NORMAL;
    if      (mode == 1) bootMode = BOOT_AP_MODE;
    else if (mode == 2) bootMode = BOOT_STA_MODE;
    writeBootMode(bootMode);
    blinkLed(3, 80, 80);
    delay(100);
    ESP.restart();
}

void mainRequestConfigRestart() {
    uint8_t bootMode = BOOT_NORMAL;

    // Explicit WiFi mode has priority over telemetry AP mapping.
    if (g_config.wifiMode == WifiMode::AP) {
        bootMode = BOOT_AP_MODE;
    } else if (g_config.wifiMode == WifiMode::STA) {
        bootMode = BOOT_STA_MODE;
    } else if (g_config.deviceMode == DeviceMode::TELEMETRY &&
               g_config.telemetryOutput == TelemetryOutput::WIFI_UDP) {
        bootMode = BOOT_TELEM_AP;
    }

    LOG_I("MAIN", "Config restart requested (bootMode=%u)", bootMode);
    writeBootMode(bootMode);
    blinkLed(3, 80, 80);
    delay(100);
    ESP.restart();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// BOOT BUTTON HANDLER
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

static void handleButton() {
    bool reading = digitalRead(PIN_BOOT_BUTTON);

    if (reading != s_lastButtonState) s_lastDebounceTime = millis();
    s_lastButtonState = reading;

    if ((millis() - s_lastDebounceTime) < DEBOUNCE_MS) return;

    if (reading != s_buttonState) {
        s_buttonState = reading;

        if (s_buttonState == LOW) {
            s_buttonPressTime = millis();
            s_buttonHandled   = false;
            LOG_D("MAIN", "Boot button pressed");
        } else {
            if (!s_buttonHandled) {
                uint32_t dur = millis() - s_buttonPressTime;
                LOG_D("MAIN", "Boot button released after %lu ms", dur);
                if (dur < SHORT_PRESS_MAX) {
                    // ELRS_HT uses WiFi for ESP-NOW — AP mode not available
                    if (g_config.deviceMode == DeviceMode::ELRS_HT) {
                        LOG_I("MAIN", "AP mode disabled in ELRS_HT mode");
                        blinkLed(1, 200, 0);
                    } else {
                        // Short press → switch mode via restart
                        switchModeTo(s_appMode == AppMode::NORMAL
                                        ? AppMode::AP_MODE
                                        : AppMode::NORMAL);
                    }
                }
                s_buttonHandled = true;
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// LED INDICATOR
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════


static void updateLed() {
    uint32_t now = millis();

    // AP/STA/TELEM mode: blink at 500 ms regardless of BLE state
    if (s_appMode == AppMode::AP_MODE || s_appMode == AppMode::TELEM_AP ||
        s_appMode == AppMode::STA_MODE) {
        if (now - s_lastLedToggle >= 500) {
            s_lastLedToggle = now;
            s_ledState = !s_ledState;
            digitalWrite(PIN_LED, s_ledState ? LOW : HIGH);
        }
        return;
    }

    // ELRS mode: solid ON when receiving, OFF otherwise
    if (g_config.deviceMode == DeviceMode::ELRS_HT) {
        if (elrsIsReceiving()) {
            s_ledState = true;
            digitalWrite(PIN_LED, LOW);
        } else {
            s_ledState = false;
            digitalWrite(PIN_LED, HIGH);
        }
        return;
    }

    // Normal mode, BLE connected: solid ON
    if (bleIsConnected()) {
        s_ledState = true;
        digitalWrite(PIN_LED, LOW);
        return;
    }

    // Normal mode, no BLE connection: LED off
    s_ledState = false;
    digitalWrite(PIN_LED, HIGH);
}

