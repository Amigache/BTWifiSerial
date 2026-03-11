# BTWifiSerial — LUA Serial Communication Protocol (v2)

Reference documentation for the binary multi-channel protocol between the
ESP32-C3 firmware and the EdgeTX Lua scripts (`BTWFS/main.lua`, `btwfs.lua`).

---

## 1. Physical Layer

| Parameter        | Value                |
|------------------|----------------------|
| Interface        | UART1                |
| Baud rate        | 115 200              |
| Format           | 8N1 (no parity)      |
| Flow control     | None                 |
| TX GPIO (ESP32)  | GPIO 21              |
| RX GPIO (ESP32)  | GPIO 20              |
| EdgeTX AUX mode  | **LUA** at 115200    |

---

## 2. Frame Structure

All frames — in both directions — use the same envelope:

```
[SYNC : 0xAA] [CH : 1] [TYPE : 1] [LEN : 1] [PAYLOAD : LEN bytes] [CRC : 1]
```

| Field   | Bytes | Description                                                              |
|---------|-------|--------------------------------------------------------------------------|
| SYNC    | 1     | Always `0xAA`                                                            |
| CH      | 1     | Logical channel (see §3)                                                 |
| TYPE    | 1     | Frame type within the channel                                            |
| LEN     | 1     | Payload length in bytes (0–255)                                          |
| PAYLOAD | LEN   | Frame data                                                               |
| CRC     | 1     | `XOR(CH, TYPE, LEN, payload[0..LEN-1])` — SYNC is **not** included      |

The parser is fully generic: no frame sizes are hardcoded. A parser that does
not recognise a (CH, TYPE) pair simply discards the frame after CRC validation.

---

## 3. Logical Channels

| CH     | Name      | Purpose                                               |
|--------|-----------|-------------------------------------------------------|
| `0x01` | **PREF**  | Preferences / configuration — bidirectional           |
| `0x02` | **INFO**  | Status, channel data, BLE scan — bidirectional        |
| `0x03` | **TRANS** | Transparent byte passthrough — bidirectional          |

---

## 4. CH_PREF — Preferences Channel

### 4.1 ESP32 → Lua

#### `PT_PREF_BEGIN` `0x01`
Signals the start of a full preference list transmission.

| Offset | Bytes | Description              |
|--------|-------|--------------------------|
| 0      | 1     | Total number of prefs to follow |

#### `PT_PREF_ITEM` `0x02`
One full preference descriptor. Payload format:

```
id(1) type(1) flags(1) label_len(1) label(N) <type-specific>
```

**Type-specific data:**

| Type       | Code | Additional bytes                                                                     |
|------------|------|--------------------------------------------------------------------------------------|
| `FT_ENUM`  | `0`  | `opt_count(1)  cur_idx(1)  [opt_len(1)  opt_str(N)] × opt_count`  (0-based index)  |
| `FT_STRING`| `1`  | `max_len(1)  val_len(1)  val(N)`                                                     |
| `FT_INT`   | `2`  | `min_lo(1) min_hi(1)  max_lo(1) max_hi(1)  val_lo(1) val_hi(1)`  (LE int16)        |
| `FT_BOOL`  | `3`  | `val(1)` — `0`=false, `1`=true                                                       |

**Flags byte (PF\_\*):**

| Bit | Mask   | Name          | Description                        |
|-----|--------|---------------|------------------------------------|
| 0   | `0x01` | `PF_RESTART`  | Device restart needed to apply     |
| 1   | `0x02` | `PF_RDONLY`   | Read-only — PREF_SET is ignored    |
| 2   | `0x04` | `PF_DASHBOARD`| Show on Dashboard System section   |

#### `PT_PREF_END` `0x03`
Signals the end of the current preference list. No payload.

#### `PT_PREF_UPDATE` `0x04`
Value-only update for one preference (fired after a cascade or a
non-restart-requiring save). Payload:

```
id(1) type(1) <value — same encoding as the type-specific value in PREF_ITEM>
```

For `FT_ENUM`/`FT_BOOL` the value is the new index/bool byte.

#### `PT_PREF_ACK` `0x05`
Result of the last `PREF_SET`. Flushed to UART before any device restart.

| Offset | Description                 |
|--------|-----------------------------|
| 0      | Preference ID               |
| 1      | `0x00` = success, `0x01` = error |

### 4.2 Lua → ESP32

#### `PT_PREF_REQUEST` `0x10`
Request a full `PREF_BEGIN + PREF_ITEM × N + PREF_END` sequence. No payload.

#### `PT_PREF_SET` `0x11`
Set a preference value. Payload:

```
id(1) type(1) <value — same encoding as PREF_ITEM type-specific value>
```

### 4.3 Preference IDs

| ID     | Name            | Type       | Flags                          | Options / Constraints                       |
|--------|-----------------|------------|--------------------------------|---------------------------------------------|
| `0x01` | `AP_MODE`       | `FT_ENUM`  | `PF_RESTART \| PF_DASHBOARD`   | `"Normal"` / `"WiFi AP"` / `"Telem AP"`    |
| `0x02` | `DEV_MODE`      | `FT_ENUM`  | `PF_RESTART \| PF_DASHBOARD`   | `"Trainer IN"` / `"Trainer OUT"` / `"Telemetry"` |
| `0x03` | `TELEM_OUT`     | `FT_ENUM`  | `PF_RESTART \| PF_DASHBOARD`   | `"WiFi UDP"` / `"BLE"` / `"Off"`           |
| `0x04` | `MIRROR_BAUD`   | `FT_ENUM`  | `PF_RESTART`                   | `"57600"` / `"115200"`                     |
| `0x05` | `MAP_MODE`      | `FT_ENUM`  | —                              | `"GV"` (Global Variables) / `"TR"` (Trainer)|
| `0x06` | `BT_NAME`       | `FT_STRING`| `PF_DASHBOARD`                 | maxLen = 15                                 |
| `0x07` | `AP_SSID`       | `FT_STRING`| `PF_RESTART`                   | maxLen = 15                                 |
| `0x08` | `UDP_PORT`      | `FT_STRING`| `PF_RESTART`                   | maxLen = 5, range 1024–65535                |
| `0x09` | `AP_PASS`       | `FT_STRING`| `PF_RESTART`                   | maxLen = 15, minLen = 8                     |

### 4.4 Business-logic cascades

When certain prefs change, the firmware may alter other prefs and sends
`PREF_UPDATE` for each cascade **before** `PREF_ACK`:

| Trigger                                  | Cascade                              |
|------------------------------------------|--------------------------------------|
| `DEV_MODE` → Trainer IN or Trainer OUT   | `TELEM_OUT` forced to `Off`          |
| `AP_MODE` → WiFi AP                      | `TELEM_OUT` forced to `Off` if BLE   |
| `AP_MODE` → Normal                       | `TELEM_OUT` cleared if WiFi UDP      |

---

## 5. CH_INFO — Information Channel

### 5.1 ESP32 → Lua

#### `PT_INFO_CHANNELS` `0x01`
Sent at ~100 Hz while BLE is connected and channel data is fresh.

Payload: 8 × int16 big-endian = **16 bytes**.  
Range: −1024 (−100 %) … 0 (centre) … +1024 (+100 %).

#### `PT_INFO_STATUS` `0x02`
Sent every 500 ms.

| Bit | Description                  |
|-----|------------------------------|
| 0   | `1` = BLE connected          |
| 1   | `1` = WiFi AP has ≥1 client  |
| 2   | `1` = BLE connecting         |

#### `PT_INFO_BEGIN` `0x03`
Start of info list. Payload: `count(1)`.

#### `PT_INFO_ITEM` `0x04`
One read-only info field. Payload:

```
id(1) type(1) label_len(1) label(N) value(var)
```

Value encoding is the same as `PREF_ITEM` for that type (no options/maxLen).

#### `PT_INFO_END` `0x05`
End of info list. No payload.

#### `PT_INFO_UPDATE` `0x06`
Value-only update for one info field. Payload: `id(1) type(1) value(var)`.

#### `PT_INFO_SCAN_STATUS` `0x07`
BLE scan state notification. Payload: `state(1) count(1)`.

`state`: `0`=idle, `1`=scanning, `2`=complete (results are being sent).

#### `PT_INFO_SCAN_ITEM` `0x08`
One BLE scan result entry.

```
idx(1) rssi_s8(1) flags(1) name_len(1) name(N) addr(17 null-padded)
```

`flags` bit 0: device advertises FrSky service.

### 5.2 Lua → ESP32

| Type                         | Code   | Payload      | Description                     |
|------------------------------|--------|--------------|---------------------------------|
| `PT_INFO_REQUEST`            | `0x10` | none         | Request full info + pref lists  |
| `PT_INFO_HEARTBEAT`          | `0x11` | none         | Tools script alive              |
| `PT_INFO_BLE_SCAN`           | `0x12` | none         | Start BLE scan                  |
| `PT_INFO_BLE_CONNECT`        | `0x13` | `idx(1)`     | Connect to scan result          |
| `PT_INFO_BLE_DISCONNECT`     | `0x14` | none         | Disconnect BLE                  |
| `PT_INFO_BLE_FORGET`         | `0x15` | none         | Forget saved BLE device         |
| `PT_INFO_BLE_RECONNECT`      | `0x16` | none         | Reconnect to saved device       |

### 5.3 Info IDs

| ID     | Name         | Type       | Description                          |
|--------|--------------|------------|--------------------------------------|
| `0x01` | `FIRMWARE`   | `FT_STRING`| Build timestamp `"DDMMYYYY HHMM"`    |
| `0x02` | `BT_ADDR`    | `FT_STRING`| Local BT MAC `"XX:XX:XX:XX:XX:XX"`  |
| `0x03` | `REM_ADDR`   | `FT_STRING`| Saved remote BT MAC or `"(none)"`    |

---

## 6. CH_TRANS — Transparent Channel

Raw bytes wrapped in the standard frame envelope. Both directions.

| TYPE              | Code   | Payload description                                       |
|-------------------|--------|-----------------------------------------------------------|
| `PT_TRANS_SBUS`   | `0x01` | Raw SBUS frame bytes                                      |
| `PT_TRANS_SPORT`  | `0x02` | S.PORT packet: `physId(1) primId(1) dataId(2 LE) value(4 LE)` |
| `PT_TRANS_FRSKY`  | `0x03` | Raw FrSky CC2540 bytes                                    |

The CRC in the outer frame protects the payload bytes.
No inner CRC is added — the outer CRC is sufficient.

---

## 7. Initialisation Sequence

On script start (or after a device restart), the Lua side sends:

```
PT_INFO_REQUEST  (CH_INFO 0x10)   — triggers prefs + info + status
PT_PREF_REQUEST  (CH_PREF 0x10)   — (redundant but harmless, can be omitted)
```

The ESP32 responds with:
```
PREF_BEGIN → PREF_ITEM × 9 → PREF_END
INFO_BEGIN → INFO_ITEM × 3 → INFO_END
INFO_STATUS
```

If prefs are not received within ~2 s, the Lua side retries the request.

---

## 8. Heartbeat & Serial Ownership

The Tools script (`main.lua`) sends `PT_INFO_HEARTBEAT` every 50 ms
**and** writes `getTime()` to SHM slot 1.

The background Function script (`btwfs.lua`) reads SHM slot 1; if the
heartbeat is < 80 ms old, it yields the serial port to the Tools script.
This prevents both scripts from consuming the same UART bytes.

On the firmware side, `s_lastToolsCmdMs` is updated on every valid frame
received. If no frame is received for 15 s, the periodic 30 s resync is
suspended to reduce traffic when the radio screen is idle.

---

## 9. Lua Module Summary

| File                                    | Purpose                                          |
|-----------------------------------------|--------------------------------------------------|
| `BTWFS/lib/serial_proto.lua`            | Protocol constants, parser, builders, decoders   |
| `BTWFS/lib/store.lua`                   | App state (prefs, info, channels, scan, status)  |
| `BTWFS/main.lua`                        | Serial loop, dispatcher, SHM heartbeat           |
| `BTWFS/pages/settings.lua`             | Data-driven settings list from `store.prefs`     |
| `BTWFS/pages/dashboard.lua`            | Data-driven system section + live channel bars   |
| `SCRIPTS/FUNCTIONS/btwfs.lua`          | Background: channel injection + TLM forward      |


---

## 1. Physical Layer

| Parameter        | Value                |
|------------------|----------------------|
| Interface        | UART (UART1)         |
| Baud rate        | 115 200              |
| Format           | 8N1 (no parity)      |
| Flow control     | None                 |
| TX GPIO (ESP32)  | GPIO 21              |
| RX GPIO (ESP32)  | GPIO 20              |
| EdgeTX AUX mode  | **LUA** at 115200    |

The ESP32 acts as the **transmission master**: it sends frames periodically
without the script requesting them, except for the special `CMD_REQ_INFO`
command.

---

## 2. Frame Structure

Every frame — whether sent by the ESP32 or by the script — follows the same
layout:

```
[SYNC 0xAA] [TYPE] [PAYLOAD …] [XOR_CRC]
```

- **SYNC** `0xAA` — synchronisation byte, always the first byte.
- **TYPE** — identifies the frame type (see tables below).
- **PAYLOAD** — zero or more data bytes; length is fixed per type.
- **XOR_CRC** — XOR of all bytes from `TYPE` through the last payload byte
  (`SYNC` is **not** included in the CRC).

> **CRC algorithm:** `crc = TYPE ^ payload[0] ^ payload[1] ^ … ^ payload[N-1]`

---

## 3. Frames: ESP32 → Script (TX)

### 3.1 Channel frame — `T_CH` `0x43`  (19 bytes)

Sent at **~100 Hz** while BLE is connected and channel data is fresh
(500 ms staleness window).

```
[AA] [43] [ch1_H] [ch1_L] [ch2_H] [ch2_L] … [ch8_H] [ch8_L] [CRC]
```

| Field    | Offset | Bytes | Description                                           |
|----------|--------|-------|-------------------------------------------------------|
| SYNC     | 0      | 1     | `0xAA`                                                |
| TYPE     | 1      | 1     | `0x43`                                                |
| CH1…CH8  | 2–17   | 16    | 8 channels × 2 bytes, **signed int16 big-endian**     |
| CRC      | 18     | 1     | XOR of bytes [1..17]                                  |

**Channel value range:** −1024 (−100 %) … 0 (centre) … +1024 (+100 %).

Conversion from PPM:
```
signed_val = (ppm - 1500) × 1024 / 450
```
where `ppm` is in the range 1050–1950 µs.

---

### 3.2 BLE status frame — `T_ST` `0x53`  (4 bytes)

Sent every **500 ms**.

```
[AA] [53] [STATUS] [CRC]
```

| Field   | Offset | Bits | Description              |
|---------|--------|------|--------------------------|
| STATUS  | 2      | bit0 | `1` = BLE connected      |
| CRC     | 3      | —    | XOR of bytes [1..2]      |

Upper bits of `STATUS` are reserved (always 0).

---

### 3.3 ACK frame — `T_ACK` `0x41`  (4 bytes)

Sent in direct response to any received command.

```
[AA] [41] [RESULT] [CRC]
```

| Field   | Value  | Description                  |
|---------|--------|------------------------------|
| RESULT  | `0x00` | Success                      |
| RESULT  | `0x01` | Error / unknown command      |

The firmware performs `uart_wait_tx_done` before any restart to guarantee the
script receives the ACK.

---

### 3.4 Config frame — `T_CFG` `0x47`  (7 bytes)

Sent in three situations:
- Immediately upon receiving `CMD_REQ_INFO`.
- Periodically every **30 s** (resync).
- Inside `luaSerialLoop()` when 30 s have elapsed since the last send.
- After a `CMD_MAP_GV` or `CMD_MAP_TR` command (before the ACK).

```
[AA] [47] [AP_MODE] [DEV_MODE] [TLM_OUTPUT] [MAP_MODE] [CRC]
```

| Field       | Offset | Value | Description                                       |
|-------------|--------|-------|---------------------------------------------------|
| AP_MODE     | 2      | `0`   | AP mode **active** (Lua overlay, inputs blocked)  |
| AP_MODE     | 2      | `1`   | Normal operation (AP off)                         |
| AP_MODE     | 2      | `2`   | Telemetry AP (WiFi AP active, Lua input continues)|
| DEV_MODE    | 3      | `0`   | Device mode = **Trainer IN**  (BLE Central)       |
| DEV_MODE    | 3      | `1`   | Device mode = **Trainer OUT** (BLE Peripheral)    |
| DEV_MODE    | 3      | `2`   | Device mode = **Telemetry**                       |
| TLM_OUTPUT  | 4      | `0`   | Telemetry output = **WiFi UDP**                   |
| TLM_OUTPUT  | 4      | `1`   | Telemetry output = **BLE**                        |
| TLM_OUTPUT  | 4      | `2`   | Telemetry output = **Off** (discard packets)      |
| MAP_MODE    | 5      | `0`   | Trainer Map = **GV** (Global Variables GV1–GV8)   |
| MAP_MODE    | 5      | `1`   | Trainer Map = **TR** (Trainer channels TR1–TR8)   |
| CRC         | 6      |       | XOR of bytes [1..5]                               |

The script uses this frame to sync the settings menu and to transition the
application state to `AP_ACTIVE` when `AP_MODE == 0`.  When `AP_MODE == 2`
(Telemetry AP), the script stays in `RUNNING` state — the WiFi AP is active
for telemetry UDP output, but Lua UI input is not blocked.

---

### 3.5 Build info frame — `T_INF` `0x49`  (15 bytes)

Sent together with `T_CFG` in response to `CMD_REQ_INFO` and during the
30 s periodic resync.

```
[AA] [49] [D1] [D2] [M1] [M2] [Y1] [Y2] [Y3] [Y4] [H1] [H2] [Mi1] [Mi2] [CRC]
```

| Field      | Offset | Bytes | Description                                     |
|------------|--------|-------|-------------------------------------------------|
| Timestamp  | 2–13   | 12    | Build date/time in ASCII: `DDMMYYYYHHMM`        |
| CRC        | 14     | 1     | XOR of bytes [1..13]                            |

Example: `"07032026"` + `"1530"` → compiled on 7 March 2026 at 15:30.

---

### 3.6 System info frame — `T_SYS` `0x59`  (91 bytes)

Sent together with `T_CFG` + `T_INF` in response to `CMD_REQ_INFO` and during
the 30 s periodic resync (see §8 for gating conditions).

```
[AA] [59] [SERIAL_MODE] [BT_NAME×16] [LOCAL_ADDR×18] [REMOTE_ADDR×18]
         [AP_SSID×16] [UDP_PORT×2] [AP_PASS×16] [BAUD_IDX] [CRC]
```

| Field         | Offset  | Bytes | Description                                             |
|---------------|---------|-------|---------------------------------------------------------|
| SERIAL\_MODE  | 2       | 1     | `OutputMode` enum value (see table below)               |
| BT\_NAME      | 3–18    | 16    | BLE device name, ASCII, null-padded                     |
| LOCAL\_ADDR   | 19–36   | 18    | Local MAC address, format `"XX:XX:XX:XX:XX:XX\0"`       |
| REMOTE\_ADDR  | 37–54   | 18    | Saved remote MAC address (or `0x00` bytes if none)      |
| AP\_SSID      | 55–70   | 16    | WiFi AP SSID, ASCII, null-padded                        |
| UDP\_PORT     | 71–72   | 2     | Telemetry UDP port, **big-endian** uint16               |
| AP\_PASS      | 73–88   | 16    | WiFi AP password, ASCII, null-padded                    |
| BAUD\_IDX     | 89      | 1     | Mirror baud rate: `0` = 57600, `1` = 115200             |
| CRC           | 90      | 1     | XOR of bytes [1..89]                                    |

**`SERIAL_MODE` values:**

| Value | Name            | Description                                          |
|-------|-----------------|------------------------------------------------------|
| 0     | FRSKY           | FrSky CC2540 trainer protocol (115200 8N1)           |
| 1     | SBUS            | SBUS trainer (100000 8E2 inverted)                   |
| 2     | SPORT\_BT       | S.PORT telemetry via BT framing (115200 8N1, XOR CRC)|
| 3     | SPORT\_MIRROR   | S.PORT telemetry mirror from AUX2 (57600/115200, raw)|
| 4     | LUA\_SERIAL     | LUA Serial mode (this protocol)                      |

---

## 4. Frames: Script → ESP32 (RX)

The script sends two types of frames to the ESP32.

### 4.1 Command frame — `T_CMD` `0x02`  (4 bytes)

```
[AA] [02] [CMD] [CRC]
```

| Field | Offset | Value  | Description                      |
|-------|--------|--------|----------------------------------|
| SYNC  | 0      | `0xAA` |                                  |
| TYPE  | 1      | `0x02` | Command frame type               |
| CMD   | 2      | —      | Command code (see table below)   |
| CRC   | 3      |        | `TYPE ^ CMD`                     |

### Command codes

| CMD    | Name                 | Description                                                    |
|--------|----------------------|----------------------------------------------------------------|
| `0x01` | `CMD_TOGGLE_AP`      | Toggle AP mode (writes NVS flag + reboot)                      |
| `0x02` | `CMD_AP_ON`          | Enable AP mode (writes NVS flag + reboot). If telemetry output is BLE, it is automatically reset to Off before switching. |
| `0x03` | `CMD_AP_OFF`         | Disable AP mode (back to normal + reboot). Also resets telemetry output to Off if it was WiFi UDP. |
| `0x06` | `CMD_REQ_INFO`       | Request immediate `T_CFG` + `T_INF` + `T_SYS` burst           |
| `0x07` | `CMD_BLE_SCAN`       | Start BLE scan (Trainer IN mode only)                          |
| `0x08` | `CMD_HEARTBEAT`      | No-op keepalive — updates the Tools-active idle timer (see §10) |
| `0x09` | `CMD_BLE_DISCONNECT` | Disconnect BLE (keep saved address)                            |
| `0x0A` | `CMD_BLE_FORGET`     | Forget saved BLE device (clears address). Triggers a new `T_SYS` frame. |
| `0x0B` | `CMD_BLE_RECONNECT`  | Reconnect to saved BLE device                                  |
| `0x0C` | `CMD_TELEM_WIFI`     | Set telemetry output → WiFi UDP (save + restart)               |
| `0x0D` | `CMD_TELEM_BLE`      | Set telemetry output → BLE (save + restart)                    |
| `0x0E` | `CMD_TELEM_OFF`      | Set telemetry output → Off/None (save + restart)               |
| `0x0F` | `CMD_BAUD_57600`     | Set S.PORT Mirror baud rate → 57600 (save + restart)           |
| `0x10`–`0x1F` | `CMD_BLE_CONNECT_N` | Connect to scan result N (0–15)                      |
| `0x20` | `CMD_DEV_TRAINER_IN` | Set device mode → Trainer IN (save + restart). Clears telemetry output to Off. |
| `0x21` | `CMD_DEV_TRAINER_OUT`| Set device mode → Trainer OUT (save + restart). Clears telemetry output to Off. |
| `0x22` | `CMD_DEV_TELEMETRY`  | Set device mode → Telemetry (save + restart)                   |
| `0x23` | `CMD_BAUD_115200`    | Set S.PORT Mirror baud rate → 115200 (save + restart)          |
| `0x24` | `CMD_MAP_GV`         | Set Trainer Map → GV (save, send T_CFG, no reboot)             |
| `0x25` | `CMD_MAP_TR`         | Set Trainer Map → TR (save, send T_CFG, no reboot)             |

Commands `0x01`, `0x02`, `0x03`, `0x0C`, `0x0D`, `0x0E`, `0x0F`, `0x20`,
`0x21`, `0x22`, `0x23` receive a `T_ACK` before the ESP32 reboots.
Commands `0x08`, `0x09`, `0x0A`, `0x0B`, `0x10`–`0x1F`, `0x24`, `0x25`
receive a `T_ACK` immediately (no reboot).  Commands `0x24` and `0x25`
additionally send a `T_CFG` frame **before** the `T_ACK` so the script can
update its state immediately.  Command `0x06` does **not** generate a
`T_ACK`; the response is the three info frames directly.

---

### 4.2 String set frame — `T_STR_SET` `0x4E`  (20 bytes)

Sent by `BTWifiSerial.lua` to write a string-valued configuration field.

```
[AA] [4E] [SUB_CMD] [DATA × 16] [CRC]
```

| Field    | Offset | Bytes | Description                                         |
|----------|--------|-------|-----------------------------------------------------|
| SYNC     | 0      | 1     | `0xAA`                                              |
| TYPE     | 1      | 1     | `0x4E`                                              |
| SUB_CMD  | 2      | 1     | Sub-command code (see table below)                  |
| DATA     | 3–18   | 16    | UTF-8 / ASCII string, null-padded to 16 bytes       |
| CRC      | 19     | 1     | XOR of bytes [1..18]                                |

**Sub-command codes:**

| SUB_CMD | Name               | Effect                                                         |
|---------|--------------------|----------------------------------------------------------------|
| `0x01`  | `STR_BT_NAME`      | Update BLE advertised name (saves, updates advertising immediately — **no reboot**) |
| `0x02`  | `STR_AP_SSID`      | Update WiFi AP SSID (saves + reboot)                           |
| `0x03`  | `STR_UDP_PORT`     | Update UDP broadcast port — DATA is a numeric ASCII string, range 1024–65535 (saves + reboot) |
| `0x04`  | `STR_AP_PASS`      | Update WiFi AP password (saves + reboot)                       |

After processing, the ESP32 sends `T_ACK` (`0x00` on success, `0x01` on
validation failure).  For `STR_BT_NAME` (no reboot), the firmware also sends
an updated `T_SYS` frame so the script can refresh its displayed values.

---

### 4.3 Telemetry forward frame — `T_TLM` `0x54`  (12 bytes)

Sent by `btwfs.lua` (background function script) to forward radio S.PORT
telemetry packets to the ESP32 for re-distribution via WiFi UDP or BLE.

```
[AA] [54] [physId] [primId] [dataId_L] [dataId_H] [v0] [v1] [v2] [v3] [CRC]
```

| Field      | Offset | Bytes | Description                          |
|------------|--------|-------|--------------------------------------|
| SYNC       | 0      | 1     | `0xAA`                               |
| TYPE       | 1      | 1     | `0x54`                               |
| physId     | 2      | 1     | Physical sensor ID                   |
| primId     | 3      | 1     | Primitive ID (`0x10` = DATA\_FRAME)  |
| dataId     | 4–5    | 2     | Sensor data ID, **little-endian**    |
| value      | 6–9    | 4     | 32-bit sensor value, **little-endian** |
| CRC        | 10     | 1     | XOR of bytes [1..9]                  |

The ESP32 initialises the telemetry output subsystem (`sportOutputInit()`) on
the first received `T_TLM` frame and routes each packet to the configured
output (`TelemetryOutput::WIFI_UDP` or `TelemetryOutput::BLE`).  If the
configured output is `TelemetryOutput::NONE`, the packet is silently
discarded.

`btwfs.lua` calls `sportTelemetryPop()` (guarded by a capability check) to
obtain packets from the EdgeTX telemetry buffer, limiting to 8 packets per
script frame to avoid UART overrun.

---

## 5. Script State Machine

```
           ┌──────────────────────────────────────────────────────┐
           │  INIT  (appState = 1)                                │
           │  • Sends CMD_REQ_INFO every 500 ms                   │
           │  • Waits for gotConfig AND gotInfo AND gotSys        │
           │  • Timeout 5 s → RUNNING or DISCONNECTED             │
           └────────┬────────────────────────┬────────────────────┘
                    │ all three received      │ timeout + connected
                    ▼                         ▼
          ┌──────────────────┐     ┌──────────────────────┐
          │  RUNNING  (2)    │◄────│  DISCONNECTED  (3)   │
          │  normal UI,      │     │  "Board not          │
          │  inputs active   │────►│  Connected" badge    │
          └────────┬─────────┘     └──────────────────────┘
                   │ T_CFG with AP_MODE=0
                   ▼
          ┌──────────────────────┐
          │  AP_ACTIVE  (4)      │
          │  orange overlay      │
          │  inputs blocked      │
          └──────────┬───────────┘
                     │ T_CFG with AP_MODE=1 or 2
                     ▼
                  RUNNING

Note: `AP_MODE=2` (Telemetry AP) transitions to `RUNNING`, **not**
`AP_ACTIVE`.  The WiFi AP is active for telemetry UDP output, but Lua UI
input continues normally — no overlay is shown.
```

### Transition table

| From           | Condition                            | To             |
|----------------|--------------------------------------|----------------|
| `INIT`         | gotConfig ∧ gotInfo ∧ gotSys         | `RUNNING`      |
| `INIT`         | gotConfig ∧ gotInfo ∧ gotSys ∧ AP=0  | `AP_ACTIVE`    |
| `INIT`         | gotConfig ∧ gotInfo ∧ gotSys ∧ AP=2  | `RUNNING`      |
| `INIT`         | timeout 5 s + boardConnected         | `RUNNING`      |
| `INIT`         | timeout 5 s + !boardConnected        | `DISCONNECTED` |
| `RUNNING`      | !boardConnected                      | `DISCONNECTED` |
| `RUNNING`      | T_CFG with AP_MODE=0                 | `AP_ACTIVE`    |
| `RUNNING`      | T_CFG with AP_MODE=2                 | `RUNNING`      |
| `DISCONNECTED` | boardConnected                       | `RUNNING`      |
| `AP_ACTIVE`    | T_CFG with AP_MODE=1 or 2            | `RUNNING`      |
| `AP_ACTIVE`    | Successful ACK for CMD_AP_ON         | `AP_ACTIVE`    |

---

## 6. Fast-Load Mechanism (CMD_REQ_INFO)

Without this mechanism, the script could wait up to 30 s to receive `T_CFG`,
`T_INF`, and `T_SYS` (because the ESP32 sends them on a periodic timer).

**Initialisation sequence:**

```
Script (INIT state)                 ESP32
     │                                │
     │─── CMD_REQ_INFO (0x06) ───────►│
     │                                │──► sendConfigFrame()
     │                                │──► sendInfoFrame()
     │                                │──► sendSysFrame()
     │◄─── T_CFG  (7 bytes)  ────────│
     │◄─── T_INF  (15 bytes) ────────│
     │◄─── T_SYS  (56 bytes) ────────│
     │                                │
     │  gotConfig = true              │
     │  gotInfo   = true              │
     │  gotSys    = true              │
     │──► RUNNING (or AP_ACTIVE)      │
```

The script repeats `CMD_REQ_INFO` every 500 ms while in `INIT` state until all
three frames arrive or the 5 s timeout expires.

---

## 7. AP Mode Behaviour

### 7.1 Regular AP Mode (`BOOT_AP_MODE = 1`)

When the board boots into **AP mode** (NVS flag set) and `serialMode` is
`LUA_SERIAL`:

1. `startApMode()` calls `luaSerialInit()` then `luaSerialSetApMode(1)`.
2. The AP mode loop runs `luaSerialLoop()` alongside `webUiLoop()`.
3. `T_CFG` frames carry `AP_MODE = 0` (AP active, input blocked).
4. The script receives `T_CFG` with `AP_MODE = 0` → transitions to `AP_ACTIVE`.
5. The orange overlay is displayed; UI inputs are blocked.
6. `T_CH` (channel) frames are **not** sent in AP mode because BLE is inactive;
   `T_ST` reports BLE disconnected.

To exit AP mode the user either uses the web UI or sends `CMD_AP_OFF` from the
radio, which writes the NVS flag and reboots the ESP32.

### 7.2 Telemetry AP Mode (`BOOT_TELEM_AP = 2`)

When the board boots into **Telemetry AP mode**, the WiFi AP is started for
telemetry UDP output, but the Lua serial protocol operates normally:

1. `startTelemetryApMode()` calls `luaSerialInit()` then
   `luaSerialSetApMode(2)`.
2. The telemetry AP loop runs `luaSerialLoop()` alongside `webUiLoop()`.
3. `T_CFG` frames carry `AP_MODE = 2` (telemetry AP, Lua continues normally).
4. The script receives `T_CFG` with `AP_MODE = 2` → stays in `RUNNING`.
5. **No overlay is shown** — the user can continue using the Lua UI normally.
6. `T_CH` frames are **not** sent (BLE is inactive due to single-radio
   constraint on ESP32-C3), but `T_TLM` frames from the radio are accepted
   and forwarded via WiFi UDP.
7. The LED blinks at 500 ms (same pattern as regular AP mode) to indicate
   the WiFi AP is active.

This mode is triggered automatically when the user selects WiFi UDP as the
telemetry output via `CMD_TELEM_WIFI` (`0x0C`).

---

## 8. Periodic Timers (ESP32)

| Event                        | Interval            | Notes                                           |
|------------------------------|---------------------|-------------------------------------------------|
| Channel frame (`T_CH`)       | 10 ms (~100 Hz)     | Only while BLE is connected and data is fresh   |
| Status frame (`T_ST`)        | 500 ms              | Always                                          |
| CFG / INF / SYS resync       | 30 000 ms           | Only while Tools script is active (see §10)     |
| Response to `CMD_REQ_INFO`   | Immediate           | Resets the 30 s periodic timer to avoid double-fire |

---

## 9. Scan Frames (ESP32 → Script)

These frames are sent during a BLE scan initiated by `CMD_BLE_SCAN`. The
firmware drip-feeds results one frame per loop tick to avoid UART overrun.

### 9.1 Scan status — `T_SCAN_STATUS` `0x44`  (5 bytes)

```
[AA] [44] [STATE] [COUNT] [CRC]
```

| Field  | Value | Description                              |
|--------|-------|------------------------------------------|
| STATE  | `1`   | Scan in progress                         |
| STATE  | `0`   | Scan complete / idle                     |
| COUNT  | 0–15  | Number of entries that will follow       |
| CRC    |       | XOR of bytes [1..3]                      |

A `T_SCAN_STATUS` with `STATE=1` is sent when the scan starts.  A second one
with `STATE=0` and the final result count is sent when the scan finishes, just
before the first `T_SCAN_ENTRY` frames begin streaming.

### 9.2 Scan entry — `T_SCAN_ENTRY` `0x52`  (40 bytes)

```
[AA] [52] [IDX] [RSSI] [HAS_FRSKY] [NAME×16] [ADDR×18] [CRC]
```

| Field      | Offset | Bytes | Description                                         |
|------------|--------|-------|-----------------------------------------------------|
| IDX        | 2      | 1     | Entry index (0–15). Used in `CMD_BLE_CONNECT_N`.    |
| RSSI       | 3      | 1     | Signal strength, signed (cast to `int8_t`)          |
| HAS_FRSKY  | 4      | 1     | `1` if device advertises FrSky UUID, else `0`       |
| NAME       | 5–20   | 16    | Device name, ASCII, null-padded                     |
| ADDR       | 21–38  | 18    | MAC address, format `"XX:XX:XX:XX:XX:XX\0"`         |
| CRC        | 39     | 1     | XOR of bytes [1..38]                                |

The script uses `IDX` to send `CMD_BLE_CONNECT_N` (`0x10 + IDX`) to connect.
Devices with `HAS_FRSKY=1` are presented with a star (★) in the UI.

---

## 10. Tools-Activity Gating

The heavy periodic resync burst (`T_CFG` + `T_INF` + `T_SYS` every 30 s) is
gated by a Tools-active idle timer on the firmware side.

**Mechanism:**

- Every time the firmware receives a valid `T_CMD` or `T_STR_SET` frame from
  the script (any command), it sets an internal timestamp `lastToolsCmdMs`.
- The periodic resync only fires if less than **15 000 ms** have elapsed since
  `lastToolsCmdMs` (i.e. the Tools script was interacting recently).
- `BTWifiSerial.lua` sends `CMD_HEARTBEAT` (`0x08`) every **5 s** while in
  `RUNNING` state to keep `lastToolsCmdMs` fresh and maintain the resync
  window without any user action.

**Rationale:** `btwfs.lua` (background Functions script) runs continuously but
never sends commands. Without gating, the firmware would blast a 91-byte
`T_SYS` + 15-byte `T_INF` + 7-byte `T_CFG` burst every 30 s into the UART
buffer — bytes that `btwfs.lua` would discard. With gating, those bursts only
happen when `BTWifiSerial.lua` (Tools) is actually open.

**Timeline example:**

```
t=0       User opens BTWifiSerial.lua → CMD_REQ_INFO sent
t=5s      CMD_HEARTBEAT → lastToolsCmdMs reset
t=10s     CMD_HEARTBEAT → lastToolsCmdMs reset
t=30s     Periodic resync fires (T_CFG + T_INF + T_SYS sent)
t=35s     User closes Tools script; CMD_HEARTBEAT stops
t=50s     (30s resync check) 15s > TOOLS_IDLE_TIMEOUT → resync suppressed
```

---

## 11. Protocol Constants Reference

| Symbol                | Value  | Description                                |
|-----------------------|--------|--------------------------------------------|
| `SYNC`                | `0xAA` | Synchronisation byte                       |
| `T_CH`                | `0x43` | Type: channel frame                        |
| `T_ST`                | `0x53` | Type: BLE status frame                     |
| `T_ACK`               | `0x41` | Type: ACK frame                            |
| `T_CFG`               | `0x47` | Type: config frame                         |
| `T_INF`               | `0x49` | Type: build info frame                     |
| `T_SYS`               | `0x59` | Type: system info frame (91 bytes)         |
| `T_SCAN_STATUS`       | `0x44` | Type: BLE scan state notification          |
| `T_SCAN_ENTRY`        | `0x52` | Type: BLE scan result entry                |
| `T_CMD`               | `0x02` | Type: command frame (Script → ESP32)       |
| `T_TLM`               | `0x54` | Type: telemetry forward frame (Script → ESP32) |
| `T_STR_SET`           | `0x4E` | Type: string set frame (Script → ESP32)    |
| `CMD_HEARTBEAT`       | `0x08` | Command: Tools-active keepalive            |
| `CMD_TELEM_OFF`       | `0x0E` | Command: telemetry output → Off            |
| `CMD_BAUD_57600`      | `0x0F` | Command: mirror baud → 57600              |
| `CMD_BAUD_115200`     | `0x23` | Command: mirror baud → 115200             |
| `TOOLS_IDLE_TIMEOUT`  | 15 000 ms | Inactivity window for periodic resync gating |
| `T_CMD`               | `0x02` | Type: command frame (RX)           |
| `CMD_TOGGLE_AP`       | `0x01` | Command: toggle AP mode            |
| `CMD_AP_ON`           | `0x02` | Command: AP mode on                |
| `CMD_AP_OFF`          | `0x03` | Command: AP mode off               |
| `CMD_REQ_INFO`        | `0x06` | Command: request CFG + INF + SYS   |
| `CMD_BLE_SCAN`        | `0x07` | Command: start BLE scan            |
| `CMD_BLE_DISCONNECT`  | `0x09` | Command: BLE disconnect            |
| `CMD_BLE_FORGET`      | `0x0A` | Command: forget saved device       |
| `CMD_BLE_RECONNECT`   | `0x0B` | Command: reconnect to saved device |
| `CMD_TELEM_WIFI`      | `0x0C` | Command: telemetry output → WiFi UDP (save + restart) |
| `CMD_TELEM_BLE`       | `0x0D` | Command: telemetry output → BLE (save + restart) |
| `CMD_DEV_TRAINER_IN`  | `0x20` | Command: device mode → Trainer IN (save + restart)  |
| `CMD_DEV_TRAINER_OUT` | `0x21` | Command: device mode → Trainer OUT (save + restart) |
| `CMD_DEV_TELEMETRY`   | `0x22` | Command: device mode → Telemetry (save + restart)   |
| `CMD_MAP_GV`          | `0x24` | Command: trainer map → GV (save + T_CFG, no reboot) |
| `CMD_MAP_TR`          | `0x25` | Command: trainer map → TR (save + T_CFG, no reboot) |
| `T_SCAN_STATUS`       | `0x44` | Type: BLE scan state notification  |
| `T_SCAN_ENTRY`        | `0x52` | Type: BLE scan result entry        |
| `T_TLM`               | `0x54` | Type: S.PORT telemetry fwd (RX)    |
