# thesada-fw firmware test suite

Automated + manual/assisted tests over the device serial shell.

## Setup

```bash
pip install pyserial
```

## Usage

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

## Test groups

| # | Group | Type | Notes |
|---|-------|------|-------|
| 1 | System | auto | version, heap, uptime, selftest |
| 2 | Filesystem | auto | ls, cat, write/cat/rm cycle, df |
| 3 | Config | auto | config.get, config.dump |
| 4 | Network | auto | ifconfig, ping (DNS), ntp, mqtt |
| 5 | Shell | auto | help, module.list |
| 6 | Lua | auto | return value, OK, error handling, reload |
| 7 | Sensors | manual | confirm temp + ADS1115 readings on MQTT/dashboard |
| 8 | ADS1115 load | manual | connect physical load, wait 70 s, confirm non-zero |
| 9 | MQTT publish | manual | Lua MQTT.publish → confirm receipt on broker |
| 10 | OTA trigger | manual | publish to cmd/ota, confirm log activity |
| 11 | WebSocket | manual | open browser terminal, try version + help |
| 12 | SD log | manual | check log file exists and content looks correct |
| 13 | Cellular | manual | disconnect WiFi, restart, confirm cellular attach |

## Output

```
[PASS]  automated check passed
[FAIL]  automated check failed
[WARN]  something is off but not fatal
[MAN ]  manual step confirmed by user
[SKIP]  group skipped via --skip
```

Exit code 0 if all automated checks pass, 1 if any fail.
Manual ([MAN]) and warned ([WARN]) results do not affect exit code.

## Port discovery

Ports are detected in order:
1. ESP32-S3 native USB (VID 0x303A)
2. CP210x / CH340 / FTDI adapters
3. `/dev/cu.usbmodem*`, `/dev/ttyACM*`, `/dev/ttyUSB*` name patterns
4. Interactive selection if multiple candidates found
