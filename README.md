# BTWifiSerial

ESP32-C3 firmware that acts as a BLE-to-Serial bridge for EdgeTX/OpenTX RC radios. It replaces the legacy FrSky CC2540/HM-10 Bluetooth module with a modern, configurable alternative that supports trainer links, SBUS output, and S.PORT telemetry forwarding — all configurable via a built-in web interface.

## Features

- **FrSky Trainer** — emulates a CC2540 BLE module for wireless trainer link
- **SBUS Trainer** — outputs standard SBUS signal via UART (100000 8E2 inverted)
- **S.PORT Telemetry** — receives and forwards telemetry data via WiFi UDP or BLE
- **Web Configuration** — dark-themed responsive WebUI served from the ESP32's own WiFi AP
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

Connect **GPIO20 (RX)** and **GPIO21 (TX)** to the radio's Trainer or AUX serial port. Power the module from the radio's 3.3V supply or USB.

- **Radios with built-in Bluetooth** (e.g. Radiomaster Boxer, TX16S): Set `BLUETOOTH = ON` in EdgeTX. The radio sends AT commands + trainer/telemetry data over its internal BT UART. BTWifiSerial responds to the AT handshake and processes the data stream transparently — no wiring needed if replacing the internal BT module, or connect via BLE.
- **Radios with external module bay / UART**: Wire the ESP32-C3 to an available UART port (Trainer port, AUX1, or AUX2). Configure the appropriate serial mode in the WebUI.

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

Outputs a standard SBUS signal on the TX pin at 100000 baud 8E2 with hardware signal inversion.

- **Use case:** Feed channel data to any SBUS-compatible receiver, flight controller, or simulator dongle.
- **Channel mapping:** PPM 1050–1950 → SBUS 172–1811 (16 channels, unused channels held at center 992).
- **Frame rate:** ~14ms. Frame-lost flag is set automatically if no BLE data arrives for 1 second.

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

### Telemetry Output

When using either S.PORT mode, parsed telemetry packets can be forwarded to:

| Output | Description |
|--------|-------------|
| **WiFi UDP** | The ESP32 starts its own WiFi AP (`BTWifiSerial` / `12345678`). Connect your phone or tablet to it and receive 8-byte `SportPacket` datagrams broadcast on UDP port 5010 (configurable). |
| **BLE** | Packets are re-framed with 0x7E framing and sent as BLE notifications on the FrSky characteristic (UUID `0xFFF6`). A BLE client app must be connected. |

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
| Blinking (500ms) | AP mode (WebUI active) |
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

## License

MIT
