# BTWifiSerial Lua Serial Protocol (v2)

Technical protocol reference for developers integrating with BTWifiSerial over UART.

Scope:
- Firmware: `src/lua_serial.cpp`, `src/lua_serial.h`
- Lua tooling: `lua/SCRIPTS/TOOLS/BTWFS/lib/serial_proto.lua`

This document describes the **current framed protocol** used by firmware and Lua scripts.

---

## 1) Physical layer

| Parameter | Value |
|---|---|
| Transport | UART1 |
| Baud | 115200 |
| Format | 8N1 |
| Flow control | None |
| ESP32 TX | GPIO21 |
| ESP32 RX | GPIO20 |

EdgeTX AUX must be configured as **LUA @ 115200**.

---

## 2) Frame envelope

All traffic is framed with a shared envelope:

```text
[SYNC=0xAA][CH][TYPE][LEN][PAYLOAD...LEN bytes][CRC]
```

- `CRC = XOR(CH, TYPE, LEN, PAYLOAD[0..LEN-1])`
- `SYNC` is **not** included in CRC
- `LEN` is 0..255

Parser behavior:
- CRC mismatch => frame dropped
- Unknown `(CH,TYPE)` => ignored after CRC pass

---

## 3) Logical channels

| CH | Name | Direction |
|---|---|---|
| `0x01` | `CH_PREF` | bidirectional |
| `0x02` | `CH_INFO` | bidirectional |
| `0x03` | `CH_TRANS` | bidirectional |

---

## 4) CH_PREF (preferences/config)

### 4.1 ESP32 -> Lua types

| Type | Name | Payload |
|---|---|---|
| `0x01` | `PT_PREF_BEGIN` | `count(1)` |
| `0x02` | `PT_PREF_ITEM` | `id(1) type(1) flags(1) label_len(1) label(N) type_data(...)` |
| `0x03` | `PT_PREF_END` | none |
| `0x04` | `PT_PREF_UPDATE` | `id(1) type(1) value(...)` |
| `0x05` | `PT_PREF_ACK` | `id(1) result(1)` |

`PT_PREF_ACK.result`:
- `0x00` => success
- `0x01` => error

### 4.2 Lua -> ESP32 types

| Type | Name | Payload |
|---|---|---|
| `0x10` | `PT_PREF_REQUEST` | none |
| `0x11` | `PT_PREF_SET` | `id(1) type(1) value(...)` |

### 4.3 Field types (`FT_*`)

| Code | Name | Encoding |
|---|---|---|
| `0` | `FT_ENUM` | full item: `opt_count(1) cur_idx(1) [opt_len(1) opt_str(N)]*` |
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

### 4.5 Preference IDs (current)

| ID | Name | Type | Notes |
|---|---|---|---|
| `0x01` | `WIFI_MODE` | ENUM | `Off` / `AP` / `STA` |
| `0x02` | `DEV_MODE` | ENUM | `Trainer IN` / `Trainer OUT` / `Telemetry` |
| `0x03` | `TELEM_OUT` | ENUM | `WiFi UDP` / `BLE` / `Off` |
| `0x04` | `MIRROR_BAUD` | ENUM | `57600` / `115200` |
| `0x05` | `MAP_MODE` | ENUM | `GV` / `TR` |
| `0x06` | `BT_NAME` | STRING | max 15 |
| `0x07` | `AP_SSID` | STRING | max 15 |
| `0x08` | `UDP_PORT` | STRING | max 5, validated in firmware as 1024..65535 |
| `0x09` | `AP_PASS` | STRING | max 15, min 8 |
| `0x0A` | `STA_SSID` | STRING | max 31 |
| `0x0B` | `STA_PASS` | STRING | max 63 |

---

## 5) CH_INFO (status/channels/scan/info)

### 5.1 ESP32 -> Lua types

| Type | Name | Payload |
|---|---|---|
| `0x01` | `PT_INFO_CHANNELS` | 8x int16 BE (16 bytes) |
| `0x02` | `PT_INFO_STATUS` | `status(1)` |
| `0x03` | `PT_INFO_BEGIN` | `count(1)` |
| `0x04` | `PT_INFO_ITEM` | `id(1) type(1) label_len(1) label(N) value(...)` |
| `0x05` | `PT_INFO_END` | none |
| `0x06` | `PT_INFO_UPDATE` | `id(1) type(1) value(...)` |
| `0x07` | `PT_INFO_SCAN_STATUS` | BLE scan: `state(1) count(1)` |
| `0x08` | `PT_INFO_SCAN_ITEM` | BLE entry: `idx(1) rssi_s8(1) flags(1) name_len(1) name(N) addr(17)` |
| `0x09` | `PT_INFO_WIFI_SCAN_STATUS` | WiFi scan: `state(1) count(1)` |
| `0x0A` | `PT_INFO_WIFI_SCAN_ITEM` | WiFi entry: `idx(1) rssi_s8(1) ssid_len(1) ssid(N)` |

### 5.2 Lua -> ESP32 types

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

- bit0 (`0x01`) => BLE connected
- bit1 (`0x02`) => WiFi active
  - AP/Telemetry-AP mode: always active
  - STA mode: active only when STA is connected
- bit2 (`0x04`) => BLE connecting

### 5.4 Info IDs

| ID | Name | Type | Description |
|---|---|---|---|
| `0x01` | `FIRMWARE` | STRING | build timestamp (`DDMMYYYY HHMM`) |
| `0x02` | `BT_ADDR` | STRING | local BLE address |
| `0x03` | `REM_ADDR` | STRING | saved remote address or `(none)` |

---

## 6) CH_TRANS (transparent payload)

| Type | Name | Payload |
|---|---|---|
| `0x01` | `PT_TRANS_SBUS` | raw SBUS bytes |
| `0x02` | `PT_TRANS_SPORT` | `physId(1) primId(1) dataId(2 LE) value(4 LE)` |
| `0x03` | `PT_TRANS_FRSKY` | raw FrSky bytes |

Current Lua tooling uses `PT_TRANS_SPORT` from `btwfs.lua` to forward radio telemetry to firmware.

---

## 7) Runtime flows

### 7.1 Initial sync flow

Lua tools startup sequence:

1. Send `PT_INFO_REQUEST`
2. Send `PT_PREF_REQUEST` (optional redundancy)
3. Retry every ~2s until prefs become ready

Firmware response:

- full pref list (`PREF_BEGIN`, `PREF_ITEM*`, `PREF_END`)
- full info list (`INFO_BEGIN`, `INFO_ITEM*`, `INFO_END`)
- status frame

### 7.2 Heartbeat / ownership

- Tools script sends `PT_INFO_HEARTBEAT` every ~50ms when foreground is active.
- Firmware updates `s_lastToolsCmdMs` on valid frames.
- Function script (`btwfs.lua`) checks SHM heartbeat to yield serial ownership while tools are active.

### 7.3 BLE scan/connect flow

- Lua sends `PT_INFO_BLE_SCAN`.
- Firmware emits `PT_INFO_SCAN_STATUS` (`scanning`), then `PT_INFO_SCAN_ITEM*`, then final status `done`.
- Lua connect command uses scan index via `PT_INFO_BLE_CONNECT`.

### 7.4 WiFi scan flow

- Lua sends `PT_INFO_WIFI_SCAN`.
- Valid only when firmware WiFi mode is active (`apMode != 0`).
- Firmware emits `PT_INFO_WIFI_SCAN_STATUS`, then `PT_INFO_WIFI_SCAN_ITEM*`, then done/fail status.

---

## 8) Pref side effects and restart semantics

Important implementation behavior in firmware (`handlePrefSet`):

- `WIFI_MODE`:
  - validates enum index 0..2
  - can cascade `TELEM_OUT` to `Off` for incompatible combinations
  - ACK then invokes mode setter (persists + restart)

- `DEV_MODE`:
  - trainer modes can force `TELEM_OUT = Off`
  - emits `PREF_UPDATE` for cascaded changes before restart

- `TELEM_OUT`, `MIRROR_BAUD`:
  - ACK then apply via main setters (restart path)

- `MAP_MODE`:
  - persisted immediately
  - ACK + `PREF_UPDATE`
  - no restart

- `BT_NAME`:
  - persisted + advertising update
  - ACK + `PREF_UPDATE`
  - no restart

- `AP_SSID`, `AP_PASS`, `UDP_PORT`:
  - persisted
  - ACK then restart (`UDP_PORT` validated 1024..65535)

- `STA_SSID`:
  - persisted + ACK
  - no immediate restart

- `STA_PASS`:
  - persisted + ACK
  - restart only if STA SSID exists and WiFi mode is STA

---

## 9) Timing constants (firmware)

| Constant | Value | Meaning |
|---|---|---|
| channel frame interval | 10ms | ~100Hz channel updates |
| status interval | 500ms | status updates |
| periodic full resync | 30000ms | pref+info refresh burst |
| tools idle timeout | 15000ms | suppress heavy resync when tools idle |

---

## 10) Developer notes

- Source of truth for IDs/types/constants is `src/lua_serial.h`.
- Lua tooling constants should stay aligned with `lib/serial_proto.lua`.
- Keep protocol backward compatibility only if you explicitly support legacy script `lua/SCRIPTS/TOOLS/BTWifiSerial.lua`.
- For new integrations, target the v2 channel/type/len protocol described here.
