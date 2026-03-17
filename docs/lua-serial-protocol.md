# BTWifiSerial Lua Serial Protocol (v2)

Developer reference for UART communication between firmware and Lua scripts.

Primary implementation files:

- Firmware: `src/lua_serial.h`, `src/lua_serial.cpp`
- Lua tooling: `lua/SCRIPTS/TOOLS/BTWFS/lib/serial_proto.lua`
- Lua data store: `lua/SCRIPTS/TOOLS/BTWFS/lib/store.lua`

This document is intended for maintainers and future integrations.

---

## 1) Transport and physical layer

| Parameter | Value |
|---|---|
| Transport | UART1 |
| Baud | 115200 |
| Format | 8N1 |
| Flow control | None |
| ESP32 TX | GPIO21 |
| ESP32 RX | GPIO20 |

Radio AUX must be configured as **LUA @ 115200**.

---

## 2) Frame envelope

All frames use this envelope:

```text
[SYNC=0xAA][CH][TYPE][LEN][PAYLOAD...][CRC]
```

Rules:

- `LEN` is 0..255
- `CRC = XOR(CH, TYPE, LEN, PAYLOAD[0..LEN-1])`
- `SYNC` is not included in CRC

Parser behavior expectations:

- CRC mismatch -> drop frame
- Unknown `(CH, TYPE)` with valid CRC -> ignore payload and continue

---

## 3) Logical channels

| CH | Name | Purpose |
|---|---|---|
| `0x01` | `CH_PREF` | configuration/preferences |
| `0x02` | `CH_INFO` | runtime info, status, scans, channels |
| `0x03` | `CH_TRANS` | transparent runtime payloads |

All channels are logically bidirectional; practical usage depends on type.

---

## 4) `CH_PREF` (configuration channel)

### 4.1 ESP32 -> Lua

| Type | Name | Payload |
|---|---|---|
| `0x01` | `PT_PREF_BEGIN` | `count(1)` |
| `0x02` | `PT_PREF_ITEM` | `id(1) type(1) flags(1) label_len(1) label(N) type_data(...)` |
| `0x03` | `PT_PREF_END` | none |
| `0x04` | `PT_PREF_UPDATE` | `id(1) type(1) value(...)` |
| `0x05` | `PT_PREF_ACK` | `id(1) result(1)` |

`PT_PREF_ACK.result`:

- `0x00` success
- `0x01` rejected/error

### 4.2 Lua -> ESP32

| Type | Name | Payload |
|---|---|---|
| `0x10` | `PT_PREF_REQUEST` | none |
| `0x11` | `PT_PREF_SET` | `id(1) type(1) value(...)` |

### 4.3 Value type encoding (`FT_*`)

| Code | Name | Encoding |
|---|---|---|
| `0` | `FT_ENUM` | full item: `opt_count(1) cur_idx(1) [opt_len(1) opt(N)]*` |
| `1` | `FT_STRING` | full item: `max_len(1) val_len(1) val(N)` |
| `2` | `FT_INT` | full item: `min(int16 LE) max(int16 LE) value(int16 LE)` |
| `3` | `FT_BOOL` | `value(1)` |

### 4.4 Preference flags (`PF_*`)

| Bit | Mask | Meaning |
|---|---|---|
| 0 | `0x01` | restart required |
| 1 | `0x02` | read-only |
| 2 | `0x04` | dashboard-visible |
| 3 | `0x08` | numeric-only string (`FT_STRING`) |

### 4.5 Current preference ID map

| ID | Name | Type | Notes |
|---|---|---|---|
| `0x01` | `WIFI_MODE` | ENUM | `Off` / `AP` / `STA` |
| `0x02` | `DEV_MODE` | ENUM | `Trainer IN` / `Trainer OUT` / `Telemetry` |
| `0x03` | `TELEM_OUT` | ENUM | `WiFi UDP` / `BLE` / `Off` |
| `0x04` | `MIRROR_BAUD` | ENUM | `57600` / `115200` |
| `0x05` | `MAP_MODE` | ENUM | `GV` / `TR` |
| `0x06` | `BT_NAME` | STRING | max 15 |
| `0x07` | `AP_SSID` | STRING | max 15 |
| `0x08` | `UDP_PORT` | STRING | max 5 (validated as 1024..65535) |
| `0x09` | `AP_PASS` | STRING | max 15, min 8 |
| `0x0A` | `STA_SSID` | STRING | max 31 |
| `0x0B` | `STA_PASS` | STRING | max 63 |

---

## 5) `CH_INFO` (runtime information)

### 5.1 ESP32 -> Lua

| Type | Name | Payload |
|---|---|---|
| `0x01` | `PT_INFO_CHANNELS` | 8 x int16 BE (16 bytes) |
| `0x02` | `PT_INFO_STATUS` | `status(1)` |
| `0x03` | `PT_INFO_BEGIN` | `count(1)` |
| `0x04` | `PT_INFO_ITEM` | `id(1) type(1) label_len(1) label(N) value(...)` |
| `0x05` | `PT_INFO_END` | none |
| `0x06` | `PT_INFO_UPDATE` | `id(1) type(1) value(...)` |
| `0x07` | `PT_INFO_SCAN_STATUS` | BLE scan: `state(1) count(1)` |
| `0x08` | `PT_INFO_SCAN_ITEM` | BLE entry: `idx(1) rssi_s8(1) flags(1) name_len(1) name(N) addr(17)` |
| `0x09` | `PT_INFO_WIFI_SCAN_STATUS` | WiFi scan: `state(1) count(1)` |
| `0x0A` | `PT_INFO_WIFI_SCAN_ITEM` | WiFi entry: `idx(1) rssi_s8(1) ssid_len(1) ssid(N)` |

### 5.2 Lua -> ESP32

| Type | Name | Payload |
|---|---|---|
| `0x10` | `PT_INFO_REQUEST` | none |
| `0x11` | `PT_INFO_HEARTBEAT` | none |
| `0x12` | `PT_INFO_BLE_SCAN` | none |
| `0x13` | `PT_INFO_BLE_CONNECT` | `idx(1)` |
| `0x14` | `PT_INFO_BLE_DISCONNECT` | none |
| `0x15` | `PT_INFO_BLE_FORGET` | none |
| `0x16` | `PT_INFO_BLE_RECONNECT` | none |
| `0x17` | `PT_INFO_WIFI_SCAN` | none |

### 5.3 `PT_INFO_STATUS` bitfield

Current firmware status bits:

- bit0 (`0x01`): BLE connected
- bit1 (`0x02`): WiFi active
  - AP/Telemetry-AP: active
  - STA: active only when STA is connected
- bit2 (`0x04`): BLE connecting

### 5.4 Current info IDs

| ID | Name | Type | Description |
|---|---|---|---|
| `0x01` | `FIRMWARE` | STRING | build timestamp |
| `0x02` | `BT_ADDR` | STRING | local BLE MAC address; falls back to the last persisted address (`g_config.localBtAddr`) while the BLE stack is still initializing |
| `0x03` | `REM_ADDR` | STRING | saved remote address or `(none)` |
| `0x04` | `WIFI_IP` | STRING | active WiFi IP with mode suffix: `"IP (STATIC)"` (AP), `"IP (DHCP)"` (STA connected), or `"(none)"` |

### 5.5 RX frame size guard

The firmware RX buffer is 80 bytes (`s_rxBuf[80]`), sized to accommodate the largest possible incoming payload: `PT_PREF_SET` with a 63-character `STA_PASS` (header 3 + type 1 + max value 63 = 67 bytes including CRC).

If a received `LEN` field would require more bytes than the buffer can hold, the frame is dropped immediately with a warning log and the parser resets. Lua should never send oversized payloads; all current command types are well under this limit.

---

## 6) `CH_TRANS` (transparent transport)

| Type | Name | Payload |
|---|---|---|
| `0x01` | `PT_TRANS_SBUS` | raw SBUS bytes |
| `0x02` | `PT_TRANS_SPORT` | `physId(1) primId(1) dataId(2 LE) value(4 LE)` |
| `0x03` | `PT_TRANS_FRSKY` | raw FrSky bytes |

Current Lua runtime forwarding (`btwfs.lua`) uses `PT_TRANS_SPORT` for telemetry forwarding to firmware.

---

## 7) Typical runtime flows

### 7.1 Boot-time sync

Lua tools start sequence:

1. send `PT_INFO_REQUEST`
2. send `PT_PREF_REQUEST` (optional redundancy)
3. retry requests until pref/info state is marked ready

Firmware responds with pref list (`BEGIN/ITEM*/END`), info list (`BEGIN/ITEM*/END`), plus status.

### 7.2 Keepalive and ownership

- Foreground tools send `PT_INFO_HEARTBEAT` periodically.
- Firmware updates tools activity timestamp when valid command/info traffic arrives.
- Background function script (`btwfs.lua`) yields serial ownership while tools heartbeat is active.

### 7.3 BLE operation flow

- scan: Lua sends `PT_INFO_BLE_SCAN` -> firmware emits scan status/items/final status
- connect: Lua sends `PT_INFO_BLE_CONNECT(idx)` from selected result row
- disconnect/forget/reconnect: explicit command types with status-driven completion

### 7.4 WiFi scan flow

- Lua sends `PT_INFO_WIFI_SCAN`
- Valid only when WiFi operation mode allows scan (`AP` or `STA` path active)
- Firmware emits WiFi scan status/items/final state

---

## 8) Preference side effects and restart semantics

High-level behavior implemented in firmware (`handlePrefSet` path):

- `WIFI_MODE`: validates enum, may cascade telemetry output constraints, then restart path
- `DEV_MODE`: may cascade telemetry output constraints, then restart path
- `TELEM_OUT`: restart path depending on selected mode
- `MIRROR_BAUD`: restart path
- `MAP_MODE`: immediate persist + update, no restart
- `BT_NAME`: persist + advertise update, no restart
- `AP_SSID`/`AP_PASS`/`UDP_PORT`: persist, validated, then restart path
- `STA_SSID`: persist, no immediate restart
- `STA_PASS`: persist; restart can be conditional in STA mode

Whenever cascaded values are changed by firmware, they should be reflected back through `PT_PREF_UPDATE`.

---

## 9) Timing constants (observed defaults)

| Constant | Typical value | Meaning |
|---|---|---|
| channel push interval | 10ms | ~100Hz channel updates |
| status interval | 500ms | status push cadence |
| full periodic resync | 30000ms | pref/info refresh burst |
| tools idle timeout | 15000ms | suppress heavy resync when tools inactive |

---

## 10) Decoder/encoder implementation notes

For maintainers extending protocol:

- Add constants in both firmware header and Lua `serial_proto.lua` in the same commit.
- Keep payload shape deterministic and length-bound.
- Prefer additive extensions (new TYPE values) over mutating existing payloads.
- If a write operation can fail, always return explicit ACK result.
- For UI-sensitive async operations (BLE/WiFi scans), emit both status and result item streams.

---

## 11) Backward compatibility policy

Current integration target is the modular `BTWFS` Lua stack.

If protocol changes are introduced:

1. keep existing TYPE payloads stable where possible,
2. add new TYPEs for new behavior,
3. update firmware + Lua parser + docs together,
4. validate that startup sync and ACK paths remain deterministic.