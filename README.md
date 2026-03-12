# BTWifiSerial

ESP32-C3 firmware that replaces legacy FrSky-style Bluetooth modules with a modern bridge for EdgeTX/OpenTX radios.

It supports trainer link, SBUS output, S.PORT telemetry forwarding, a built-in WebUI, and a binary Lua serial protocol for on-radio tools.

## What this project includes

- **Firmware (ESP32-C3, Arduino + PlatformIO)**
  - BLE central/peripheral workflows
  - FrSky trainer protocol compatibility
  - SBUS output mode
  - S.PORT telemetry ingest + output routing (WiFi UDP or BLE)
  - Lua serial protocol (multi-channel framed protocol)
  - WebUI + OTA update server

- **WebUI (embedded in firmware)**
  - Configuration pages/cards exposed over WiFi AP or STA
  - Save flows with restart where needed
  - BLE scan/connect/disconnect/forget actions
  - OTA firmware upload from browser

- **Lua scripts (EdgeTX)**
  - `lua/SCRIPTS/FUNCTIONS/btwfs.lua`: background function for channel injection + telemetry forwarding
  - `lua/SCRIPTS/TOOLS/BTWFS/`: modular tools UI (current)
  - `lua/SCRIPTS/TOOLS/BTWifiSerial.lua`: legacy monolithic tools script (kept for compatibility/migration)

---

## Hardware

Target board: **ESP32-C3 SuperMini**

| Pin | Function |
|-----|----------|
| GPIO21 | UART TX |
| GPIO20 | UART RX |
| GPIO9  | BOOT button (active low) |
| GPIO8  | LED (active low) |

---

## Operating model

### Serial output modes (`OutputMode`)

- `frsky`
- `sbus`
- `sport_bt`
- `sport_mirror`
- `lua_serial`

### Device modes (`DeviceMode`)

- `trainer_in` (BLE central)
- `trainer_out` (BLE peripheral)
- `telemetry`

### WiFi modes (`WifiMode`)

- `off`
- `ap`
- `sta`

### Runtime boot modes (internal)

- Normal
- AP mode
- Telemetry AP mode
- STA mode

Firmware may redirect from normal boot to AP/STA/Telemetry-AP depending on saved config.

---

## WebUI summary

Access through:

- AP mode: `http://192.168.4.1`
- STA mode: device IP assigned by your router

Main cards/areas:

- **Status**: connection + runtime state
- **System Config**: device mode, serial mode, trainer map (**Save & Restart**)
- **WiFi Config**: WiFi mode + credentials (**Save & Restart**)
- **Telemetry Output** (only on S.PORT serial modes): output target, mirror baud, UDP port
- **Bluetooth**: BT name save, BLE scan/connect/disconnect/forget
- **Firmware Update**: OTA upload
- **System Actions**: reboot

### Save behavior (important)

- **System Config**: explicit save, restart required
- **WiFi Config**: explicit save, restart required
- **Bluetooth name**: explicit save, no restart required
- **Telemetry output / mirror baud / UDP port**: immediate command-based update

---

## EdgeTX / Lua integration

### Required radio setup for Lua serial mode

1. Configure AUX port to **LUA @ 115200**.
2. Copy scripts:
   - `btwfs.lua` → `/SCRIPTS/FUNCTIONS/btwfs.lua`
   - Folder `BTWFS` → `/SCRIPTS/TOOLS/BTWFS/`
3. Add Special Function: switch `ON` → Lua script `btwfs`.
4. Open tool from EdgeTX Tools menu (`BTWFS/main.lua`).

### Script responsibilities

- **`btwfs.lua`**
  - Parses channel frames from ESP32
  - Injects into GV or Trainer channels depending on `Trainer Map`
  - Forwards S.PORT telemetry upstream to ESP32 over `CH_TRANS/PT_TRANS_SPORT`
  - Coordinates serial ownership using SHM heartbeat from tools script

- **`BTWFS` tools app**
  - Renders multi-page UI
  - Requests/syncs prefs + info
  - Sends preference writes (`PREF_SET`) and BLE/WiFi scan commands
  - Maintains heartbeat to signal foreground ownership

For binary protocol details, see: `docs/lua-serial-protocol.md`.

---

## Build and flash

Requirements:

- PlatformIO
- ESP32-C3 board support

Commands:

```bash
pio run
pio run --target upload
pio device monitor
```

---

## Repository layout (key parts)

- `src/`: firmware sources
- `docs/lua-serial-protocol.md`: technical protocol spec for developers
- `lua/SCRIPTS/FUNCTIONS/`: background script
- `lua/SCRIPTS/TOOLS/BTWFS/`: modular tools UI
- `lua/SCRIPTS/TOOLS/BTWifiSerial.lua`: legacy tool variant

---

## Documentation scope

- This README is intentionally an **overview**.
- Protocol internals and frame-level contracts are documented in `docs/lua-serial-protocol.md`.
- End-user step-by-step manual is out of scope here and can be added separately.
