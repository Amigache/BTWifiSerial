# BTWifiSerial

![Build](https://img.shields.io/badge/build-passing-brightgreen)
![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--C3-orange)
![Framework](https://img.shields.io/badge/framework-Arduino-blue)
![EdgeTX](https://img.shields.io/badge/EdgeTX-Lua%20Tools-6f42c1)
![Protocol](https://img.shields.io/badge/protocol-BTWS-1f6feb)

ESP32-C3 firmware that acts as a BLE-to-Serial bridge for EdgeTX/OpenTX RC radios. It replaces the legacy FrSky CC2540/HM-10 Bluetooth module with a modern, configurable alternative that supports trainer links, SBUS output, and S.PORT telemetry forwarding. Configuration is available via the built-in WebUI and, in Lua Serial mode, from the EdgeTX Tools script on the radio.

## Features

- **FrSky Trainer** — emulates a CC2540 BLE module for wireless trainer link
- **SBUS Trainer** — outputs standard SBUS signal via UART (100000 8E2)
- **S.PORT Telemetry** — receives and forwards telemetry data via WiFi UDP or BLE
- **Web Configuration** — dark-themed responsive WebUI served from the ESP32's own WiFi AP
- **On-Radio Lua Configuration** — in Lua Serial mode, settings can be changed directly from EdgeTX Tools script UI
- **OTA Updates** — drag-and-drop firmware upload from the browser
- **AT Command Emulation** — responds to HM-10/CC2540 AT commands so EdgeTX sees a native BT module

## Hardware

**Board:** ESP32-C3 SuperMini

| Pin | Function |
|-----|----------|
| GPIO21 | UART TX (serial output to radio/receiver) |
| GPIO20 | UART RX (serial input from radio) |
| GPIO9 | Boot button (mode toggle) |
| GPIO8 | Built-in LED (active LOW) |

### Wiring to a Radio

Connect **GPIO21 (TX)** to the radio's AUX signal pin and share a common **GND**. You can power the module from the radio's supply (check voltage — see note below).

#### Signal level and power

The ESP32-C3 operates at **3.3V logic**. GPIO pins are **not 5V tolerant**.

- Most EdgeTX radios (TX16S, Boxer, etc.) have 3.3V logic on their AUX/Trainer ports — direct connection is safe.
- If connecting to a 5V port, use a level shifter or voltage divider on the RX line.
- The 5V supply pin on AUX connectors can be used to power the ESP32-C3 via its 5V/USB input pad.

#### Radios with built-in Bluetooth (e.g. Radiomaster TX16S, Boxer)

Set `BLUETOOTH = ON` in EdgeTX System Settings. The radio communicates with the module over its internal Bluetooth UART using AT commands + FrSky protocol. BTWifiSerial handles this transparently — no extra wiring needed when the module replaces the internal BT module, or connect via BLE from Central mode.

#### Radios with AUX/Trainer UART port

Wire GPIO21 (TX) to the AUX signal pin and GND to GND. Configure the matching serial mode in the WebUI. See the [EdgeTX Radio Configuration](#edgetx-radio-configuration) section for exact EdgeTX settings per mode.

## Getting Started

### 1. Flash the Firmware

Open the project in PlatformIO and upload:

```
pio run --target upload
```

Or use the OTA update from the WebUI once the initial firmware is flashed.

### 2. Enter Configuration Mode

**Short press** the BOOT button (GPIO9). The LED will blink 3 times to confirm, then the device restarts in AP mode.

### 3. Connect to the WebUI

1. On your phone or computer, connect to the WiFi network:
   - **SSID:** `BTWifiSerial`
   - **Password:** `12345678`
2. Open a browser and go to **http://192.168.4.1**

### 4. Configure and Reboot

Set your desired serial mode, BLE role, and other options. Click **Reboot** when done. The device restarts in Normal mode and begins operating with your settings. All settings are saved to flash and persist across reboots.

## Operating Modes

### FrSky Trainer (default)

Emulates a FrSky CC2540 BLE module. Sends and receives 8-channel trainer data using the FrSky BLE protocol at 115200 baud 8N1.

- **Use case:** Wireless trainer link between two radios, or receiving channels from a HeadTracker.
- **Protocol:** `[0x7E] [0x80] [channel data] [CRC] [0x7E]` — 8 channels, 12-bit, XOR CRC, byte-stuffed.
- **AT commands:** Fully handled. EdgeTX can set the BT name, scan for devices, and connect — all through its native Bluetooth menu.

### SBUS Trainer

Outputs a standard SBUS signal on the TX pin at 100000 baud 8E2.

- **Use case:** Feed channel data to any SBUS-compatible receiver, flight controller, or simulator dongle.
- **Channel mapping:** PPM 1050–1950 → SBUS 172–1811 (16 channels, unused channels held at center 992).
- **Frame rate:** ~14ms. Frame-lost flag is set automatically if no BLE data arrives for 1 second.

> **Signal inversion note:** The SBUS protocol is normally inverted (idle LOW). Some radios (e.g. Radiomaster TX16S, TX12) have a **hardware inverter** on the AUX port, so the ESP32 must output a **non-inverted** signal. Other radios/receivers without a hardware inverter expect the standard inverted SBUS. The `SBUS_HW_INVERT` build flag in `platformio.ini` controls this:
> - `-D SBUS_HW_INVERT=0` — no ESP32 inversion (use with TX16S, TX12, and radios with a hardware inverter on AUX)
> - Remove or comment that flag — ESP32 inverts the signal (use with receivers or devices that expect standard SBUS)

### S.PORT Telemetry (BT)

Receives EdgeTX S.PORT telemetry sent over the Bluetooth UART at 115200 baud 8N1.

- **Use case:** Radios with `BLUETOOTH = ON` that stream re-framed S.PORT telemetry alongside AT commands.
- **Input protocol:** `[0x7E] [8 data bytes, byte-stuffed] [XOR CRC] [0x7E]` — packets batched 2 per write by EdgeTX.
- **AT commands:** Handled on the same byte stream (transparent multiplexing).

### S.PORT Telemetry (Mirror)

Receives raw S.PORT data from EdgeTX's AUX2 Telemetry Mirror output.

- **Use case:** Radios with a dedicated AUX2 serial port configured as "Telemetry Mirror" in EdgeTX.
- **Baud rate:** 57600 (default) or 115200 (for TX16S / Horus F16). Configurable in the WebUI.
- **Input protocol:** Native S.PORT bus: `[0x7E] [physID] [primID] [dataID] [value] [additive CRC]`. Only `DATA_FRAME` (primID = 0x10) packets are forwarded.

### LUA Serial

Bidirectional binary protocol between the ESP32 and two EdgeTX Lua scripts. Provides 8-channel trainer data (via Global Variables) and a rich on-radio configuration UI.

- **Use case:** Full integration with EdgeTX — channel data, BLE management, telemetry forwarding, and settings are all handled from Lua scripts running on the radio.
- **Protocol:** Custom binary frames over UART at 115200 8N1. See [lua-serial-protocol.md](docs/lua-serial-protocol.md) for full specification.
- **Scripts:**
  - `btwfs.lua` — Background **Function** script. Receives channel frames and writes values to GV1–GV8 (Flight Mode 0). Optionally forwards S.PORT telemetry from the radio to the ESP32 via `T_TLM` frames.
  - `BTWifiSerial.lua` — **Tools** script. Provides a multi-page touch/button UI for managing AP mode, BLE role, BLE scanning/connecting, telemetry output selection, and viewing system information.

### Telemetry Output

When using either S.PORT mode, parsed telemetry packets can be forwarded to:

| Output | Description |
|--------|-------------|
| **WiFi UDP** | The ESP32 starts its own WiFi AP (`BTWifiSerial` / `12345678`). Connect your phone or tablet to it and receive 8-byte `SportPacket` datagrams broadcast on UDP port 5010 (configurable). |
| **BLE** | Packets are re-framed with 0x7E framing and sent as BLE notifications on the FrSky characteristic (UUID `0xFFF6`). A BLE client app must be connected. |
| **Off** | Telemetry packets are discarded. No extra output is started. This is the default. |

> **Mutual exclusion — BLE telem and AP Mode:** The ESP32-C3 has a single shared radio. If WiFi AP mode is enabled while telemetry output is set to BLE, the firmware automatically resets telemetry output to Off before switching modes. This is enforced at the firmware level regardless of how the mode change is triggered.

> **Telemetry AP:** When WiFi UDP is selected as the telemetry output in LUA Serial mode, the ESP32 boots into a special "Telemetry AP" mode. The WiFi AP is started for UDP broadcast, but the Lua scripts continue to operate normally — no orange overlay is shown, and the user can still interact with the Tools script UI. The LED blinks at 500 ms to indicate the WiFi AP is active.

## BLE Roles

| Role | Description |
|------|-------------|
| **Peripheral** (Slave) | The module advertises itself. The radio or another device connects to it. Used when the radio acts as BLE Central (e.g. EdgeTX with BLUETOOTH = ON). |
| **Central** (Master) | The module scans for and connects to a remote BLE peripheral (e.g. a HeadTracker). Supports auto-reconnect to the last saved device every 5 seconds. |
| **Telemetry** | Same as Peripheral, used for S.PORT telemetry relay via BLE notifications. |

> **Note:** Changing the BLE role requires a reboot. The WebUI will prompt for confirmation and restart the device automatically.

## Web Interface

Access the WebUI by entering AP mode (short press BOOT button) and browsing to `http://192.168.4.1`.

### Status Card

Shows the current BLE connection status, serial mode, BLE role, device name, local and remote BLE addresses.

### Serial Mode

Select the operating mode from the dropdown. Takes effect immediately (saved to flash).

### Trainer Map (LUA Serial mode only)

Select how received channel data is injected into the radio:

- **GV** — writes to Global Variables GV1–GV8 (default). Use `GV1`–`GV8` as mixer inputs.
- **TR** — injects via Trainer channels TR1–TR8 (values scaled ±1024 → ±512). Use `TR1`–`TR8` as mixer inputs.

This option is only visible when Serial Mode is set to **LUA Serial**.

### Telemetry Output

Visible only when a S.PORT mode is selected. Configure:

- **Forward to** — WiFi UDP or BLE
- **Mirror Baud Rate** — 57600 or 115200 (shown only for S.PORT Mirror mode)
- **UDP Port** — default 5010
- **Live stats** — packet count, packets/sec, output status

### Bluetooth

- **Device Name** — change the BLE advertised name (up to 30 characters)
- **Role** — switch between Peripheral, Central, and Telemetry

### BLE Scan (Central mode only)

- **Saved peer** — shows the last connected device with Connect / Disconnect / Forget buttons
- **Scan** — discovers nearby BLE devices (5-second active scan). FrSky-compatible devices are marked with a star (★)
- **Connect** — tap any discovered device to initiate connection

### Connected Clients (Peripheral mode only)

Shows the address of the currently connected BLE central with a Disconnect button.

### Firmware Update

Drag and drop a `.bin` file onto the upload area or click to browse. The firmware is uploaded over HTTP and flashed via OTA. The device reboots automatically after a successful update.

### Reboot

Restarts the device in Normal mode. A confirmation dialog is shown before proceeding.

## LED Indicator

| LED State | Meaning |
|-----------|---------|
| OFF | Normal mode, no BLE connection |
| Solid ON | Normal mode, BLE connected |
| Blinking (500ms) | AP mode or Telemetry AP mode (WiFi active) |
| 3 rapid blinks | Mode toggle confirmed (before restart) |

## Button

| Action | Result |
|--------|--------|
| Short press (<1s) | Toggle between Normal and AP mode (restarts the device) |

## AT Command Compatibility

BTWifiSerial responds to the HM-10/CC2540 AT command set that EdgeTX uses when `BLUETOOTH = ON`. This allows the radio to configure the module name, discover devices, and establish connections through its native Bluetooth settings menu.

| Command | Response | Description |
|---------|----------|-------------|
| `AT+BAUD4` | `OK+Set:4` | Acknowledge baud rate (already 115200) |
| `AT+NAMExxx` | `OK+Name:xxx` | Set device name |
| `AT+TXPW2` | `OK+Txpw:2` | Acknowledge TX power |
| `AT+ROLE0` | `OK+Role:0` / `Peripheral:addr` | Set Peripheral mode |
| `AT+ROLE1` | `OK+Role:1` / `Central:addr` | Set Central mode |
| `AT+DISC?` | `OK+DISCS` → results → `OK+DISCE` | BLE scan (Central) |
| `AT+CONaddr` | `OK+CONNA` | Connect to device |
| `AT+CLEAR` | `OK+CLEAR` | Disconnect |

## EdgeTX Radio Configuration

This section covers how to configure EdgeTX for each serial mode.

### SBUS Trainer (via AUX port)

This is the recommended mode for wired head-tracker input using HeadTracker or any BLE device that outputs FrSky-compatible channels.

**Step 1 — Hardware port (System Settings)**

`SYS` → `Hardware` → find your AUX port (AUX1 or AUX2) → set it to **SBUS Trainer**.

**Step 2 — Trainer mode (Model Settings)**

`MDL` → `Trainer` → set mode to **Master/Serial**.

> ⚠️ Do **not** use `Master/SBUS Module` — that mode reads from the external module bay, not from the AUX port.

**Step 3 — Channel override (Mixer)**

In `MDL` → `Mixes`, add a mix on the target channel using input source `TR1`, `TR2`, etc. and assign a switch to activate it. When the switch is active and BLE data is arriving, the trainer channels will override the stick inputs.

---

### FrSky Trainer / S.PORT (via built-in Bluetooth)

For radios with internal Bluetooth (TX16S, Boxer, etc.):

1. `SYS` → `Bluetooth` → mode: **Trainer** or **Telemetry** depending on use case.
2. The radio will scan for and connect to the module by name (default: `BTWifiSerial`).
3. No AUX port wiring needed — Bluetooth is used end-to-end.

---

### S.PORT Telemetry Mirror (via AUX port)

1. `SYS` → `Hardware` → set AUX port to **Telemetry Mirror**.
2. In the WebUI, set Serial Mode to **S.PORT Telemetry (Mirror)** and match the baud rate (57600 for most radios, 115200 for TX16S/Horus).
3. Select telemetry output destination (WiFi UDP or BLE).

---

### LUA Serial (via AUX port)

1. `SYS` → `Hardware` → find your AUX port (AUX1 or AUX2) → set it to **LUA** at **115200** baud.
2. Copy the Lua scripts to the radio's SD card:
   - `btwfs.lua` → `/SCRIPTS/FUNCTIONS/btwfs.lua`
   - `BTWifiSerial.lua` → `/SCRIPTS/TOOLS/BTWifiSerial.lua`
3. In `MDL` → `Special Functions`, create a new entry:
   - **Switch:** `ON` (always active)
   - **Function:** `Lua Script`
   - **Script:** `btwfs`
4. Open the **BTWifiSerial** Tools script from the radio's Tools menu to access the configuration UI.
5. Channel data injection depends on the **Trainer Map** setting (configurable from the BTWifiSerial Tools script or the Web UI):
   - **GV** (default) — values are written to GV1–GV8 in Flight Mode 0. Use `GV1`–`GV8` as mixer inputs.
   - **TR** — values are injected via `setTrainerChannels()` (array of 8 channels, scaled ±1024 → ±512). Use `TR1`–`TR8` as mixer inputs, and set Trainer mode to **Master/Serial** in Model Settings.

> **Telemetry forwarding:** If the radio has active telemetry sensors, `btwfs.lua` automatically forwards S.PORT packets to the ESP32 via `T_TLM` frames. Configure the telemetry output (WiFi UDP, BLE, or Off) from the Telemetry page in the BTWifiSerial Tools script.

---

## Lua Scripts

Two Lua scripts work together to provide full on-radio integration in LUA Serial mode.

### btwfs.lua — Background Function Script

**Location:** `/SCRIPTS/FUNCTIONS/btwfs.lua`

Runs continuously as an EdgeTX Special Function (`Lua Script`, switch `ON`). Responsibilities:

- **Channel data:** Parses `T_CH` frames from the ESP32 and injects 8 signed channel values (−1024 to +1024) into the radio. The injection method depends on the **Trainer Map** setting:
  - **GV** (default) — writes values to Global Variables GV1–GV8 (Flight Mode 0). Use `GV1`–`GV8` as mixer inputs.
   - **TR** — calls `setTrainerChannels()` with an 8-channel array, each value divided by 2 (±1024 → ±512, matching EdgeTX's ±512 trainer range). Use `TR1`–`TR8` as mixer inputs.
- **Config fallback:** When the BTWifiSerial Tools script is not running, `btwfs.lua` reads the Trainer Map setting directly from the `T_CFG` frame sent by the ESP32, ensuring correct injection mode without user intervention.
- **Telemetry forwarding:** Calls `sportTelemetryPop()` each frame to collect pending S.PORT packets and sends them to the ESP32 as `T_TLM` frames (up to 8 packets per frame).
- **Coexistence:** When the BTWifiSerial Tools script is active, incoming serial bytes are yielded to it via a shared-memory heartbeat mechanism. The Trainer Map mode is communicated via shared memory (SHM slot 2). Telemetry forwarding continues independently.

> **Resource efficiency:** `btwfs.lua` consumes `T_CH` frames (channel data) and `T_CFG` frames (to read the Trainer Map mode as a fallback when the Tools script is not running). All other frame types (`T_INF`, `T_SYS`) are discarded by the parser. The firmware detects when the Tools script is closed and suppresses the heavy periodic resync burst (`T_CFG + T_INF + T_SYS` every 30 s), sending only `T_CH` and `T_ST` while the background script runs alone.

### BTWifiSerial.lua — Tools Script

**Location:** `/SCRIPTS/TOOLS/BTWifiSerial.lua`

Opened from the radio's `SYS` → `Tools` menu. Provides a full-screen multi-page UI:

| Page | Description |
|------|-------------|
| **Dashboard** | Shows BLE connection status, system information (build version, serial mode, BT name, addresses), AP mode toggle. |
| **Bluetooth** | BLE role selection (Peripheral/Central/Telemetry), scan for devices (Central mode), connect/disconnect/forget saved peers. |
| **Telemetry** | Select telemetry output (WiFi UDP, BLE, or Off). Saving triggers a config write + ESP32 restart. |
| **Options** | Configure Trainer Map mode (GV or TR) — determines how received channel data is injected into the radio. |
| **Settings** | Configure WiFi AP SSID, AP password, UDP port, and S.PORT Mirror baud rate. Changes that require a network restart trigger an automatic reboot. |

The Tools script communicates with the ESP32 via command frames (`T_CMD`) and receives configuration/status via `T_CFG`, `T_INF`, `T_SYS`, and `T_ACK` frames.

---

## Build

### Requirements

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- ESP32-C3 SuperMini board (or compatible ESP32-C3 board)

### Dependencies

| Library | Version |
|---------|---------|
| NimBLE-Arduino | ^1.4.3 |
| ESPAsyncWebServer | ^3.6.0 |
| ArduinoJson | ^7.3.0 |

### Build and Upload

```bash
# Build
pio run

# Upload via USB
pio run --target upload

# Monitor serial output
pio device monitor
```

## Acknowledgements

This project was inspired by and built with knowledge from the following open source projects:

- **[EdgeTX](https://github.com/EdgeTX/edgetx)** — open source firmware for RC radio transmitters, whose BLE trainer and telemetry protocols are implemented here.
- **[BTWifiModule](https://github.com/olliw42/btWifiModule)** — ESP32-based BT/WiFi module reference, which influenced the AT command handling and FrSky BLE protocol implementation.
- **[HeadTracker](https://github.com/dlktdr/HeadTracker)** — open source head tracker project, which provided insight into the BLE channel data format and PPM output used as input source.

## License

This project is open source, released under the [MIT License](LICENSE).

You are free to use, modify, and distribute this software for any purpose, including commercial use, as long as the original license notice is retained.
