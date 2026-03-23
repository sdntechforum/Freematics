# Project documentation (PROJECT.md)

This file describes the Freematics firmware repository for contributors and AI assistants (historically also referenced as CLAUDE.md-style guidance).

## Build System

This project uses **PlatformIO** with the Arduino framework targeting ESP32 (Freematics ONE+ hardware).

```bash
# Build a project (run from the project subdirectory)
pio run -d telelogger
pio run -d datalogger

# Build and upload to device
pio run -d telelogger --target upload

# Monitor serial output
pio device monitor -d telelogger --baud 115200

# Build, upload, and monitor in one command
pio run -d telelogger --target upload && pio device monitor --baud 115200
```

All projects share libraries from `../../libraries` (relative to each project directory), configured via `lib_extra_dirs` in `platformio.ini`.

## Project Structure

Two primary firmware projects:

- **`firmware_v5/telelogger/`** (or `telelogger/` under the v5 tree) — Real-time telemetry with cloud transmission. Uses a larger flash partition (`huge_app.csv`). This is the more feature-complete project.
- **`firmware_v5/datalogger/`** — Local storage with HTTP REST API access. No cloud transmission.

Utility/test projects: `can_sniffer/`, `j1939_monitor/`, `simple_obd_test/`, `simple_gps_test/`, `sim5360test/`, `sim7600test/`, `mpu9250test/`, `uart_forward/`.

## Architecture

### Data Flow

```
[OBD-II / GPS / MEMS sensors]
        ↓
[CBufferManager] — circular buffer slots (IRAM or PSRAM)
        ↓
[Storage: SDLogger / SPIFFSLogger / CStorageRAM]
        ↓  (telelogger only)
[TeleClient: UDP or HTTP → WiFi or Cellular]
```

### Key Subsystems

**Data Collection** (`telelogger.ino` / `datalogger.ino`):

- `processOBD()` — reads standard OBD-II PIDs from vehicle
- `processMEMS()` — reads 9-axis motion sensor, computes quaternion orientation
- `processGPS()` — updates geolocation from GNSS receiver
- Adaptive sampling: faster when vehicle is moving, slower when stationary

**Buffer Management** (`telestore.h` / `telestore.cpp`):

- `CBufferManager` manages circular buffer slots (32–1024 depending on PSRAM availability)
- Each slot holds one data snapshot; slots cycle through EMPTY → FILLING → FILLED → EMPTY
- Enables offline-first operation: data is buffered in RAM during network outages

**Storage** (`telestore.h`, `datalogger.h`):

- `FileLogger` base class → `SDLogger` (MicroSD) and `SPIFFSLogger` (internal flash)
- `CStorageRAM` serializes buffer for network transmission
- CSV format for log files; binary PID-tagged format in buffers
- SPIFFS auto-purges oldest files when space is low; SD rotates files by size

**Network Transmission** (`teleclient.h` / `teleclient.cpp`):

- `TeleClientUDP` — Freematics Hub / Traccar-oriented protocol
- `TeleClientHTTP` — OsmAnd-oriented protocol
- Each client has WiFi and cellular variants; WiFi is preferred with automatic fallback
- Reconnection handled automatically on failure

**HTTP Server** (`dataserver.cpp`, datalogger only):

- REST endpoints: `/api/info`, `/api/live`, `/api/list`, `/api/log/[id]`, `/api/data/[id]?pid=XX`, `/api/delete/[id]`

### PID System (firmware)

All data is tagged with numeric PIDs in code. OBD-II PIDs use the usual mode-01 style; custom PIDs are defined in the shared libraries:

- `PID_GPS_*` for geolocation data
- `PID_ACC_*` for accelerometer/gyro/magnetometer axes
- `PID_BATTERY_VOLTAGE`, `PID_DEVICE_TEMP`, `PID_CSQ` for device health

For the **on-the-wire text layout** to Freematics Hub, see **Freematics Packed Data Format** below.

### Configuration

Each project has a `config.h` with feature flags:

- `ENABLE_OBD`, `GNSS` / `USE_GNSS`, `ENABLE_MEMS` — enable/disable subsystems
- `STORAGE` — SD, SPIFFS, or none
- Cellular modem selection and APN
- `SERVER_PROTOCOL` — UDP vs HTTP(S)
- Server host, port, paths

The telelogger also supports `Kconfig.projbuild` for ESP-IDF menuconfig-style configuration.

## Hardware Targets

- **Freematics ONE+** — ESP32-based OBD-II dongle
- Model variants affect available hardware: Model B adds CAN bus, Model H adds J1939 support
- CPU runs at 160 MHz; QIO flash mode

---

## Freematics Hub — role and data path

**Freematics Hub** is a telemetry server that accepts data from remote devices, caches it in memory, persists to disk, and exposes it via HTTP/REST. Devices and consumers can run at different rates without losing samples when the link is intermittent.

Devices send **UDP datagrams** and/or **HTTP** transactions. The Hub API is documented here:

- [Freematics Hub API](https://freematics.com/pages/hub/api/)

### Session lifecycle (device → Hub)

Typical sequence:

1. **Login** — Tell the Hub a feeding session is starting (event notification).
2. **Send data** — Repeat telemetry payloads (packed PID/value format).
3. **Logout** — End the session (e.g. trip over).
4. **Ping** — Periodic keepalive so the Hub knows the device is still online.

---

## UDP payloads — events and data

### Event notification (login, logout, ping, etc.)

Text form:

```text
<DeviceID>#EV=<eventID>,SK=<serverKey>,TS=<deviceTimestamp>,VIN=<vehicleID>*<checksum>
```

- **`SK=`** — Server key; reserved; may be omitted in practice.
- **`TS=`** — Device time reference (commonly a millisecond counter from the device).
- **`VIN=`** — Vehicle identifier; used with login to associate a **feed**.
- **`*<checksum>`** — Two hex digits: 8-bit sum of all **ASCII bytes before the `*`**, modulo 256, written as hex (same rule used in `TeleClientUDP::verifyChecksum` in this repo).

Example login:

```text
ABCDEFG#EV=1,TS=39539,VIN=A1JC5444R7252367*XX
```

Firmware event IDs (`teleclient.h`) include: `EVENT_LOGIN` (1), `EVENT_LOGOUT` (2), `EVENT_SYNC` (3), `EVENT_RECONNECT` (4), `EVENT_COMMAND` (5), `EVENT_ACK` (6), `EVENT_PING` (7).

### Server reply (UDP)

After some event requests, the Hub may reply with a short datagram such as:

```text
1#EV=1,RX=<n>,TS=<timestamp>*<checksum>
```

Leading hex value is a **feed ID**; `RX` counts received items; `EV` echoes the event; checksum uses the same `*` scheme.

### Data transmission (UDP)

Telemetry wraps the **packed data format** with device id and checksum:

```text
<DeviceID>#<PID>:<value>,...*<checksum>
```

Example:

```text
ABCDEFG#0:68338,10D:79,30:1010,105:199,10C:4375,104:56,111:62,20:0;-1;95,10:6454200,A:-32.727482,B:150.150301,C:159,D:0,F:5,24:1250*7A
```

**Note:** Some published descriptions use `$` before the checksum; **this firmware uses `*`** before the two hex checksum digits.

---

## Freematics Packed Data Format

Text-based stream of **comma-separated** `PID:value` pairs sent to the Hub over **UDP** or **HTTP POST** (payload body).

- **PID** — Hex identifier in range `0` … `FFFF` (written without `0x`, e.g. `10D`, `A`, `24`).
- **Value** — Integer, float, or **multi-field** values separated by **`;`** (e.g. accelerometer `x;y;z`) or **`:`** where defined (e.g. gyro).

General pattern:

```text
<PID1>:<v1>,<PID2>:<v2>,<PID3>:<p1>;<p2>;<p3>,...
```

### PID 0 — record timestamp

**PID `0`** is reserved: value is a **32-bit millisecond** timestamp (usually the device’s `millis()`-style counter) for the following samples in that packet.

### Standard OBD-II Mode 01 PIDs (examples)

These appear as hex PIDs in the stream (often with mode bit set in the PID encoding used by the device; examples below match common Hub documentation):

| PID   | Meaning |
|-------|---------|
| `104` | Engine load |
| `105` | Engine coolant temperature |
| `10a` | Fuel pressure |
| `10b` | Intake manifold absolute pressure |
| `10c` | Engine RPM |
| `10d` | Vehicle speed |
| `10e` | Timing advance |
| `10f` | Intake air temperature |
| `110` | MAF air flow rate |
| `111` | Throttle position |
| `11f` | Run time since engine start |
| `121` | Distance traveled with MIL on |
| `12f` | Fuel level input |
| `131` | Distance traveled since codes cleared |
| `133` | Barometric pressure |
| `142` | Control module voltage |
| `143` | Absolute load value |
| `15b` | Hybrid battery pack remaining life |
| `15c` | Engine oil temperature |
| `15e` | Engine fuel rate |

### Custom PIDs (single-byte / mode 0 style, `0`–`FF`)

Used for GNSS, MEMS, and device telemetry:

| PID  | Meaning |
|------|---------|
| `10` | UTC time (`HHMMSSmm`) |
| `11` | UTC date (`DDMMYY`) |
| `A`  | Latitude |
| `B`  | Longitude |
| `C`  | Altitude (m) |
| `D`  | Speed (km/h) |
| `E`  | Course (degrees) |
| `F`  | Satellites in use |
| `20` | Accelerometer (`x;y;z`) |
| `21` | Gyroscope (`x:y:z`) |
| `22` | Magnetometer (`x/y/z`) |
| `23` | MEMS temperature (0.1 °C) |
| `24` | Battery voltage (0.01 V) |
| `25` | Orientation (yaw/pitch/roll) |
| `81` | Cellular RSSI (dB) |
| `82` | CPU / device temperature (0.1 °C) |
| `83` | CPU hall sensor |

### Worked example (Hub log line)

Raw:

```text
UCFLFX2V#0:1237313,24:1167,20:0.02;0.03;0.06,10:20561763,A:35.617649,B:-78.850929,C:0,D:0.1,E:0,F:0,12:0,82:45*F0
```

| Field | Interpretation |
|-------|----------------|
| Device ID | `UCFLFX2V` |
| `0` | Timestamp / tick `1237313` |
| `24` | Battery (0.01 V) → 11.67 V |
| `20` | Accel `0.02;0.03;0.06` |
| `10` | GPS time field |
| `A`, `B` | Latitude, longitude |
| `C`, `D`, `E`, `F` | Altitude, speed, course, sats |
| `12` | Additional field as emitted by device |
| `82` | Temperature (0.1 °C) → 4.5 °C |
| `*F0` | Checksum |

---

## Traccar server logs (Freematics protocol) — what to expect

This section summarizes **observed** `tracker-server.log` behavior when Traccar uses the **Freematics** protocol decoder (UDP). It helps correlate **server-side** traces with **device** serial output and Hub documentation. Log excerpts here follow patterns seen in production-style logs (e.g. devices `UCFLFX2V`, cellular clients in `172.58.x.x` / `172.59.x.x` US carrier NAT space).

### Log line shape

Typical forms:

```text
2024-09-20 13:50:05  INFO: [Uc99d8184: freematics < 172.58.251.206] 5543464c46583256...
2024-09-20 13:50:05  INFO: [Uc99d8184: freematics > 172.58.251.206] 312345563d312c52583d312c...
2024-09-20 13:50:06  INFO: [Uc99d8184] id: UCFLFX2V, time: 2024-09-20 13:50:04, lat: 35.65516, lon: -78.83470, speed: 0.2, course: 0.0
2024-09-16 00:05:56  INFO: Event id: UCFLFX2V, time: 2024-09-16 00:05:56, type: deviceUnknown, notifications: 0
```

- **`[Uc99d8184: freematics < ip]`** — Bytes **received from** the device (inbound UDP payload). The trailing blob is often **hex-encoded ASCII** of the **entire** Freematics frame (same text you would see on device serial after `[DAT]` / in Hub docs), not binary PID structs.
- **`[... freematics > ip]`** — Bytes **sent to** the device (Hub/decoder reply), also commonly logged as **hex**.
- **`[Uc99d8184]`** (short form) — Decoder has turned a **position packet** into a normalized fix: device **unique id** (`UCFLFX2V`), **time**, **lat/lon**, **speed**, **course**, etc.
- **`Event id: … type: deviceOnline | deviceUnknown`** — Traccar **connection state**, not part of the Freematics payload itself.

### Decoding hex payloads to ASCII

Example (ping / keepalive):

| Hex (truncated) | Decodes to |
|-----------------|------------|
| `5543…2a3535` | `UCFLFX2V#EV=7,TS=8497494,ID=UCFLFX2V*55` |
| `3123…2a3334` | `1#EV=7,RX=1,TS=8497494*34` |

**Python one-liner** for any full hex string from the log:

```bash
python3 -c "import sys; print(bytes.fromhex(sys.argv[1]).decode('ascii', errors='replace'))" PASTE_HEX_HERE
```

This matches firmware **event IDs** in `teleclient.h`: **`EV=1`** login, **`EV=2`** logout, **`EV=7`** ping, etc. The server reply **`1#EV=…`** uses a leading **feed/session id** (`1` in hex in the example) plus **`RX=`** receive counter and echoed **`TS=`**, with the same **`*` + hex checksum** convention.

### Login + telemetry sequence (example)

1. **Login** — Inbound hex decodes to something like `UCFLFX2V#EV=1,TS=…,VIN=…*xx` (or `ID=UCFLFX2V` in variants); server may reply with `…#EV=1,RX=…,TS=…*xx`; Traccar emits **`deviceOnline`**.
2. **Telemetry** — Repeated inbound hex decodes to **`UCFLFX2V#0:…,24:…,20:…,10:…,A:…,B:…,…*xx`** (packed PID format above). Traccar logs human-readable **`id: UCFLFX2V, lat, lon, speed, course`** lines.
3. **Ping-only periods** — When the vehicle stack sends **no position packets** for a while, logs may show only **`EV=7`** ping exchanges; Traccar may later mark **`deviceUnknown`** if its **timeout** fires without a fresh position, even if pings still arrive (depends on Traccar version, decoder, and timeout settings).

### `deviceUnknown` ~10 minutes apart (observed pattern)

In rotated logs, **`deviceUnknown`** often appears on a **~10 minute** cadence relative to **`deviceOnline`**, when the device is on **cellular** and mostly sending **pings** rather than continuous position streams. That aligns with **standby / low-motion** behavior on the device (fewer or no full GPS frames in UDP) combined with Traccar’s **“no position update”** semantics—not necessarily a full UDP outage. Tuning **protocol timeout** / **minimum period** on the server, or firmware **ping** / **position** strategy, changes this symptom.

### What this does *not* show

- Traccar logs here do **not** expose raw **OBD PID** names—only the **decoded position** summary and the **hex** Freematics text if you decode it.
- **Downlink** to the device is limited to what the Freematics decoder sends in the **`>`** lines (typically event replies / sync); there is no general “command API” in these logs.

### Firmware ↔ server correlation tips

- Compare **device serial** `[DAT] UCFLFX2V#…` lines to **server** inbound hex after ASCII decode; they should match modulo timing and any logging truncation.
- **`81:`** in the decoded string is **cellular RSSI (dBm)**; absence vs presence tracks Wi‑Fi vs LTE paths on dual-stack firmware.
- If **server** shows **ping (`EV=7`)** but **no** `lat`/`lon` lines, the device is likely not sending **GPS-rich** packets in that interval (standby, no fix, or config).

---

## GNSS — why “1980” or odd dates appear without a fix

GPS time is defined from an **epoch at 1980-01-06** UTC. Receivers and stacks often report default or raw week/time values when **no valid latitude/longitude** is available yet, which can surface as **1980-era** or otherwise invalid calendar dates until a proper fix. This is expected behavior when searching for satellites or indoors, not a Hub-specific bug.

For background:

- [GPS time / epoch (Wikipedia and receiver docs)](https://en.wikipedia.org/wiki/GPS_time)
- [Dilution of precision (DOP)](https://en.wikipedia.org/wiki/Dilution_of_precision_(navigation)) — geometry of satellites affects horizontal/vertical/time accuracy (HDOP, VDOP, PDOP, etc.).

---

## Firmware vs Hub document quirks

- Checksum delimiter on the wire in **`telelogger` UDP** is **`*HH`**, not `$`.
- PID letters `A`–`F` in ASCII payloads are **hex digits** for those custom IDs, not decimal 10–15 in the printed stream (they appear as the characters `A`, `B`, …).
- Exact PID encoding for OBD mode vs raw PID may vary slightly by firmware build; when in doubt, compare against serial `[DAT]` lines and the Hub API above.

---

## Signed SMS remote reboot (telelogger) — design, code changes, and operations

This section documents **HMAC-signed SMS** support that lets a registered cellular modem (e.g. SIM7070G on Freematics ONE+) accept a **single authenticated text** and trigger a controlled **reboot**. It also covers **BLE provisioning** fixes and **developer IDE** helpers added in the same effort.

### Feature summary

- **Transport:** Normal **mobile SMS** to the SIM in the device (not BLE, not UDP). The modem is polled periodically; unread SMS matching the signed format is verified and then deleted.
- **Command:** Currently only **`REBOOT`** is implemented.
- **Security model:** 32-byte **HMAC-SHA256** secret in NVS; per-SMS **monotonic counter** (replay protection); optional **allow-listed sender** string (exact match to modem-reported originator).
- **Device identity in MAC:** The signed string includes the same **8-character device ID** as UDP packets (from `genDeviceID()` / efuse), e.g. `ABCD1234` — **not** the IMEI.

### Code architecture

| Component | Role |
|-----------|------|
| `firmware_v5/telelogger/sms_command.h` | Feature gate `ENABLE_SMS_COMMANDS`, NVS key names, `smsCommandPoll()` declaration. |
| `firmware_v5/telelogger/sms_command.cpp` | Poll interval, read oldest unread SMS (`CellSIMCOM::smsReadOldestUnread`), parse body, verify HMAC (mbedtls), counter, optional sender, NVS update, `esp_restart()`. |
| `firmware_v5/telelogger/telelogger.ino` | Calls `smsCommandPoll()` from the main cellular/work loop when `STATE_CELL_CONNECTED`. BLE command handlers provision NVS: `SMS?`, `SMSKEY=`, raw 64-hex / 32-byte key, `SMSEN=`, `SMSFROM=`, `SMSCTR=`, `SMSCLR`. |
| `firmware_v5/telelogger/config.h` | `ENABLE_SMS_COMMANDS`, `SMS_POLL_INTERVAL_MS` (default **120000** ms). |
| `libraries/FreematicsPlus/FreematicsNetwork.*` | `CellSIMCOM` SMS helpers: text mode, `AT+CMGL="REC UNREAD"`, delete by index. |
| `libraries/FreematicsPlus/utility/ble_spp_server.*` | **Provisioning transport:** larger GATT command/status characteristics, **MTU negotiation**, **length-prefixed** queued payloads (fixes `strlen` on binary keys), correct **free** of command blocks, safer queue-full handling. |

### SMS body format (what you send from a phone)

Single line, **four space-separated fields** (parsed with `sscanf`):

```text
FM1 REBOOT <decimal_counter> <16_hex_chars>
```

**Do not** send the pipe form `FM1|REBOOT|n|DEVID` as the SMS text. Pipes are used only **inside** the HMAC message (see below).

### HMAC message string (firmware-internal)

```text
FM1|REBOOT|<counter>|<devid>
```

- `<counter>`: decimal, must be **strictly greater** than the last stored counter in NVS (`SMS_NVS_CTR`).
- `<devid>`: **exactly 8 characters** from `genDeviceID()` (same prefix as UDP lines `DEVID#0:…`).

**MAC in SMS:** First **8 bytes** of `HMAC-SHA256(key, utf8_message)`, encoded as **16 lowercase/uppercase hex digits** (constant-time compare on device).

### NVS keys (namespace `storage`)

| Key | Purpose |
|-----|---------|
| `sms_hmac` (blob, 32 bytes) | HMAC secret |
| `sms_en` (u8) | `1` = SMS commands enabled |
| `sms_ctr` (u32) | Last accepted counter |
| `sms_from` (string) | If non-empty, **exact** match required against modem-reported sender |

### Sender allow-list (`SMSFROM=`) — important

Matching uses **`strcmp`** against the **sender string the modem puts in `+CMGL`** (second quoted field). Carriers differ:

- If **`[SMS] denied (sender)`** appears, the stored string does not match what the network reports.
- Prefer **E.164 with `+`** (e.g. `SMSFROM=+15551234567`) if that is how the modem presents the number.
- Clear filter: `SMSFROM=-` or `SMSFROM=CLR`.

### Code changes (this enhancement — by file)

**`firmware_v5/telelogger/telelogger.ino`**

- BLE: **`SMS?` / `SMSKEY=` / `SMSEN=` / `SMSFROM=` / `SMSCTR=` / `SMSCLR`** (behind `ENABLE_BLE` + `ENABLE_SMS_COMMANDS`).
- Accept **32-byte raw** key on BLE (length from `ble_recv_payload_len()`), for nRF Connect **Hex** writes.
- Accept **64 ASCII hex** characters only (no `SMSKEY=` prefix) for the Freematics mobile app field style.
- Strip **`\n`** as well as **`\r`** before parsing text commands.
- Clearer errors: e.g. `ERR_NOT_HEX_64`, `ERR_UUID_NOT_CMD`, `ERR_UNK L=… NEED_32B_OR_64HEX`, `ERR_EMPTY`.
- `SMSKEY=` path unchanged for tools that send the full prefixed line.

**`libraries/FreematicsPlus/utility/ble_spp_server.h`**

- `SPP_CMD_MAX_LEN` / `SPP_STATUS_MAX_LEN` increased (**128**) so long lines like `SMSKEY=<64 hex>` fit the GATT attribute max length.

**`libraries/FreematicsPlus/utility/ble_spp_server.c`**

- On connect: **`esp_ble_gatt_set_local_mtu(517)`** so one ATT write can carry long ASCII lines after the phone exchanges MTU.
- Command writes: allocate **`sizeof(size_t) + len + 1`**, store **length prefix**, queue pointer; **`ble_recv_payload_len()`** returns that length.
- **`ble_send_response`**: `free` the **block base** (`payload - sizeof(size_t)`), not the inner pointer only.
- Queue full: **free** the dropped oldest block before replacing.
- GAP spam: **`ESP_LOGE` → `ESP_LOGD`** for generic GAP events.

**`firmware_v5/telelogger/patch_ino_compile_db.py`** (optional local tool)

- After `pio run -t compiledb`, duplicates `compile_commands.json` entry for `telelogger.ino` (PlatformIO lists `telelogger.ino.cpp`). Improves VS Code / Cursor IntelliSense for the sketch file.

**`firmware_v5/telelogger/.gitignore`**

- Ignore **`compile_commands.json`** (large, machine-generated).

**`.vscode/`** (optional; useful for monorepo checkout)

- `c_cpp_properties.json`: `compileCommands` + cross **`compilerPath`** for ESP32-C3 / Xtensa toolchains.
- `tasks.json`: chained **compiledb + patch** task.

### Counter and MAC — you cannot reuse the same MAC

- **`SMS?`** returns **`CTR:<n>`** where `<n>` is the **last accepted** counter.
- The **next** SMS must use **counter `n+1`** (or any integer **> n** within firmware limits).
- The **16 hex MAC depends on the counter** (and devid). **Each** SMS needs a **fresh** MAC computed for `FM1|REBOOT|<new_counter>|<devid>`.

### Operational guide — nRF Connect, serial, Python, SMS

#### 1. Prerequisites

- **`ENABLE_BLE 1`** in `config.h` for BLE provisioning (can be disabled after NVS is set, if desired).
- **`ENABLE_SMS_COMMANDS 1`** (default in tree).
- Cellular **registered**; SIM supports **SMS**.
- Know the device’s **8-character ID** (UDP line prefix or boot `DEVICE ID:`).

#### 2. BLE: write the secret key (characteristic `FFE1`)

1. Connect with **nRF Connect** (or Freematics app).
2. **Request high MTU** (e.g. 247–517).
3. On service **`ABF0`**, characteristic **`FFE1`** (command):
   - **Option A — Hex (32 bytes):** write **32 raw bytes** (the decoded HMAC key). Firmware shows `[BLE] <32-byte key> -> OK`.
   - **Option B — Text (64 hex chars):** write ASCII only, e.g. `a1b2…` (64 nibbles). Must be exactly **64** hex digits.
4. **Do not** type `0xFFE1` as the payload — that UUID only identifies **which** characteristic to write.

#### 3. BLE: enable SMS and optional sender filter

- `SMSEN=1` → `OK`
- `SMSFROM=+1XXXXXXXXXX` (or exact string your modem reports) → `OK`
- `SMSFROM=-` → clear allow-list (any sender, if policy allows)

#### 4. Read counter and flags

- `SMS?` → `EN:x KEY:x CTR:n F:…`
  - **`KEY:1`** means a 32-byte secret is stored.
  - **`CTR:n`** — next SMS counter must be **> n**.

#### 5. Python: build the SMS body

Replace key hex, devid, and **counter = last_CTR + 1**:

```python
import hmac, hashlib

KEY_HEX = "YOUR_64_CHAR_HEX_SECRET"  # same secret as stored in device (32 bytes)
DEVID = "ABCD1234"                   # 8 chars from device / UDP prefix
ctr = 2                              # must be > SMS? CTR

key = bytes.fromhex(KEY_HEX)
msg = f"FM1|REBOOT|{ctr}|{DEVID}".encode("ascii")
mac16 = hmac.new(key, msg, hashlib.sha256).hexdigest()[:16]
print("Sign:", msg.decode())
print("SMS:  FM1 REBOOT", ctr, mac16)
```

Send the printed **`SMS:`** line as the **entire SMS body** to the **SIM’s phone number**.

#### 6. Timing and logs

- Default poll: **`SMS_POLL_INTERVAL_MS`** (120 s) — wait up to ~2 minutes after sending.
- Success serial line: **`[SMS] verified REBOOT`** then ROM boot banner.
- Failures: **`[SMS] ignored (parse|format|counter|replay|mac fmt)`**, **`[SMS] denied (sender)`**, **`[SMS] auth fail (lockout 1h)`** (bad MAC).

### Security and operations notes

- **Treat the 32-byte key as a secret.** If exposed (chat, screenshot, repo), **rotate**: `SMSCLR` or erase NVS, provision a **new** key, update your Python/script.
- **SMS is not encrypted** on the air; HMAC proves possession of the key and binds counter + devid — it does not hide the command from the carrier.
- **Lockout:** repeated **wrong MAC** triggers a **1-hour** lockout (`sms_command.cpp`).

---

## Contributing this enhancement upstream (merge readiness)

You can open a **pull request** to the community repository when the change set is **clean, reviewable, and safe to share**. Use this checklist:

1. **Split concerns (recommended)**  
   - **PR A — Feature:** `telelogger` SMS + BLE provisioning + `FreematicsPlus` BLE GATT fixes (core product value).  
   - **PR B — Optional:** `.vscode/`, `patch_ino_compile_db.py`, `compile_commands` gitignore — some maintainers prefer **not** merging editor-specific files; offer them separately or document in PR description.

2. **Do not commit secrets**  
   - No real **HMAC keys**, phone numbers, or server credentials in code or docs. Use placeholders in examples.

3. **Build proof**  
   - `pio run` from **`firmware_v5/telelogger`** for the **board/env you target** (ESP32 vs ESP32-C3, etc.). Mention any **`platformio.ini`** / `config.h` requirements (`ENABLE_BLE`, PSRAM flags).

4. **Changelog / commit message**  
   - Summarize: signed SMS reboot, NVS provisioning over BLE, SIMCOM SMS read path, BLE MTU + command buffer length + length-prefixed BLE queue.

5. **Upstream process**  
   - Follow the **host repo’s** `CONTRIBUTING.md` (if any): branch naming, issue link, CLA.

6. **Housekeeping**  
   - Omit **`.DS_Store`** and local artifacts from the PR. Keep diffs minimal outside the feature.

With the checklist satisfied, the SMS + BLE provisioning work is **reasonable to propose** as an enhancement; final acceptance depends on upstream maintainers’ scope and style preferences.
