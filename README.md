# BTWifiSerial

![Platform](https://img.shields.io/badge/platform-ESP32--C3-0A7EA4)
![Framework](https://img.shields.io/badge/framework-Arduino-00979D)
![Build](https://img.shields.io/badge/build-PlatformIO-F5822A)
![Radio](https://img.shields.io/badge/radio-EdgeTX-4B6BFB)
![Interface](https://img.shields.io/badge/interface-WebUI%20%2B%20Lua-2E8B57)

BTWifiSerial is an ESP32-C3 firmware + EdgeTX Lua toolchain that replaces legacy FrSky-style Bluetooth modules with a modern bridge.

It lets you run trainer, telemetry, and configuration workflows through one device, with two control surfaces:

- **Web UI** (phone/PC browser)
- **Lua UI on the radio** (EdgeTX Tools)

## Table of Contents

- [What BTWifiSerial does](#what-btwifiserial-does)
- [Configuration paths: Web UI or Lua UI](#configuration-paths-web-ui-or-lua-ui)
- [Operating concepts](#operating-concepts)
- [Hardware target](#hardware-target)
- [Repository contents](#repository-contents)
- [Installation](#installation)
- [First steps (recommended order)](#first-steps-recommended-order)
- [Save and restart behavior](#save-and-restart-behavior)
- [Runtime roles of Lua scripts](#runtime-roles-of-lua-scripts)
- [Troubleshooting quick checks](#troubleshooting-quick-checks)
- [Factory reset (Web UI)](#factory-reset-web-ui)
- [Documentation](#documentation)
- [Notes for maintainers](#notes-for-maintainers)

---

## What BTWifiSerial does

BTWifiSerial sits between your radio and the outside world (BLE peers, WiFi telemetry clients, and serial peripherals).

Core capabilities:

- BLE central/peripheral behavior depending on selected device mode
- Trainer channel transport and mapping
- SBUS / S.PORT related serial routing modes
- Telemetry output routing (WiFi UDP / BLE / Off)
- On-device configuration with persistence + controlled reboot flows
- OTA firmware update from browser

---

## Configuration paths: Web UI or Lua UI

You can configure the same device from either interface.

### Web UI (recommended for full setup)

Use this when you want to:

- Perform initial setup
- Configure WiFi SSID/password/port quickly
- Run OTA updates
- Manage BLE scan/connect/disconnect/forget

Access:

- AP mode: `http://192.168.4.1`
- STA mode: the IP assigned by your router

### Lua UI (recommended for on-radio workflow)

Use this when you want to:

- Configure and monitor directly from the radio
- Scan/select WiFi networks and edit credentials
- Manage BLE actions without leaving the transmitter
- Keep all operational checks in one place during field use

---

## Operating concepts

### Serial output modes

- `frsky`
- `sbus`
- `sport_bt`
- `sport_mirror`
- `lua_serial`

### Device modes

- `trainer_in` (BLE central)
- `trainer_out` (BLE peripheral)
- `telemetry`

### WiFi modes

- `off`
- `ap`
- `sta`

The firmware can restart into different internal runtime profiles depending on saved config.

---

## Hardware target

Target board: **ESP32-C3 SuperMini**

| Pin | Function |
|---|---|
| GPIO21 | UART TX |
| GPIO20 | UART RX |
| GPIO9 | BOOT button (active low) |
| GPIO8 | LED (active low) |

---

## Repository contents

- `src/` firmware sources (Arduino + PlatformIO)
- `lua/SCRIPTS/FUNCTIONS/btwfs.lua` EdgeTX background function
- `lua/SCRIPTS/TOOLS/BTWFS/` modular EdgeTX Tools app (current UI)
- `docs/` project documentation

---

## Installation

### 1) Build and flash firmware

Requirements:

- PlatformIO
- ESP32-C3 board support

Commands:

```bash
pio run
pio run --target upload
pio device monitor
```

### 2) Configure radio serial port

On EdgeTX/OpenTX radio, set AUX serial to:

- **LUA @ 115200**

### 3) Copy Lua scripts to SD card

Copy:

- `lua/SCRIPTS/FUNCTIONS/btwfs.lua` -> `/SCRIPTS/FUNCTIONS/btwfs.lua`
- folder `lua/SCRIPTS/TOOLS/BTWFS/` -> `/SCRIPTS/TOOLS/BTWFS/`

### 4) Add Special Function for background script

Create a Special Function:

- Switch: `ON`
- Action: `Lua Script`
- Script: `btwfs`

### 5) Open the Tools UI

From radio Tools menu, run:

- `BTWFS/main.lua`

---

## First steps (recommended order)

1. **Power and verify link**
   - Confirm board is responsive in Web UI or Lua UI.

2. **Set base mode selections**
   - Device mode, serial mode, trainer mapping.

3. **Set WiFi behavior**
   - Choose `AP` or `STA`.
   - If `STA`, configure SSID/password and verify router connection.

4. **Select telemetry output strategy**
   - WiFi UDP / BLE / Off depending on your ground station workflow.

5. **BLE workflow check**
   - Scan, connect, disconnect, reconnect, forget as needed.

6. **Persist and reboot when prompted**
   - Some settings are immediate, others require restart.

---

## Save and restart behavior

Typical behavior by setting category:

- **System mode changes**: save + restart
- **WiFi mode/credentials changes**: save + restart (mode dependent)
- **BT name**: save without restart
- **Telemetry output / mirror baud / UDP-related fields**: may trigger immediate action or restart depending on setting

Always wait for completion/acknowledgement before changing another critical setting.

---

## Runtime roles of Lua scripts

- `btwfs.lua`
  - Runs in background
  - Handles channel/telemetry data path
  - Coordinates serial ownership with tools heartbeat

- `BTWFS` Tools app
  - Foreground UI and user actions
  - Preference/info synchronization
  - BLE and WiFi scan command execution

---

## Troubleshooting quick checks

- No data in Tools page:
  - Verify AUX is `LUA 115200`
  - Verify `btwfs` Special Function is active
  - Verify wiring GPIO21/GPIO20 to radio serial path

- WiFi scan unavailable:
  - Ensure WiFi mode is active (`AP` or `STA`)
  - If BLE is currently active in conflicting mode, close BLE action first

- BLE connect fails:
  - Re-scan and confirm target is still advertising
  - Retry from clean state (disconnect/forget if needed)

---

## Factory reset (Web UI)

Web UI includes a **Factory Reset** action under **System Actions**.

Behavior:

- Restores configuration defaults
- Saves defaults to NVS
- Reboots the device

Important default values after reset:

- Device Mode: `Trainer IN`
- Serial Mode: `LUA Serial`
- WiFi Mode: `Off`

After reset, you can still enter AP mode manually (BOOT button) or configure from the radio Lua UI.

---

## Documentation

- End-user guide: `docs/user-manual.md`

## Notes for maintainers

- Keep Lua constants and firmware protocol IDs synchronized.
- Treat `docs/` as the source for long-form documentation updates.