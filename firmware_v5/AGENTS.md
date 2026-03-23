# AGENTS.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Project Overview

Freematics ONE+ firmware v5 — ESP32-based vehicle telemetry firmware for [Freematics ONE+](https://freematics.com/products/freematics-one-plus/) devices. Collects OBD-II, GPS, and MEMS sensor data from vehicles and either logs locally or transmits to remote servers. Written in C++ using the Arduino framework on ESP32 via PlatformIO.

## Build System

Each subdirectory is an independent PlatformIO project. Build from within the specific project directory:

```
# Build a specific project
pio run -d telelogger

# Upload firmware
pio run -d telelogger --target upload

# Serial monitor (115200 baud for all projects)
pio device monitor -d telelogger -b 115200
```

All projects reference shared libraries at `../../libraries` (relative to each project dir) via `lib_extra_dirs` in `platformio.ini`. The shared libraries live at `../libraries/` relative to the repo root and include:
- **FreematicsPlus** — Main hardware abstraction (OBD, GNSS, MEMS sensors, cellular/WiFi modules)
- **httpd** — Lightweight embedded HTTP server
- **FreematicsOLED** — OLED display driver
- **TinyGPS** — GPS NMEA parsing (used by simple_gps_test only)

The telelogger can alternatively be built with ESP-IDF using `Kconfig.projbuild` for `menuconfig` configuration.

## Repository Structure

The repo contains two main applications and several test/utility sketches:

**Main applications:**
- `telelogger/` — Full telemetry logger with remote data transmission (the primary application)
- `datalogger/` — Local-only data logger with WiFi HTTP API for data access

**Test/utility sketches:**
- `can_sniffer/`, `simple_obd_test/`, `simple_gps_test/`, `mpu9250test/`, `sim5360test/`, `sim7600test/`, `uart_forward/`, `j1939_monitor/`

## Architecture: telelogger (primary application)

### Dual-task design
- **Main task** (`loop()` → `process()`): Collects data from OBD, GPS, MEMS sensors. Fills `CBuffer` slots and manages standby/wakeup lifecycle.
- **Telemetry task** (`telemetry()`, runs on separate FreeRTOS task created via `subtask.create()`): Manages network connections (WiFi/cellular) and transmits buffered data to the remote server.

### Producer-consumer buffer system
`CBufferManager` manages a pool of `CBuffer` slots (defined in `teleclient.h/.cpp`). The main task fills buffers (`BUFFER_STATE_FILLING` → `BUFFER_STATE_FILLED`), and the telemetry task locks and consumes them (`BUFFER_STATE_LOCKED`). With PSRAM, up to 1024 slots buffer hours of data during network outages. Without PSRAM, 32 slots are available.

### State machine
Device state is tracked via bitflags in the `State` class (`STATE_OBD_READY`, `STATE_GPS_READY`, `STATE_MEMS_READY`, `STATE_NET_READY`, `STATE_WORKING`, `STATE_STANDBY`, etc.). The main loop transitions between `STATE_WORKING` (active data collection) and standby (triggered by stationary timeout or OBD errors). Wakeup is triggered by MEMS motion detection or battery voltage jump.

### Network layer
- `TeleClientUDP` — UDP client for Freematics Hub / Traccar protocol
- `TeleClientHTTP` — HTTP(S) client implementing OsmAnd protocol
- Both support seamless WiFi ↔ cellular failover: prefers WiFi when available, falls back to cellular (SIM5360/SIM7600/SIM7070)

### Storage layer
`CStorage` base class with two paths:
- `CStorageRAM` — Serializes data into a RAM buffer for network transmission (with checksum-appended packet format: `DEVID#key:value,key:value*CHECKSUM`)
- `FileLogger` → `SDLogger` / `SPIFFSLogger` — Writes CSV data to SD card or SPIFFS flash (files at `/DATA/<id>.CSV`)

### OBD PID polling
PIDs are organized in tiers in the `obdData[]` array. Tier 1 PIDs (speed, RPM, throttle, engine load) are polled every cycle. Higher tiers are polled round-robin, one per cycle, to balance data freshness with bus bandwidth.

## Compile-time Configuration

All features are toggled via `#define` in each project's `config.h`. Key defines for telelogger:

- `ENABLE_OBD` (0/1) — OBD-II connection
- `ENABLE_MEMS` (0/1) — Motion sensor
- `GNSS` (GNSS_NONE/GNSS_INTERNAL/GNSS_EXTERNAL/GNSS_CELLULAR)
- `STORAGE` (STORAGE_NONE/STORAGE_SPIFFS/STORAGE_SD)
- `SERVER_PROTOCOL` (PROTOCOL_UDP/PROTOCOL_HTTP/PROTOCOL_HTTPS)
- `SERVER_HOST`, `SERVER_PORT` — Remote server connection
- `ENABLE_WIFI`, `ENABLE_BLE`, `ENABLE_HTTPD` — Optional subsystems
- `ENABLE_SMS_COMMANDS` (0/1) — Signed SMS commands over cellular (see below)
- `SMS_POLL_INTERVAL_MS` — How often the telemetry task polls for SMS when cellular is up (default 120000)
- `BOARD_HAS_PSRAM` — Enables large buffer mode

Datalogger uses similar but slightly different naming (`USE_OBD`, `USE_GNSS`, `USE_MEMS`, `ENABLE_WIFI_STATION`, `ENABLE_WIFI_AP`).

## Key Patterns

- The telelogger and datalogger have independent, parallel implementations of BLE command handling (`processBLE()`), HTTP server (`dataserver.cpp`), and data logging classes — they are not shared code.
- MEMS sensor initialization tries multiple sensor types in sequence: ICM-42627 → ICM-20948 → MPU-9250, using the first one found.
- NVS (Non-Volatile Storage) is used in telelogger to persist runtime configuration changes (APN, WiFi credentials) made via BLE commands.
- The `config.xml` in telelogger defines a schema for the Freematics Builder GUI configuration tool.

## Secure SMS remote commands (telelogger)

Telelogger can accept **authenticated SMS** while the **cellular modem is active** (typically driving on LTE; not when the sketch has powered the modem off because Wi‑Fi is in use). This is for **break-glass actions** (currently **`REBOOT` only**) without relying on IP reachability to the dongle.

### Source layout

- `telelogger/sms_command.h`, `telelogger/sms_command.cpp` — HMAC verification, NVS, command dispatch
- `libraries/FreematicsPlus/FreematicsNetwork.{h,cpp}` — `CellSIMCOM::smsEnsureTextMode()`, `smsReadOldestUnread()`, `smsDeleteByIndex()` (SIM7600-style `AT+CMGL="REC UNREAD"`)

### When polling runs

`smsCommandPoll()` is called from the **telemetry task** after `teleClient.inbound()`, only if `STATE_CELL_CONNECTED` is set. There is **no** extra wake of the modem for SMS when you are at home on Wi‑Fi with cellular shut down (power-saving behavior unchanged).

### Security model

1. **32-byte secret** stored in NVS blob key `sms_hmac` (never commit secrets; provision over BLE).
2. **SMS body format** (single line, spaces as shown):

   `FM1 REBOOT <counter> <16-hex>`

   - `FM1` — protocol tag (version 1).
   - `REBOOT` — only supported verb in current firmware.
   - `<counter>` — decimal `uint32`, must be **strictly greater** than the last accepted value stored in NVS (`sms_ctr`). Prevents replay of an old SMS.
   - `<16-hex>` — first **8 bytes** of **HMAC-SHA256**(key, UTF-8 string `FM1|REBOOT|<counter>|<devid>`), encoded as **16 lowercase/uppercase hex digits** (firmware accepts either case).

3. **Enable flag** — NVS `sms_en` must be **1** or SMS handling is skipped even if a key exists.
4. **Optional sender allow-list** — NVS string `sms_from` (e.g. `+15551234567`). If empty, **any** sender is accepted (weaker; SMS caller-ID spoofing is a known industry issue). If set, the modem-reported originating address must match exactly.
5. **Brute-force throttling** — After an **HMAC failure**, firmware refuses further SMS processing for **1 hour** (RAM only; cleared by reboot). Malformed or non-matching messages are deleted without triggering that lockout.

### Generating `YOUR_64_HEX_NIBBLES` (the 32-byte key)

The BLE command `SMSKEY=` expects **exactly 64 hexadecimal characters** = **32 bytes** of random key material.

**Option A — OpenSSL (macOS / Linux)**

```bash
openssl rand -hex 32
```

Copy the single line of 64 hex characters (no spaces). That output **is** your `YOUR_64_HEX_NIBBLES` value for `SMSKEY=`.

**Option B — Python 3**

```bash
python3 -c "import secrets; print(secrets.token_hex(32))"
```

**Option C — Python one-liner to print key + first test SMS line** (replace `UCFLFR15` with your `DEVICE ID` from serial boot log, and `1` with your next counter after `SMS?` / NVS):

```python
import hmac, hashlib, secrets

key_hex = secrets.token_hex(32)   # store this securely; use as SMSKEY= on BLE
key = bytes.fromhex(key_hex)
devid = "UCFLFR15"
ctr = 1
msg = f"FM1|REBOOT|{ctr}|{devid}".encode()
mac8 = hmac.new(key, msg, hashlib.sha256).digest()[:8]
sms = f"FM1 REBOOT {ctr} {mac8.hex()}"
print("BLE: SMSKEY=" + key_hex)
print("SMS body:", sms)
```

Store the key in a **password manager or fleet secrets store**. If the key leaks, rotate: `SMSCLR` over BLE, generate a new key, `SMSKEY=...`, `SMSEN=1`, and reset counter coordination on the server.

### BLE provisioning commands (Freematics Controller / SPP)

Requires `ENABLE_BLE 1` in `config.h`. Commands are line-oriented (same style as existing `APN=`, `RESET`, etc.).

| Command | Meaning |
|--------|---------|
| `SMS?` | Reply: `EN`, `KEY` (1 if 32-byte blob present), `CTR` (last accepted counter), `F:` trusted sender or `-` |
| `SMSKEY=<64 hex>` | Store 32-byte HMAC key (does not enable SMS by itself) |
| `SMSEN=1` / `SMSEN=0` | Enable or disable SMS command processing |
| `SMSFROM=+15551234567` | Only accept SMS from this originating address (modem format) |
| `SMSFROM=-` or `SMSFROM=CLR` | Clear allow-list (any sender) |
| `SMSCTR=<n>` | Set last accepted counter in NVS (use after server/device skew; dangerous if set too high) |
| `SMSCLR` | Erase SMS key, counter, sender, and set `sms_en=0` |

Typical first-time sequence: `SMSKEY=<openssl output>` → `SMSEN=1` → optional `SMSFROM=...` → `SMS?` to confirm.

### How to test (recommended order)

1. **Build and flash** telelogger with `ENABLE_SMS_COMMANDS 1` (default in `config.h` unless overridden) and cellular enabled for your board.
2. **Serial monitor** at 115200: note **DEVICE ID** (e.g. `UCFLFR15`) for the HMAC string.
3. **Ensure cellular is actually used** for the test: either disable Wi‑Fi in `config.h` for the test build, or test while driving / in an area where the device uses LTE and `STATE_CELL_CONNECTED` is true. SMS will **not** be read if the modem is powered off (e.g. home Wi‑Fi with cell disabled by the sketch).
4. **BLE**: connect with the Freematics Controller app (or any SPP terminal), send `SMSKEY=<64 hex>` then `SMSEN=1`. Send `SMS?` and confirm `KEY:1 EN:1`. If you use `SMSFROM=`, send a test SMS from **that exact number** only.
5. **Compute SMS body** on a PC using the Python snippet above (correct `devid` and `ctr`; for first test use `ctr` **greater than** current `CTR` from `SMS?`, usually `1` if counter was never set).
6. **Send the SMS** from your phone or an SMS API (Twilio, etc.) to the **SIM phone number** of the dongle. Keep the body **one line**, no quotes.
7. **Watch serial**: within about **SMS_POLL_INTERVAL_MS** (default 2 minutes), you should see either `[SMS] verified REBOOT` and a reboot, or a rejection reason (`ignored (parse)`, `auth fail`, etc.). Temporarily lower `SMS_POLL_INTERVAL_MS` in `config.h` (e.g. 30000) for faster iteration during development.
8. **Second test**: increment `ctr` (e.g. 2), recompute HMAC, send again; sending the **same** `ctr` again should log replay ignored and **not** reboot.

### Server / fleet integration notes

- Maintain **per-device** next counter in your database; increment on each approved command.
- HMAC input is exactly: `FM1|REBOOT|<decimal_counter>|<devid>` with `devid` matching the 8-character ID from the device.
- Align counter after mistakes with BLE `SMSCTR=` or clear and re-key with `SMSCLR`.

### Header include note for agents

`sms_command.h` includes `../../libraries/FreematicsPlus/FreematicsNetwork.h` **by path** so the build does not pick `libraries/FreematicsONE/FreematicsNetwork.h`, which is a different API and does not define `CellSIMCOM`.
