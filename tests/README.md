# thesada-fw firmware test suite

Two layers:

- **Native unit tests** - pure logic (predicates, parsers, lookup tables)
  compiled and run on the host, no board. Fast, deterministic, gated in CI.
- **On-device serial tests** (`test_firmware.py`) - automated + manual
  checks driven over the device shell. Cover the integrated system.

## Native unit tests

Pure headers with no Arduino/IDF/NVS includes (`*_policy.h`, `*_payload.h`,
`*_keymap.h`) are tested off-board with Unity under the PlatformIO `native`
env. Suites live in `test/test_<name>/`.

```bash
pio test -e native                    # all suites
pio test -e native -f test_rollback   # one suite
scripts/static-check.sh               # cppcheck over the pure units (CI gate)
```

Current suites:

| Suite | Header under test | Covers |
|---|---|---|
| `test_rollback` | `mqtt_rollback_policy.h` | when a bad-mqtt-config reboot rolls back to last-good vs when it must not |
| `test_secret_keymap` | `secret_keymap.h` | logical field -> 15-char NVS key, per-SSID WiFi hashing, unknown-field / truncation rejection |
| `test_cli_payload` | `cli_payload.h` | CLI envelope + chunked-payload parsing |

To add one: create `test/test_<name>/test_<name>.cpp`, `#include` the pure
header, write `TEST_ASSERT_*` cases, and wire a `main()` calling
`UNITY_BEGIN()` / `RUN_TEST` / `UNITY_END()` (copy an existing suite). It
joins `pio test -e native` automatically. See
[CODE-GUIDELINES.md - Native unit harness](../CODE-GUIDELINES.md).

## On-device serial tests

Automated + manual/assisted tests over the device serial shell.

### Setup

```bash
pip install pyserial
```

### Usage

```bash
# Auto-detect port, run all tests (will prompt for manual steps)
python tests/test_firmware.py

# Explicit port
python tests/test_firmware.py --port /dev/cu.usbmodem1101

# Skip slow or hardware-dependent manual groups
python tests/test_firmware.py --skip ota,cellular,ads1115

# Automated only (skip all manual groups)
python tests/test_firmware.py --skip sensors,ads1115,mqtt,ota,websocket,sd,cellular
```

### Test groups

| # | Group | Type | Notes |
|---|-------|------|-------|
| 1 | System | auto | version, heap, uptime, selftest |
| 2 | Filesystem | auto | ls, cat, write/cat/rm cycle, df |
| 3 | Config | auto | config.get, config.dump |
| 3b | Secrets | auto | secret.set/info/clear round-trip, config.dump stays clean, resolution order |
| 4 | Network | auto | ifconfig, ping (DNS), ntp, mqtt |
| 5 | Shell | auto | help, module.list |
| 5b | Module gating | auto | module.status activation vs config[key].enabled |
| 6 | Lua | auto | return value, OK, error handling, reload |
| 7 | Sensors | manual | confirm temp + ADS1115 readings on MQTT/dashboard |
| 8 | ADS1115 load | manual | connect physical load, wait 70 s, confirm non-zero |
| 9 | MQTT publish | manual | Lua MQTT.publish → confirm receipt on broker |
| 10 | OTA trigger | manual | publish to cmd/ota, confirm log activity |
| 11 | WebSocket | manual | open browser terminal, try version + help |
| 12 | SD log | manual | check log file exists and content looks correct |
| 13 | Cellular | manual | disconnect WiFi, restart, confirm cellular attach |

### Output

```
[PASS]  automated check passed
[FAIL]  automated check failed
[WARN]  something is off but not fatal
[MAN ]  manual step confirmed by user
[SKIP]  group skipped via --skip
```

Exit code 0 if all automated checks pass, 1 if any fail.
Manual ([MAN]) and warned ([WARN]) results do not affect exit code.

### Port discovery

Ports are detected in order:
1. ESP32-S3 native USB (VID 0x303A)
2. CP210x / CH340 / FTDI adapters
3. `/dev/cu.usbmodem*`, `/dev/ttyACM*`, `/dev/ttyUSB*` name patterns
4. Interactive selection if multiple candidates found
