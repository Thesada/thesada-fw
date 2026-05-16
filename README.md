# thesada-fw

Modular ESP32 firmware for property monitoring nodes. Built from scratch in C++17 on the Arduino framework using PlatformIO. Targets the ESP32-S3 family - primary board is the LILYGO T-SIM7080G-S3 with WiFi + LTE-M/NB-IoT fallback. The node stays connected and publishing whether the access point is up or the cell tower is the only thing reachable.

**Currently deployed:** monitoring an outdoor wood boiler (temperature, pump current, Telegram alerts) and indoor climate (SHT31). Running 24/7 in the field.

Full documentation: [thesada.io/firmware](https://thesada.io/firmware/)

---

## Features

**Connectivity**
- WiFi multi-SSID with RSSI ranking, configurable retries per SSID (wifi.retries, default 2), and automatic LTE-M/NB-IoT fallback (SIM7080G)
- Fallback AP with captive portal when no WiFi network in range (configurable password + timeout)
- TLS MQTT over both WiFi (PubSubClient) and cellular (modem-native AT commands)
- MQTT connection watchdog (10 min timeout, force reconnect on half-open TCP sockets)
- TCP keepalive on MQTT socket (30s idle / 10s interval / 3 probes - detects NAT timeouts)
- CA cert loaded from filesystem - no hardcoded certificates
- TLS heap guard - skips cert upgrade when free heap < 40KB (prevents OOM on constrained boards)
- NTP sync with ISO 8601 timestamps in all log output

**Sensors & power**
- DS18B20 temperature (OneWire, multi-sensor, auto-discovery, retry on disconnect, last-known-value fallback)
- SHT31 temperature + humidity (I2C, raw driver, no external library) [ENABLE_SHT31]
- ADS1115 RMS current sensing (30 samples over 2x 60Hz cycles, outputs amps + watts for SCT-013-030)
- Configurable temperature unit (C/F) - reflected in MQTT, dashboard, and HA discovery
- AXP2101 PMU management (battery voltage/percent/charge state, solar charging, VBUS limits) [ENABLE_PMU]
- Heartbeat LED: AXP2101 CHGLED or GPIO pin (active-low supported via negative pin number)
- Battery monitoring with configurable low-battery alerts [ENABLE_BATTERY]
- Configurable charge current (0-1000mA) and cutoff voltage (4.0-4.4V)
- Deep sleep with RTC memory persistence (boot count, OTA check time)

**Data**
- SD card CSV logging with per-boot files and configurable log rotation (SD handling fully in SDModule)
- MQTT publish queue with ring buffer and minimum send interval
- MQTT CLI: full shell access over MQTT (`cli/#` - topic is command, payload is args, response on `cli/response`)
- MQTT remote config: set single key (`cli/config.set`) or push full config (`cli/fs.write` + `cli/config.reload`)
- MQTT file ops: `fs.write` (truncate), `fs.append`, `fs.cat` with chunked reads (offset/length for large files)
- Home Assistant MQTT auto-discovery (per-sensor topics, availability via LWT, WiFi diagnostics)
- Lua 5.3 scripting engine - hot-reloadable event rules without recompiling
- Lua bindings: MQTT.subscribe, JSON.decode, EventBus, Config, Node, Telegram

**Web interface**
- Live sensor dashboard (temperature, current, battery %) - no login required for read-only data
- Admin panel (auth-gated): config editor, OTA upload, file browser, WebSocket terminal
- LiteServer SSID dropdown with scan results and signal strength (+ manual entry fallback)
- WebSocket terminal streams live firmware logs and accepts shell commands (replays last 50 lines on connect)
- REST API: `POST /api/cmd` runs any shell command with JSON response

**Shell CLI**
- 35+ commands across serial, WebSocket, HTTP, and MQTT - same handler, zero duplication
- Commands: filesystem, config, network diagnostics, Lua exec, OTA trigger, selftest, battery, sensors, module status
- Debug CLI: `ota.status`, `boot.info`, `partitions`, `chip.info`, `sdkconfig`, `net.mqtt` (subscription table + RX ring of recent topics) for remote-debug sessions without serial access

**OTA**
- Push: upload `.bin` via web dashboard or curl
- Pull: JSON manifest with SHA256 verification, periodic check + MQTT trigger
- Watchdog-safe download loop: `yield()` + `esp_task_wdt_reset()` after every chunk + short socket/handshake timeouts keep fetchManifest and applyUpdate from stalling past the task watchdog on flaky wireless links
- `ota.check [--force] [url]` shell command: accessible over MQTT CLI (`<prefix>/cli/ota.check`), serial, WebSocket, HTTP. `--force` bypasses version check for dev iteration and stuck-device recovery. Deferred execution so the CLI response publishes before the reboot
- PSRAM routing: when `BOARD_HAS_PSRAM` is set, large allocations (CA cert, heap-hungry buffers) go to external PSRAM to keep the internal heap free for TLS context allocation

**Telemetry**
- Heap + PSRAM telemetry: free / min free / max alloc block / psram free published to MQTT every 5 min with HA auto-discovery for each metric
- Config + script SHA256 hashes in device info for drift detection
- Every Telegram alert tagged with `[heap=N]` for post-mortem correlation

**Security**
- Bearer token auth: `POST /api/login` returns a 1-hour token (max 4 concurrent, auto-evict oldest)
- HTTP Basic Auth fallback on all admin endpoints (backwards compatible with curl/scripts)
- Per-IP rate limiting (5 failed logins - 30 s lockout)
- WebSocket auth via pre-granted IP tokens (prevents unauthenticated shell access)
- Sensor dashboard and `/api/state` public (read-only); all admin endpoints auth-gated

**Alerting (Lua-driven)**
- Alert logic lives in Lua scripts (hot-reloadable, no recompile)
- Telegram.send(chat_id, msg) and Telegram.broadcast(msg) Lua bindings
- Sustain counters (alert after N readings OR T minutes, whichever is less)
- Cooldown timers (prevent repeat alerts)
- Node.setTimeout(ms, fn) for delayed actions (e.g. boot alerts after WiFi ready)
- Three output channels: MQTT, direct Telegram Bot API (multi-recipient), HTTP webhook
- HTTPS requests capped at 10s to prevent MQTT keepalive starvation

---

## Architecture

```
+-- Optional modules (ENABLE_*) -------------------------+
|  Temperature  SHT31  ADS1115  Battery  SD  PowerManager |
|  HttpServer  LiteServer  ScriptEngine  Cellular         |
|  Telegram  PWM                                          |
+---------------------------------------------------------+
|  Core (always compiled)                                 |
|  WiFiManager + NTP  MQTTClient  OTAUpdate               |
|  Shell (35+ cmds)   EventBus    SleepManager            |
|  ModuleRegistry     Config      Log  HeartbeatLED       |
+---------------------------------------------------------+
```

Modules self-register via `MODULE_REGISTER(Class, Priority)` at the bottom of each .cpp file - main.cpp has zero module includes and just calls `ModuleRegistry::beginAll()` / `loopAll()`. Priorities control init order: POWER(10), NETWORK(20), SERVICE(30), SCRIPT(40), SENSOR(50), OUTPUT(60). Modules communicate via EventBus - never direct calls. Lua bindings are also self-registering: modules call `ScriptEngine::addBindings()` in their `begin()`, so ScriptEngine has no module includes either.

Config is split: `thesada_config.h` for compile-time module enables, `config.json` on LittleFS for all runtime values.

**Core (always compiled):** Config, EventBus, Log, Shell, ModuleRegistry, WiFiManager, MQTTClient, OTAUpdate, SleepManager, HeartbeatLED

**Optional modules (ENABLE_* guards):** Temperature, SHT31, ADS1115, Battery, PMU, SD, Cellular, Telegram, HttpServer, LiteServer, ScriptEngine, PWM, PowerManager

Minimal build (core only) saves ~313 KB flash. Full build with all modules: 1.4 MB. Release includes both `firmware.bin` (full) and `firmware_minimal.bin` (core only).

---

## Hardware

| Board | PIO environment | Notes |
|---|---|---|
| LILYGO T-SIM7080G-S3 | `esp32-owb` | Primary target - all modules, PSRAM, cellular |
| LILYGO T-SIM7080G-S3 | `esp32-owb-debug` | Same hardware, verbose logging + DEBUG_AT_COMMANDS |
| LILYGO T-SIM7080G-S3 | `esp32-owb-rescue` | Stripped rescue build (~1070 KB) for remote recovery |
| ESP32-S3 bare devkit | `esp32-s3-debug` | USB CDC serial, SHT31 enabled (`BOARD_S3_BARE`) |
| ESP32-S3 bare devkit | `esp32-s3-debug-rescue` | Rescue twin for lab validation |

Rescue builds strip all optional modules except PMU via `BOARD_OWB_RESCUE` - used for OTA recovery on weak links where the full binary fails mid-download. Bare-S3 builds (`BOARD_S3_BARE`) drop the LILYGO-specific hardware (cellular, PMU, battery, SD) and switch the default sensor to SHT31 for desk testing.

---

## Quick start

```bash
python3 scripts/check_deps.py        # verify PlatformIO + libraries
cp examples/config.json.example data/config.json
# edit data/config.json (WiFi, MQTT, sensor pins)
# optionally copy example scripts:
#   cp examples/scripts/rules.lua.example data/scripts/rules.lua
pio run -e esp32-owb --target upload      # or esp32-s3-debug for bare-S3
pio run -e esp32-owb --target uploadfs
```

---

## Structure

```
thesada-fw/
  src/
    main.cpp                  # entry point - no module includes
    thesada_config.h          # compile-time ENABLE_* flags, pin maps
  lib/
    thesada-core/src/         # Config, EventBus, Log, Shell, ModuleRegistry, ...
    thesada-mod-temperature/  # each module is a PlatformIO library
    thesada-mod-ads1115/
    thesada-mod-battery/
    thesada-mod-cellular/
    thesada-mod-httpserver/
    thesada-mod-powermanager/
    thesada-mod-pwm/
    thesada-mod-scriptengine/
    thesada-mod-sd/
    thesada-mod-telegram/
  scripts/
    add_framework_libs.py     # PlatformIO framework lib discovery
    ota_upload.py             # push OTA to a device over HTTP
    deploy-ota.sh             # deploy to self-hosted OTA server (gitignored)
  examples/                   # config.json.example and Lua script examples
  tests/                      # hardware-in-the-loop test suite (test_firmware.py)
  data/                       # LittleFS filesystem (config.json, scripts/, ca.crt)
  platformio.ini              # board environments + library deps
```

Each `thesada-mod-*` directory is a standalone PlatformIO library with its own `library.json` (`libCompatMode: off`). All includes use angle brackets (`#include <Log.h>`) - no relative paths. Modules that wrap a static class (PowerManager, HttpServer, ScriptEngine) have thin `*Module` wrappers that handle the MODULE_REGISTER glue.

---

## Development

Install the local git hooks once after clone:

```bash
./scripts/hooks/install.sh
```

This symlinks `scripts/hooks/pre-commit` into `.git/hooks/`. The hook
refuses a commit that touches load-bearing source files (OTA, MQTT,
Shell, Config, HttpServer, Cellular) without an update to
[docs/invariants.md](docs/invariants.md). The same check runs server
side in CI (`.github/workflows/ci.yml` job `invariant-ledger`).

Bypass when the touch genuinely does not establish or rely on an
invariant:

```bash
INVARIANT_OK=1 git commit ...                     # env-set, no audit trail
git commit -m "... \n\nINVARIANT_OK: 1"           # trailer, greppable in git log
```

See [CODE-GUIDELINES.md](CODE-GUIDELINES.md) "Ledger discipline" for
what counts as establishing an invariant.

---

## Licence

SPDX-License-Identifier: GPL-3.0-only
