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
#include "web_ui.h"

// â”€â”€â”€ Runtime log level (defined here, declared extern in log.h) â”€â”€â”€â”€â”€
uint8_t g_logLevel = LOG_LEVEL;

// â”€â”€â”€ Global channel data â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
ChannelData g_channelData;

// ─── Boot mode via NVS (RTC_DATA_ATTR does NOT survive ESP.restart on ESP32-C3) ─
static constexpr uint8_t BOOT_NORMAL  = 0;
static constexpr uint8_t BOOT_AP_MODE = 1;

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

// â”€â”€â”€ Application state â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
enum class AppMode : uint8_t { NORMAL, AP_MODE };
static AppMode s_appMode      = AppMode::NORMAL;
static bool    s_serialActive = false;

// â”€â”€â”€ Boot button debounce â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static constexpr uint32_t DEBOUNCE_MS     = 50;
static constexpr uint32_t SHORT_PRESS_MAX = 1000;

static bool     s_lastButtonState  = HIGH;
static bool     s_buttonState      = HIGH;
static uint32_t s_lastDebounceTime = 0;
static uint32_t s_buttonPressTime  = 0;
static bool     s_buttonHandled    = false;

// â”€â”€â”€ LED â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static uint32_t s_lastLedToggle = 0;
static bool     s_ledState      = false;

// â”€â”€â”€ Forward declarations â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void startNormalMode();
static void startApMode();
static void stopSerialOutput();
static void startSerialOutput();
static void handleButton();
static void updateLed();
static void blinkLed(uint8_t times, uint32_t onMs, uint32_t offMs);
static void switchModeTo(AppMode next);

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// SETUP
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
    } else {
        LOG_I("MAIN", "NVS: booting into NORMAL mode");
        startNormalMode();
    }

    LOG_I("MAIN", "Setup complete. Short press BOOT to toggle mode.");
    LOG_I("MAIN", "Free heap: %u bytes", ESP.getFreeHeap());
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// LOOP
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void loop() {
    handleButton();

    switch (s_appMode) {
        case AppMode::NORMAL:
            bleLoop();
            if (s_serialActive) {
                switch (g_config.serialMode) {
                    case OutputMode::FRSKY:        frskySerialLoop(); break;
                    case OutputMode::SBUS:         sbusLoop();        break;
                    case OutputMode::SPORT_BT:
                    case OutputMode::SPORT_MIRROR:  sportTelemetryLoop(); break;
                }
            }
            break;

        case AppMode::AP_MODE:
            // Only the web UI runs in AP mode.  BLE advertising is off
            // (no bleLoop needed) and serial output is not started.
            webUiLoop();
            break;
    }

    updateLed();
    yield();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// MODE MANAGEMENT
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static void startNormalMode() {
    LOG_I("MAIN", ">>> NORMAL MODE <<<");
    LOG_D("MAIN", "Free heap: %u", ESP.getFreeHeap());

    bleInit();
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

    s_appMode = AppMode::AP_MODE;
    LOG_D("MAIN", "AP mode running, heap: %u", ESP.getFreeHeap());
}

static void startSerialOutput() {
    stopSerialOutput();
    switch (g_config.serialMode) {
        case OutputMode::FRSKY:        frskySerialInit();      break;
        case OutputMode::SBUS:         sbusInit();             break;
        case OutputMode::SPORT_BT:
        case OutputMode::SPORT_MIRROR: sportTelemetryInit();   break;
    }
    s_serialActive = true;
}

static void stopSerialOutput() {
    if (!s_serialActive) return;
    frskySerialStop();
    sbusStop();
    sportTelemetryStop();
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
    } else {
        LOG_I("MAIN", "Switching to Normal mode (restarting...)");
        writeBootMode(BOOT_NORMAL);
    }

    blinkLed(3, 80, 80);   // 3 fast blinks -> visual confirmation
    delay(100);
    ESP.restart();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// BOOT BUTTON HANDLER
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
                    // Short press â†’ switch mode via restart
                    switchModeTo(s_appMode == AppMode::NORMAL
                                    ? AppMode::AP_MODE
                                    : AppMode::NORMAL);
                }
                s_buttonHandled = true;
            }
        }
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// LED INDICATOR
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

static void updateLed() {
    uint32_t now = millis();

    // AP mode: blink at 500 ms regardless of BLE state
    if (s_appMode == AppMode::AP_MODE) {
        if (now - s_lastLedToggle >= 500) {
            s_lastLedToggle = now;
            s_ledState = !s_ledState;
            digitalWrite(PIN_LED, s_ledState ? LOW : HIGH);
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

