# thesada-fw

Modular ESP32 firmware for property monitoring nodes. Built from scratch in C++17 on the Arduino framework using PlatformIO. Runs on multiple board targets - from LILYGO S3 with cellular fallback, to WROOM-32 with OLED, to CYD touch displays, to Ethernet nodes. WiFi, Ethernet, or LTE-M - the node stays connected and publishing.

**Currently deployed:** monitoring an outdoor wood boiler (temperature, pump current, Telegram alerts) and indoor climate (SHT31). Running 24/7 in the field.

Full documentation: [thesada.io/firmware/fw-index](https://thesada.io/firmware/fw-index)

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
- SSD1306 OLED display (128x64, I2C) - Lua-driven rendering, remote MQTT data [ENABLE_DISPLAY]

**Data**
- SD card CSV logging with per-boot files and configurable log rotation (SD handling fully in SDModule)
- MQTT publish queue with ring buffer and minimum send interval
- MQTT CLI: full shell access over MQTT (`cli/#` - topic is command, payload is args, response on `cli/response`)
- MQTT remote config: set single key (`cli/config.set`) or push full config (`cli/file.write` + `cli/config.reload`)
- Home Assistant MQTT auto-discovery (per-sensor topics, availability via LWT, WiFi diagnostics)
- MQTT file upload (`cli/file.write` - push scripts and config files remotely)
- Lua 5.3 scripting engine - hot-reloadable event rules without recompiling
- Lua bindings: MQTT.subscribe, JSON.decode, Display.*, EventBus, Config, Node, Telegram

**Web interface**
- Live sensor dashboard (temperature, current, battery %) - no login required for read-only data
- Admin panel (auth-gated): config editor, OTA upload, file browser, WebSocket terminal
- LiteServer SSID dropdown with scan results and signal strength (+ manual entry fallback)
- WebSocket terminal streams live firmware logs and accepts shell commands (replays last 50 lines on connect)
- REST API: `POST /api/cmd` runs any shell command with JSON response

**Shell CLI**
- 30+ commands across serial, WebSocket, HTTP, and MQTT - same handler, zero duplication
- Commands: filesystem, config, network diagnostics, Lua exec, OTA trigger, selftest, battery, sensors, module status

**OTA**
- Push: upload `.bin` via web dashboard or curl
- Pull: JSON manifest with SHA256 verification, periodic check + MQTT trigger

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
┌─ Optional modules (ENABLE_*) ───────────────────────┐
│  ┌─────────────┐ ┌───────────┐ ┌──────────────────┐ │
│  │ Temperature │ │   SHT31   │ │     ADS1115      │ │
│  └─────────────┘ └───────────┘ └──────────────────┘ │
│  ┌─────────────┐ ┌──────────┐ ┌──────────────────┐  │
│  │   Battery   │ │    SD    │ │   PowerManager   │  │
│  └─────────────┘ └──────────┘ └──────────────────┘  │
│  ┌─────────────┐ ┌──────────┐ ┌───────────────────┐ │
│  │ HttpServer  │ │LiteServer│ │   ScriptEngine    │ │
│  └─────────────┘ └──────────┘ └───────────────────┘ │
│  ┌─────────────┐ ┌──────────┐ ┌───────────────────┐ │
│  │  Cellular   │ │ Ethernet │ │    Telegram       │ │
│  └─────────────┘ └──────────┘ └───────────────────┘ │
│  ┌─────────────┐ ┌────────────┐                     │
│  │   Display   │ │ TftDisplay │                     │
│  └─────────────┘ └────────────┘                     │
├─ Core (always compiled) ────────────────────────────┤
│  ┌─────────────┐ ┌──────────┐ ┌───────────────────┐ │
│  │ WiFiManager │ │   MQTT   │ │    OTAUpdate      │ │
│  │  + NTP      │ │  Client  │ │    (pull only*)   │ │
│  └─────────────┘ └──────────┘ └───────────────────┘ │
│  ┌─────────────┐ ┌──────────┐ ┌───────────────────┐ │
│  │    Shell    │ │ EventBus │ │   SleepManager    │ │
│  │  30+ cmds   │ │          │ │                   │ │
│  └─────────────┘ └──────────┘ └───────────────────┘ │
│  ┌──────────────┐                                   │
│  │ModuleRegistry│                                   │
│  └──────────────┘                                   │
└─────────────────────────────────────────────────────┘
* Push OTA (/ota upload) requires ENABLE_HTTPSERVER
```

Modules self-register via `MODULE_REGISTER(Class, Priority)` at the bottom of each .cpp file - main.cpp has zero module includes and just calls `ModuleRegistry::beginAll()` / `loopAll()`. Priorities control init order: POWER(10), NETWORK(20), SERVICE(30), SCRIPT(40), SENSOR(50), OUTPUT(60). Modules communicate via EventBus - never direct calls. Lua bindings are also self-registering: modules call `ScriptEngine::addBindings()` in their `begin()`, so ScriptEngine has no module includes either.

Config is split: `thesada_config.h` for compile-time module enables, `config.json` on LittleFS for all runtime values.

**Core (always compiled):** Config, EventBus, Log, Shell, ModuleRegistry, WiFiManager, MQTTClient, OTAUpdate, SleepManager, HeartbeatLED

**Optional modules (ENABLE_* guards):** Temperature, SHT31, ADS1115, Battery, PMU, SD, Cellular, Ethernet, Telegram, HttpServer, LiteServer, ScriptEngine, Display (OLED), TftDisplay (CYD), PWM, PowerManager

Minimal build (core only) saves ~313 KB flash. Full build with all modules: 1.4 MB. Release includes both `firmware.bin` (full) and `firmware_minimal.bin` (core only).

---

## Hardware

| Board | PIO environment | Notes |
|---|---|---|
| LILYGO T-SIM7080-S3 | `esp32-owb` | Primary target - all modules |
| ESP32-S3 bare devkit | `esp32-s3-debug` | USB CDC serial, SHT31 enabled (BOARD_S3_BARE) |
| ESP32-WROOM-32 | `esp32-wroom` | No cellular/PMU/SD - OLED display, WiFi, MQTT |
| CYD (ESP32-2432S028R) | `esp32-cyd` | 2.8" TFT touch, LiteServer, SD (SPI) |
| WT32-ETH01 | `esp32-eth` | LAN8720A Ethernet, WiFi fallback, well pump node |

Board-specific module overrides are in `thesada_config.h` (e.g. `BOARD_ETH` enables Ethernet and disables cellular/PMU).

---

## Quick start

See [`base/`](base/) for build instructions, config reference, and shell command list.

```bash
cd base
python3 scripts/check_deps.py        # verify PlatformIO + libraries
cp examples/config.json.example data/config.json
# edit data/config.json (WiFi, MQTT, sensor pins)
# optionally copy example scripts:
#   cp examples/scripts/rules.lua.example data/scripts/rules.lua
#   cp examples/scripts/display.lua.example data/scripts/display.lua  (OLED display)
pio run -e esp32-owb --target upload      # or esp32-wroom for WROOM-32
pio run -e esp32-owb --target uploadfs
```

**Releasing:** `./release.sh` builds full + minimal binaries, tags, pushes, and creates a GitHub release with auto-generated changelog.

---

## Structure

```
thesada-fw/
  base/
    src/
      main.cpp                  # entry point - no module includes
      thesada_config.h          # compile-time ENABLE_* flags, pin maps
    lib/
      thesada-core/src/         # Config, EventBus, Log, Shell, ModuleRegistry, ...
      thesada-mod-temperature/  # each module is a PlatformIO library
      thesada-mod-ads1115/
      thesada-mod-battery/
      thesada-mod-cellular/
      thesada-mod-display/
      thesada-mod-httpserver/
      thesada-mod-powermanager/
      thesada-mod-pwm/
      thesada-mod-scriptengine/
      thesada-mod-sd/
      thesada-mod-telegram/
      thesada-mod-tftdisplay/
    scripts/
      add_framework_libs.py     # PlatformIO framework lib discovery
      ota_upload.py             # push OTA to a device over HTTP
    examples/                   # config.json.example and Lua script examples
  examples/                     # minimal reference projects
  tests/                        # hardware-in-the-loop test suite (test_firmware.py)
  release.sh                    # one-command build, tag, push, GitHub release
```

Each `thesada-mod-*` directory is a standalone PlatformIO library with its own `library.json` (`libCompatMode: off`). All includes use angle brackets (`#include <Log.h>`) - no relative paths. Modules that wrap a static class (PowerManager, HttpServer, ScriptEngine) have thin `*Module` wrappers that handle the MODULE_REGISTER glue.

---

## Licence

SPDX-License-Identifier: GPL-3.0-only
