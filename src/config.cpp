/**
 * @file config.cpp
 * @brief Persistent configuration using ESP32 Preferences (NVS)
 */

#include "config.h"
#include "log.h"
#include <Preferences.h>

Config g_config;

static Preferences prefs;
static const char* NVS_NAMESPACE = "btwifi";

void configInit() {
    g_config.setDefaults();
    configLoad();
}

void configLoad() {
    if (!prefs.begin(NVS_NAMESPACE, true)) {  // read-only
        LOG_W("CFG", "NVS namespace not found, using defaults");
        return;
    }

    g_config.serialMode   = (OutputMode)prefs.getUChar("serialMode", (uint8_t)OutputMode::FRSKY);
    g_config.bleRole      = (BleRole)prefs.getUChar("bleRole", (uint8_t)BleRole::PERIPHERAL);
    g_config.hasRemoteAddr = prefs.getBool("hasRmtAddr", false);

    String name = prefs.getString("btName", "BTWifiSerial");
    strlcpy(g_config.btName, name.c_str(), sizeof(g_config.btName));

    String lAddr = prefs.getString("localAddr", "");
    strlcpy(g_config.localBtAddr, lAddr.c_str(), sizeof(g_config.localBtAddr));

    String addr = prefs.getString("rmtAddr", "");
    strlcpy(g_config.remoteBtAddr, addr.c_str(), sizeof(g_config.remoteBtAddr));
    g_config.remoteAddrType = prefs.getUChar("rmtAddrType", 0);

    // Telemetry settings
    g_config.telemetryOutput = (TelemetryOutput)prefs.getUChar("telemOut", (uint8_t)TelemetryOutput::WIFI_UDP);
    g_config.udpPort         = prefs.getUShort("udpPort", 5010);
    g_config.sportBaud       = prefs.getULong("sportBaud", 57600);

    prefs.end();

    const char* modeStr = "FrSky";
    switch (g_config.serialMode) {
        case OutputMode::SBUS:         modeStr = "SBUS";         break;
        case OutputMode::SPORT_BT:     modeStr = "S.PORT-BT";    break;
        case OutputMode::SPORT_MIRROR: modeStr = "S.PORT-Mirror"; break;
        default: break;
    }
    LOG_I("CFG", "Loaded: mode=%s role=%s name=%s",
        modeStr,
        g_config.bleRole == BleRole::PERIPHERAL ? "Peripheral" :
            (g_config.bleRole == BleRole::CENTRAL ? "Central" : "Telemetry"),
        g_config.btName);
}

void configSave() {
    if (!prefs.begin(NVS_NAMESPACE, false)) {  // read-write
        LOG_E("CFG", "Failed to open NVS for writing");
        return;
    }

    prefs.putUChar("serialMode", (uint8_t)g_config.serialMode);
    prefs.putUChar("bleRole", (uint8_t)g_config.bleRole);
    prefs.putString("btName", g_config.btName);
    prefs.putString("localAddr", g_config.localBtAddr);
    prefs.putString("rmtAddr", g_config.remoteBtAddr);
    prefs.putBool("hasRmtAddr", g_config.hasRemoteAddr);
    prefs.putUChar("rmtAddrType", g_config.remoteAddrType);

    // Telemetry settings
    prefs.putUChar("telemOut", (uint8_t)g_config.telemetryOutput);
    prefs.putUShort("udpPort", g_config.udpPort);
    prefs.putULong("sportBaud", g_config.sportBaud);

    prefs.end();

    LOG_I("CFG", "Settings saved to NVS");
}
