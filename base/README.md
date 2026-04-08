# thesada-fw base

Custom firmware for LILYGO T-SIM7080-S3 (ESP32-S3). WiFi + LTE-M/NB-IoT fallback, TLS MQTT, DS18B20 temperature sensing, ADS1115 RMS current sensing, SD card logging, Lua scripting, and full shell CLI over serial, WebSocket, HTTP, and MQTT.

---

## Quick Start

```bash
# 1. Copy and fill in config
cp data/config.json.example data/config.json
# edit data/config.json with your credentials

# 2. Add TLS CA certificate (ISRG Root X1 for Let's Encrypt brokers and GitHub OTA)
curl -s https://letsencrypt.org/certs/isrgrootx1.pem -o data/ca.crt

# 3. Edit config.h to enable/disable modules
# 4. Compile
pio run -e esp32-s3-dev

# 5. Flash firmware + filesystem
pio run -e esp32-s3-dev --target upload
pio run -e esp32-s3-dev --target uploadfs

# 6. Subsequent updates via web OTA at http://[device-ip]/  (Admin → OTA)
```

> **Note:** PlatformIO 6.13.0 requires `intelhex` in the PlatformIO Python environment:
> ```bash
> ~/.local/pipx/venvs/platformio/bin/python -m pip install intelhex
> # or if using ~/.platformio/penv:
> ~/.platformio/penv/bin/pip install intelhex
> ```

---

## CA Certificate

No certificate is hardcoded. Place your CA cert PEM as `data/ca.crt` before uploading the filesystem. Both WiFi MQTT and the cellular modem load it at boot.

**For Let's Encrypt brokers (and GitHub OTA):**
```bash
curl -s https://letsencrypt.org/certs/isrgrootx1.pem -o data/ca.crt
```

If `/ca.crt` is absent, TLS connects without verification and a `[WRN]` is logged.

---

## config.json fields

| Section | Key | Description |
|---|---|---|
| `device` | `name` | MQTT client ID, hostname |
| `device` | `friendly_name` | Shown in web footer |
| `device` | `heartbeat_s` | Blue LED pulse interval in seconds; `-1` = disabled, min 5 |
| `web` | `user` / `password` | Dashboard and API login |
| `wifi` | `networks` | List of `{ssid, password}` - tries strongest RSSI first |
| `wifi` | `timeout_per_ssid_s` | Seconds per SSID attempt (default 10) |
| `wifi` | `wifi_check_interval_s` | Cellular → WiFi recheck interval (default 900 s) |
| `wifi` | `static_ip` | Static IP (empty = DHCP) |
| `wifi` | `gateway` / `subnet` / `dns` | Required when `static_ip` is set |
| `ntp` | `server` | NTP server (default `pool.ntp.org`) |
| `ntp` | `tz_offset_s` | UTC offset in seconds (e.g. 3600 = CET) |
| `mqtt` | `broker` / `port` | Broker host + port (default 8883 TLS) |
| `mqtt` | `user` / `password` | Broker credentials |
| `mqtt` | `topic_prefix` | Base MQTT topic (default `thesada/node`) |
| `mqtt` | `send_interval_s` | Min seconds between published messages (0 = unlimited) |
| `ota` | `manifest_url` | URL to `firmware.json` manifest for pull OTA |
| `ota` | `check_interval_s` | Pull OTA check interval (default 21600 = 6 h) |
| `temperature` | `pin` | OneWire GPIO pin |
| `temperature` | `interval_s` | Read interval |
| `temperature` | `auto_discover` | Auto-detect and name sensors on first boot |
| `temperature` | `sensors` | List of `{address, name}` overrides |
| `ads1115` | `i2c_sda` / `i2c_scl` | I2C pins (default 1 / 2) |
| `ads1115` | `address` | I2C address (default 72 = 0x48) |
| `ads1115` | `interval_s` | Read interval |
| `ads1115` | `channels` | List of `{name, mux, gain}` |
| `cellular` | `apn` | SIM APN |
| `cellular` | `sim_pin` | SIM PIN (empty if none) |
| `cellular` | `rf_settle_ms` | LTE radio settle delay (default 15000) |
| `cellular` | `reg_timeout_ms` | Registration timeout (default 180000) |
| `sd` | `enabled` | Set `false` to disable SD logging |
| `sd` | `pin_clk` / `pin_cmd` / `pin_data` | SD_MMC GPIO pins |
| `sd` | `max_file_kb` | Max log file size in KB before rotating to next file (0 = unlimited, default 1024) |
| `telegram` | `bot_token` | Telegram Bot API token (from @BotFather). Empty = skip direct Telegram send. |
| `telegram` | `chat_ids` | Array of Telegram chat ID strings (users and groups) |
| `webhook` | `url` | HTTP(S) POST URL for alerts (empty = disabled) |
| `webhook` | `message_template` | `{{value}}` is replaced with the alert text |

---

## Shell commands

All commands work identically over serial, WebSocket (`/ws/serial`), HTTP (`POST /api/cmd`), and MQTT (`<prefix>/cli/<command>`). Every command outputs at least one useful variable reading.

| Group | Commands |
|---|---|
| System | `help`, `version`, `restart`, `heap`, `uptime`, `sensors`, `battery`, `selftest` |
| Filesystem | `ls`, `cat`, `rm`, `write`, `mv`, `df` |
| Config | `config.get`, `config.set`, `config.save`, `config.reload`, `config.dump` |
| Network | `ifconfig`, `ping`, `ntp`, `mqtt` |
| Modules | `module.list`, `module.status` |
| Lua | `lua.exec`, `lua.load`, `lua.reload` |

Key commands: `sensors` lists all configured sensors with addresses + battery state. `module.status` shows runtime state per module (sensor counts, intervals, Telegram direct on/off, SD mount).

Paths prefixed with `/sd/` are routed to SD_MMC; all others use LittleFS.

> **VSCode PIO monitor on macOS**: serial input may not reach the device from the integrated terminal.
> Use `pio device monitor` from a real terminal instead.

---

## Web interface

| Route | Method | Auth | Description |
|---|---|---|---|
| `/` | GET | yes | Live sensor dashboard |
| `/api/info` | GET | no | Firmware version, build date, device name |
| `/api/state` | GET | no | Current sensor readings JSON |
| `/api/cmd` | POST | yes | Run any shell command: `{"cmd":"version"}` → `{"ok":true,"output":["..."]}` |
| `/api/config` | GET | yes | Read `config.json` |
| `/api/config` | POST | yes | Write `config.json` + restart (page auto-refreshes after 10s) |
| `/api/backup` | POST | yes | Copy `config.json` to SD card |
| `/api/restart` | POST | yes | Reboot device |
| `/api/files` | GET | yes | List SD or LittleFS root (`?source=sd\|littlefs`) |
| `/api/file` | GET | yes | Read file (`?path=...&source=...`) |
| `/api/file` | POST | yes | Write file |
| `/api/file` | DELETE | yes | Delete file |
| `/ota` | POST | yes | Push OTA firmware upload (page auto-refreshes after 10s) |
| `/ws/serial` | WS | no | Bidirectional terminal - log stream + all shell commands |

---

## OTA

### Push (web)

Upload a compiled `.bin` via Admin → OTA in the web dashboard, or:
```bash
curl -u admin:password -X POST http://[ip]/ota -F "firmware=@build/firmware.bin"
```

### Pull (manifest)

Set `ota.manifest_url` in config to a `firmware.json` URL. The device fetches it periodically and on `thesada/node/cmd/ota` MQTT message.

**Manifest format** (generated by `scripts/generate_manifest.py`):
```json
{
  "version": "1.x.y",
  "url":     "https://github.com/thesada/thesada-fw/releases/download/v1.x.y/firmware.bin",
  "sha256":  "abc123..."
}
```

**GitHub Releases** work out of the box if `data/ca.crt` is the ISRG Root X1 - same cert used for Let's Encrypt MQTT brokers.

---

## Lua scripting

Scripts live on LittleFS at `/scripts/main.lua` (runs once at boot) and `/scripts/rules.lua` (event rules, hot-reloadable via `lua.reload` or MQTT `cmd/lua/reload`).

```lua
-- rules.lua example
EventBus.subscribe("temperature", function(data)
  local s = data.sensors[1]
  if s.temp_c > 40 then
    Log.warn("Overheat: " .. s.name .. " " .. s.temp_c .. "°C")
    MQTT.publish("thesada/node/alert", s.name .. " overheating")
  end
end)
```

Available bindings: `Log.info/warn/error`, `MQTT.publish`, `EventBus.subscribe`, `Config.get`, `Node.restart/version/uptime`.

---

## Alert routing

```
Device → MQTT <prefix>/alert      → Home Assistant automation → Telegram
       → Telegram Bot API direct  (if bot_token + chat_ids configured)
       → HTTP webhook             (if webhook.url configured)
```

All three channels fire independently.

**Alert logic lives in Lua** (`/scripts/alerts.lua`). The firmware provides the bindings:
- `Telegram.send(chat_id, message)` - send to specific chat
- `Telegram.broadcast(message)` - send to all configured chat_ids
- `EventBus.subscribe("temperature", func)` - react to sensor events
- See `data/scripts/alerts.lua.example` for a complete example with sustain counters and cooldown timers.

---

## Dependency check

```bash
python scripts/check_deps.py
```

Queries PlatformIO registry and GitHub for newer versions. Exit 0 = up to date, exit 1 = updates available. Run periodically and review changelogs before upgrading.

---

## Dependencies

| Library | Version | Notes |
|---|---|---|
| ArduinoJson | 7.4.3 | |
| PubSubClient | 2.8 | |
| OneWire | 2.3.8 | |
| DallasTemperature | 4.0.6 | |
| TinyGSM | 0.12.0 | |
| XPowersLib | git | |
| Adafruit ADS1X15 | 2.6.2 | |
| ESPAsyncWebServer | git | |
| ESP-Arduino-Lua | git | GPL-3.0 |
| AsyncTCP | vendored | v3.3.2 + null-PCB crash fixes in `lib/AsyncTCP/` |
