# BTWifiSerial User Manual

This manual is for pilots and operators using BTWifiSerial in real radio workflows.

It explains setup, operation, available modes, and practical procedures from first boot to daily use.

## Table of Contents

- [1) What BTWifiSerial is](#1-what-btwifiserial-is)
- [2) Before you start](#2-before-you-start)
- [3) Installation and first boot](#3-installation-and-first-boot)
- [4) User interfaces](#4-user-interfaces)
- [5) Modes explained](#5-modes-explained)
- [6) Daily operation workflows](#6-daily-operation-workflows)
- [7) Saving changes and restart behavior](#7-saving-changes-and-restart-behavior)
- [8) Troubleshooting](#8-troubleshooting)
- [9) Factory reset (Web UI)](#9-factory-reset-web-ui)
- [10) Recommended operating patterns](#10-recommended-operating-patterns)
- [11) Safety and consistency notes](#11-safety-and-consistency-notes)

---

## 1) What BTWifiSerial is

BTWifiSerial is an ESP32-C3 bridge that provides:

- BLE trainer and device connectivity workflows
- telemetry routing options (WiFi UDP, BLE, or Off depending on mode)
- on-device configuration from either browser or radio Lua UI

You can operate it through:

- **Web UI** (phone/tablet/PC)
- **EdgeTX Tools UI (BTWFS)** on your radio

---

## 2) Before you start

### Hardware

- ESP32-C3 SuperMini flashed with BTWifiSerial firmware
- radio serial connection wired to ESP32 UART

### Radio requirements

- EdgeTX/OpenTX AUX configured as **LUA @ 115200**
- `btwfs.lua` running as a Special Function

### SD card files

- `/SCRIPTS/FUNCTIONS/btwfs.lua`
- `/SCRIPTS/TOOLS/BTWFS/` (full folder)

---

## 3) Installation and first boot

### Step A: flash firmware

From project root:

```bash
pio run
pio run --target upload
```

### Step B: configure radio

1. Set AUX serial to `LUA 115200`.
2. Add Special Function:
   - switch `ON`
   - action `Lua Script`
   - script `btwfs`
3. Open `BTWFS/main.lua` from radio Tools.

### Step C: choose your setup path

- If you prefer browser setup first: open Web UI and configure main settings.
- If you prefer all from radio: use BTWFS pages directly.

---

## 4) User interfaces

### 4.1 Web UI

Access:

- AP mode: `http://192.168.4.1`
- STA mode: your router-assigned IP

Common sections:

- Status
- System configuration
- WiFi configuration
- Bluetooth actions
- Telemetry-related options
- OTA update

### 4.2 Lua UI (BTWFS)

The radio UI gives direct control and live status while operating the model.

Typical pages include:

- Dashboard
- Settings
- Bluetooth
- WiFi (when available by mode/output)

Use your radio wheel/keys to navigate, edit values, and confirm actions.

---

## 5) Modes explained

### 5.1 Device modes

#### Trainer IN

- radio receives trainer-related flow from configured link path
- typically used in central role workflows

#### Trainer OUT

- radio/output behavior oriented to sending trainer data outward
- BLE behavior differs from Trainer IN depending on firmware role

#### Telemetry

- focuses on telemetry workflows
- output routing depends on telemetry output selection

### 5.2 WiFi modes

#### Off

- no WiFi services active
- WiFi scanning/config workflows are unavailable

#### AP

- device creates access point
- useful for direct local browser access

#### STA

- device joins existing router
- use this for shared network environments

### 5.3 Telemetry output options

- WiFi UDP
- BLE
- Off

Availability and behavior depend on selected device mode and serial mode combinations.

---

## 6) Daily operation workflows

### 6.1 Quick pre-flight check

1. Confirm `btwfs` Special Function is ON.
2. Open BTWFS tool and verify status is alive.
3. Check selected mode set (device/WiFi/telemetry output).
4. If BLE is needed, verify connected/disconnected state is expected.

### 6.2 Connect to BLE device

1. Open Bluetooth page.
2. Start BLE scan.
3. Select target from result list.
4. Wait for connection completion.

If already paired, you can reconnect from saved device flow.

### 6.3 Forget and re-pair

Use forget when:

- target changed address/profile
- reconnect loops fail
- stale saved peer causes conflicts

Sequence:

1. disconnect if currently connected,
2. forget saved device,
3. scan and connect again.

### 6.4 Configure WiFi network from Lua UI

1. In Settings, open WiFi scan picker.
2. Select SSID.
3. Confirm/edit password.
4. Save and wait for restart/reconnect.

After restart:

- in AP mode, reconnect to AP and re-open UI,
- in STA mode, verify router association.

---

## 7) Saving changes and restart behavior

Not all settings apply the same way.

General rule:

- mode/transport changes usually require restart
- identity-only updates (for example BT name) may apply without restart

Best practice:

1. change one critical setting group,
2. save,
3. wait for completion,
4. verify status before next major change.

This avoids stacked transitions and ambiguous state.

---

## 8) Troubleshooting

### 8.1 No data in Lua tool

Check:

- AUX is `LUA 115200`
- `btwfs` Special Function is active
- wiring to ESP32 UART is correct

### 8.2 Cannot scan WiFi

Check:

- WiFi mode is not `Off`
- device is not in a conflicting operation state

### 8.3 BLE connection fails repeatedly

Try:

1. disconnect/forget,
2. scan again,
3. reconnect from fresh result list.

### 8.4 Web UI unreachable

- AP mode: ensure you are connected to device AP, then use `192.168.4.1`
- STA mode: verify router IP assignment and same network segment

---

## 9) Factory reset (Web UI)

Web UI includes a **Factory Reset** action in the **System Actions** section.

Use this when you want to return to a known baseline configuration.

What it does:

1. restores default configuration,
2. writes defaults to persistent storage,
3. reboots the device.

Default state after reset:

- Device Mode: `Trainer IN`
- Serial Mode: `LUA Serial`
- WiFi Mode: `Off`

After reset, you can configure from radio Lua immediately, and you can still open Web UI by entering AP mode manually.

---

## 10) Recommended operating patterns

- Use Web UI for broad configuration and OTA.
- Use Lua UI for field operations and quick checks.
- Keep mode strategy simple per model profile.
- Validate after each restart-triggering configuration change.

---

## 11) Safety and consistency notes

- Avoid changing multiple transport-critical settings at once.
- Confirm active mode before flight.
- If behavior is unexpected, return to known baseline:
  - WiFi mode,
  - device mode,
  - telemetry output,
  - BLE pairing state.

A clean baseline reduces troubleshooting time dramatically.