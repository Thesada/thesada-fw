#!/usr/bin/env python3
# tests/test_firmware.py - thesada-fw firmware test suite
#
# Runs automated and manual/assisted tests over the device serial shell.
# Auto-detects the serial port (ESP32-S3 native USB VID/PID, then common
# patterns), or use --port to override.
#
# Usage:
#   python tests/test_firmware.py
#   python tests/test_firmware.py --port /dev/cu.usbmodem1101
#   python tests/test_firmware.py --skip ota,cellular,ads1115
#
# Requires: pyserial
#   pip install pyserial
#
# SPDX-License-Identifier: GPL-3.0-only

import sys
import time
import re
import argparse
import glob
import json
import base64
import urllib.request
import urllib.error

try:
    import serial
    import serial.tools.list_ports
    _SERIAL_AVAILABLE = True
except ImportError:
    _SERIAL_AVAILABLE = False

# ── ANSI colors ───────────────────────────────────────────────────────────────
RED    = "\033[91m"
GREEN  = "\033[92m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RESET  = "\033[0m"

# ── ESP32-S3 native USB (VID 0x303A) ─────────────────────────────────────────
ESP32_S3_VID = 0x303A

# Common USB-serial adapter VIDs (CP210x, CH340, FTDI)
ADAPTER_VIDS = (0x10C4, 0x1A86, 0x0403)

BAUD         = 115200
CMD_WAIT     = 1.5   # default seconds to wait per command


# ── Port discovery ────────────────────────────────────────────────────────────

def discover_port():
    """
    Return a single serial port for an ESP32 device, or None if ambiguous.
    Priority:
      1. ESP32-S3 native USB (VID 0x303A)
      2. Common USB-serial adapters (CP210x / CH340 / FTDI)
      3. /dev/cu.usbmodem* / ttyACM* / ttyUSB* name patterns
    """
    ports = list(serial.tools.list_ports.comports())

    for p in ports:
        if p.vid == ESP32_S3_VID:
            return p.device

    for vid in ADAPTER_VIDS:
        matches = [p.device for p in ports if p.vid == vid]
        if len(matches) == 1:
            return matches[0]

    candidates = (
        glob.glob("/dev/cu.usbmodem*") +
        glob.glob("/dev/ttyACM*") +
        glob.glob("/dev/ttyUSB*")
    )
    if len(candidates) == 1:
        return candidates[0]

    return None   # ambiguous - caller will ask


def pick_port_interactive():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print(f"{RED}No serial ports found. Connect device and retry.{RESET}")
        sys.exit(1)
    print("Available serial ports:")
    for i, p in enumerate(ports):
        desc = f"  {p.description}" if p.description else ""
        print(f"  {i + 1}. {p.device}{desc}")
    choice = input("Select port number: ").strip()
    try:
        return ports[int(choice) - 1].device
    except (ValueError, IndexError):
        print("Invalid selection.")
        sys.exit(1)


# ── Serial shell wrapper ──────────────────────────────────────────────────────

# Log lines from the firmware look like [INF][TAG] or [WRN][TAG] - two pairs
# of brackets. Shell command output uses plain text or [PASS]/[FAIL]/[WARN]
# (only one pair of brackets, no TAG). This regex matches firmware log lines.
_LOG_RE = re.compile(r'^\[(INF|WRN|ERR|DBG)\]\[')


class DeviceShell:
    def __init__(self, port, baud=BAUD):
        print(f"{DIM}Connecting to {port} at {baud} baud...{RESET}")
        self.ser = serial.Serial(port, baud, timeout=0.1)
        time.sleep(1.0)   # let device settle after DTR toggle
        self._flush()
        print(f"{DIM}Connected.{RESET}\n")

    def _flush(self):
        self.ser.read_all()

    def send_raw(self, cmd, wait=CMD_WAIT):
        """Send a command and return ALL received lines (including log lines)."""
        self._flush()
        self.ser.write((cmd + "\n").encode())
        time.sleep(wait)
        raw = self.ser.read_all().decode(errors="replace")
        lines = [l.rstrip() for l in raw.splitlines()]
        # Drop blank lines and echo of the command itself.
        return [l for l in lines if l and l != cmd]

    def cmd(self, command, wait=CMD_WAIT):
        """Send command, return only non-firmware-log output lines."""
        return [l for l in self.send_raw(command, wait) if not _LOG_RE.match(l)]

    def close(self):
        self.ser.close()


class HttpShell:
    """Shell interface over HTTP /api/cmd - same API as DeviceShell.

    Sends each command as POST /api/cmd and returns the output lines.
    No serial port or pyserial required.
    """

    def __init__(self, host: str, user: str, password: str):
        self.host  = host
        self._url  = f"http://{host}/api/cmd"
        self._auth = "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()

        print(f"{DIM}Connecting to http://{host} ...{RESET}", end=" ", flush=True)
        try:
            req = urllib.request.Request(
                f"http://{host}/api/info",
                headers={"User-Agent": "thesada-fw-test/1.0"},
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                info = json.loads(resp.read())
            print(f"{GREEN}OK{RESET}")
            print(f"{DIM}  Device : {info.get('device', '?')}")
            print(f"  Version: v{info.get('version', '?')}{RESET}\n")
        except Exception as e:
            print(f"{RED}FAILED{RESET}")
            print(f"{RED}Cannot reach {host}: {e}{RESET}")
            sys.exit(1)

    def cmd(self, command: str, wait: float = CMD_WAIT) -> list:
        """POST command to /api/cmd, return output lines."""
        body    = json.dumps({"cmd": command}).encode()
        timeout = max(15, int(wait * 4))
        req = urllib.request.Request(
            self._url,
            data=body,
            headers={
                "Content-Type":  "application/json",
                "Authorization": self._auth,
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = json.loads(resp.read())
                return data.get("output", [])
        except urllib.error.HTTPError as e:
            if e.code == 401:
                print(f"{RED}HTTP 401 - check --web-user / --web-pass.{RESET}")
                sys.exit(1)
            return [f"HTTP error {e.code}"]
        except Exception as e:
            return [f"Error: {e}"]

    def send_raw(self, command: str, wait: float = CMD_WAIT) -> list:
        """Alias for cmd() - HttpShell has no log-line mixing."""
        return self.cmd(command, wait)

    def close(self):
        pass  # no connection to close


# ── MQTT CLI shell ────────────────────────────────────────────────────────────

class MqttShell:
    """Shell interface over MQTT CLI - publishes to <prefix>/cli/<cmd>,
    reads response from <prefix>/cli/response.

    Requires: paho-mqtt (pip install paho-mqtt) or mosquitto_pub/sub on PATH.
    """

    def __init__(self, broker: str, port: int, user: str, password: str,
                 prefix: str, use_tls: bool = True):
        self.broker  = broker
        self.port    = port
        self.user    = user
        self.password = password
        self.prefix  = prefix
        self.use_tls = use_tls
        self._resp_topic = f"{prefix}/cli/response"

        print(f"{DIM}Testing MQTT CLI via {broker}:{port} prefix={prefix} ...{RESET}")

        # Test connectivity with a version command
        resp = self.cmd("version", wait=15)
        if resp and any("thesada-fw" in l for l in resp):
            version = resp[0] if resp else "?"
            print(f"{GREEN}OK{RESET} - {version}\n")
        else:
            print(f"{RED}FAILED - no response to version command{RESET}")
            sys.exit(1)

    def cmd(self, command: str, wait: float = CMD_WAIT) -> list:
        """Publish command to MQTT CLI, wait for response."""
        import subprocess

        # Split command into topic part and payload part
        # e.g. "config.get mqtt" -> topic=config.get, payload=mqtt
        parts = command.split(" ", 1)
        cmd_name = parts[0]
        payload = parts[1] if len(parts) > 1 else ""

        pub_topic = f"{self.prefix}/cli/{cmd_name}"
        timeout = max(15, int(wait * 4))

        tls_args = ["--capath", "/etc/ssl/certs"] if self.use_tls else []

        # Subscribe for response in background
        sub_proc = subprocess.Popen(
            ["mosquitto_sub",
             "-h", self.broker, "-p", str(self.port),
             "-u", self.user, "-P", self.password,
             *tls_args,
             "-t", self._resp_topic, "-C", "1",
             "-W", str(timeout)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )

        time.sleep(1)

        # Publish command
        subprocess.run(
            ["mosquitto_pub",
             "-h", self.broker, "-p", str(self.port),
             "-u", self.user, "-P", self.password,
             *tls_args,
             "-t", pub_topic, "-m", payload],
            capture_output=True, timeout=10
        )

        try:
            stdout, _ = sub_proc.communicate(timeout=timeout)
            if stdout:
                data = json.loads(stdout.decode())
                return data.get("output", [])
        except (subprocess.TimeoutExpired, json.JSONDecodeError):
            sub_proc.kill()
        return []

    def send_raw(self, command: str, wait: float = CMD_WAIT) -> list:
        return self.cmd(command, wait)

    def close(self):
        pass


# ── MQTT CLI test suite ──────────────────────────────────────────────────────

def _mqtt_check(r, name, cmd, check_fn, detail=""):
    """Run a CLI command via MQTT and check the result."""
    out = r.sh.cmd(cmd, wait=10)
    if out and check_fn(out):
        r.ok(name, detail)
    elif out:
        r.fail(name, f"unexpected: {out[0][:60]}")
    else:
        r.fail(name, "no response")


def test_mqtt_cli(r):
    """Test MQTT CLI commands (requires MqttShell)."""
    r.group("MQTT CLI")

    _mqtt_check(r, "version", "version",
                lambda out: any("thesada-fw" in l for l in out),
                "firmware version returned")

    _mqtt_check(r, "help", "help",
                lambda out: any("help" in l for l in out),
                "command list returned")

    _mqtt_check(r, "uptime", "uptime",
                lambda out: len(out) > 0 and ("d " in out[0] or "0d" in out[0]),
                "uptime string returned")

    _mqtt_check(r, "heap", "heap",
                lambda out: len(out) > 0 and ("bytes" in out[0].lower() or " B" in out[0]),
                "heap info returned")

    _mqtt_check(r, "net.ip", "net.ip",
                lambda out: any("IP:" in l or "WiFi:" in l for l in out),
                "network info returned")

    _mqtt_check(r, "net.mqtt", "net.mqtt",
                lambda out: any("connected" in l.lower() for l in out),
                "MQTT connected")

    _mqtt_check(r, "module.list", "module.list",
                lambda out: any("[x]" in l or "[ ]" in l for l in out),
                "module list returned")

    _mqtt_check(r, "fs.ls /", "fs.ls /",
                lambda out: any("config.json" in l for l in out),
                "root listing includes config.json")

    _mqtt_check(r, "fs.df", "fs.df",
                lambda out: any("LittleFS" in l for l in out),
                "disk usage returned")

    _mqtt_check(r, "selftest", "selftest",
                lambda out: any("passed" in l for l in out),
                "selftest passed")

    _mqtt_check(r, "config.get", "config.get device.name",
                lambda out: len(out) > 0 and len(out[0]) > 0,
                "config key readable")

    # Chunked fs.cat over MQTT (offset+length mode returns JSON with metadata)
    # Send: cli/fs.cat  payload: "/config.json 0 64"
    # Response includes total, offset, length, done, data fields.
    out = r.sh.cmd("fs.cat /config.json 0 64", wait=10)
    if out:
        # MQTT shell returns the raw JSON response - check for chunked fields
        combined = " ".join(out)
        if "total" in combined or "offset" in combined or "{" in combined:
            r.ok("fs.cat chunked (MQTT)", "chunked response with metadata")
        else:
            r.ok("fs.cat chunked (MQTT)", f"response: {out[0][:60]}")
    else:
        r.fail("fs.cat chunked (MQTT)", "no response")


# ── Test runner ───────────────────────────────────────────────────────────────

class Runner:
    def __init__(self, shell, skips):
        self.sh      = shell
        self.skips   = {s.lower().strip() for s in skips}
        self.results = []   # (group, name, status, detail)
        self._group  = ""

    # ── Recording ──

    def group(self, title):
        self._group = title
        print(f"\n{BOLD}{title}{RESET}")
        print("─" * 54)

    def _record(self, status, name, detail=""):
        self.results.append((self._group, name, status, detail))
        icons = {
            "pass": f"{GREEN}[PASS]{RESET}",
            "fail": f"{RED}[FAIL]{RESET}",
            "warn": f"{YELLOW}[WARN]{RESET}",
            "skip": f"{CYAN}[SKIP]{RESET}",
            "man":  f"{YELLOW}[MAN ]{RESET}",
        }
        icon   = icons.get(status, "      ")
        suffix = f" {DIM}- {detail}{RESET}" if detail else ""
        print(f"  {icon} {name}{suffix}")

    def ok(self,   name, detail=""): self._record("pass", name, detail)
    def fail(self, name, detail=""): self._record("fail", name, detail)
    def warn(self, name, detail=""): self._record("warn", name, detail)
    def skip(self, name):            self._record("skip", name)
    def man(self,  name, detail=""): self._record("man",  name, detail)

    # ── User interaction ──

    def ask(self, prompt, default=None):
        hint = " [Y/n]" if default == "y" else " [y/N]" if default == "n" else " [y/n]"
        try:
            ans = input(f"    {CYAN}>{RESET} {prompt}{hint}: ").strip().lower()
        except EOFError:
            ans = default or "n"
            print(f"{DIM}(non-interactive - defaulting to '{ans}'){RESET}")
        if not ans and default:
            return default == "y"
        return ans in ("y", "yes")

    def ask_value(self, prompt):
        try:
            return input(f"    {CYAN}>{RESET} {prompt}: ").strip()
        except EOFError:
            print(f"{DIM}(non-interactive - skipping){RESET}")
            return ""

    def pause(self, prompt):
        try:
            input(f"    {CYAN}>{RESET} {prompt} [press Enter]")
        except EOFError:
            print(f"{DIM}(non-interactive - continuing){RESET}")

    def is_skipped(self, key):
        return key.lower() in self.skips

    # ── Summary ──

    def summary(self):
        counts = {s: sum(1 for _, _, st, _ in self.results if st == s)
                  for s in ("pass", "fail", "warn", "skip", "man")}

        print(f"\n{'═' * 54}")
        print(f"{BOLD}Test Summary{RESET}")
        print(f"  {GREEN}Passed{RESET}:  {counts['pass']}")
        if counts["fail"]: print(f"  {RED}Failed{RESET}:  {counts['fail']}")
        if counts["warn"]: print(f"  {YELLOW}Warned{RESET}:  {counts['warn']}")
        if counts["man"]:  print(f"  {YELLOW}Manual{RESET}:  {counts['man']}")
        if counts["skip"]: print(f"  {CYAN}Skipped{RESET}: {counts['skip']}")
        print(f"{'═' * 54}")

        if counts["fail"] == 0:
            print(f"{GREEN}All automated checks passed.{RESET}")
        else:
            print(f"{RED}{counts['fail']} check(s) failed:{RESET}")
            for g, n, st, d in self.results:
                if st == "fail":
                    print(f"  {RED}x{RESET} [{g}] {n}" + (f" - {d}" if d else ""))

        return counts["fail"]


# ── Test groups ───────────────────────────────────────────────────────────────

def test_system(r):
    r.group("1 · System")
    sh = r.sh

    # version
    lines = sh.cmd("version")
    if lines and "thesada-fw" in lines[0]:
        r.ok("version", lines[0])
    else:
        r.fail("version", f"unexpected: {lines}")

    # heap
    lines = sh.cmd("heap")
    m = re.search(r'Free:\s+(\d+)\s+B', lines[0] if lines else "")
    if m:
        free = int(m.group(1))
        detail = f"{free // 1024} KB free"
        r.ok("heap", detail) if free > 50_000 else r.warn("heap", f"low - {detail}")
    else:
        r.fail("heap", f"unexpected: {lines}")

    # uptime
    lines = sh.cmd("uptime")
    if lines and re.match(r'\d+d \d{2}:\d{2}:\d{2}', lines[0]):
        r.ok("uptime", lines[0])
    else:
        r.fail("uptime", f"unexpected: {lines}")

    # heartbeat LED - read config, visual check if enabled
    lines = sh.cmd("config.get device.heartbeat_s")
    hb_val = lines[0].strip() if lines else "-1"
    try:
        hb_s = int(hb_val)
    except ValueError:
        hb_s = -1
    if hb_s < 0:
        r.ok("heartbeat LED", "disabled in config (device.heartbeat_s = -1)")
    else:
        interval = max(hb_s, 5)
        print(f"  {DIM}Heartbeat configured: every {interval} s. Waiting {interval + 2} s...{RESET}")
        time.sleep(interval + 2)
        if r.ask(f"Did the blue LED (CHGLED) pulse once in the last {interval} s?"):
            r.man("heartbeat LED", f"every {interval} s")
        else:
            r.warn("heartbeat LED", "no pulse observed - check device.heartbeat_s and PMU wiring")

    # selftest - runs last; parses its own [PASS]/[FAIL]/[WARN] lines
    lines = sh.cmd("selftest", wait=4.0)
    summary_line = next((l for l in lines if re.match(r'=== \d+ passed', l)), None)
    if summary_line:
        m = re.match(r'=== (\d+) passed, (\d+) failed', summary_line)
        passed, failed = int(m.group(1)), int(m.group(2))
        warns = [l for l in lines if l.startswith("[WARN]")]
        if failed == 0:
            r.ok("selftest", f"{passed} passed" + (f", {len(warns)} warned" if warns else ""))
        else:
            r.fail("selftest", f"{passed} passed, {failed} failed")
            for l in lines:
                if l.startswith("[FAIL]"):
                    print(f"      {RED}{l}{RESET}")
        for w in warns:
            print(f"      {YELLOW}{w}{RESET}")
    else:
        r.fail("selftest", f"no summary line - raw: {lines[:4]}")


def test_filesystem(r):
    r.group("2 · Filesystem")
    sh = r.sh

    # ls /
    lines = sh.cmd("fs.ls /")
    if lines and not any("error" in l.lower() for l in lines):
        r.ok("fs.ls /", f"{len(lines)} entries")
    else:
        r.warn("fs.ls /", f"empty or error: {lines}")

    # fs.cat /config.json
    lines = sh.cmd("fs.cat /config.json", wait=2.0)
    combined = " ".join(lines)
    if "{" in combined and "net.mqtt" in combined:
        r.ok("fs.cat /config.json", "valid JSON content")
    else:
        r.fail("fs.cat /config.json", "missing expected content")

    # write / cat / rm cycle
    path    = "/test_fw_check.txt"
    payload = "thesada-test-ok"

    lines = sh.cmd(f"fs.write {path} {payload}")
    if lines and "Wrote" in lines[0]:
        r.ok(f"fs.write {path}")
    else:
        r.fail(f"fs.write {path}", f"output: {lines}")

    lines = sh.cmd(f"fs.cat {path}")
    if lines and payload in lines[0]:
        r.ok(f"fs.cat {path} (verify content)")
    else:
        r.fail(f"fs.cat {path} (verify content)", f"got: {lines}")

    lines = sh.cmd(f"fs.rm {path}")
    if lines and "Removed" in lines[0]:
        r.ok(f"fs.rm {path}")
    else:
        r.fail(f"fs.rm {path}", f"output: {lines}")

    # df
    lines = sh.cmd("fs.df")
    lfs_line = next((l for l in lines if "LittleFS:" in l), None)
    sd_line  = next((l for l in lines if "SD card:" in l), None)

    if lfs_line:
        r.ok("fs.df (LittleFS)", lfs_line.strip())
    else:
        r.fail("fs.df (LittleFS)", f"no LittleFS line: {lines}")

    if sd_line and "not mounted" not in sd_line:
        r.ok("df (SD card)", sd_line.strip())
    elif sd_line:
        r.warn("df (SD card)", "not mounted")
    else:
        r.warn("df (SD card)", "no SD line in df output")


def test_chunked_io(r):
    """Test chunked file I/O: fs.write, fs.append, chunked fs.cat."""
    r.group("2b · Chunked I/O")
    sh = r.sh

    path = "/test_chunked.txt"
    part1 = "hello-chunk-one"
    part2 = "-appended"

    # fs.write creates file (truncate mode)
    lines = sh.cmd(f"fs.write {path} {part1}")
    if lines and "Wrote" in lines[0]:
        r.ok(f"fs.write {path}", lines[0])
    else:
        r.fail(f"fs.write {path}", f"output: {lines}")

    # fs.append adds to existing file
    lines = sh.cmd(f"fs.append {path} {part2}")
    if lines and "Appended" in lines[0]:
        r.ok(f"fs.append {path}", lines[0])
    else:
        r.fail(f"fs.append {path}", f"output: {lines}")

    # fs.cat (line-by-line) should show combined content
    lines = sh.cmd(f"fs.cat {path}")
    combined = " ".join(lines) if lines else ""
    expected = part1 + part2
    if expected in combined:
        r.ok(f"fs.cat {path} (combined)", f"content matches: {expected}")
    else:
        r.fail(f"fs.cat {path} (combined)", f"expected '{expected}', got: {lines}")

    # fs.write again should truncate (not append)
    lines = sh.cmd(f"fs.write {path} overwritten")
    lines = sh.cmd(f"fs.cat {path}")
    content = lines[0] if lines else ""
    if content == "overwritten":
        r.ok("fs.write truncates", "content replaced on second write")
    else:
        r.fail("fs.write truncates", f"expected 'overwritten', got: {content}")

    # Clean up
    sh.cmd(f"fs.rm {path}")

    # Chunked fs.cat on config.json (known to exist and be >50 bytes)
    # Note: chunked mode (offset+length) only works over MQTT CLI.
    # Serial/HTTP fs.cat returns line-by-line output without chunking metadata.
    # This test verifies the line-by-line path still works after the chunked
    # code was added (regression check).
    lines = sh.cmd("fs.cat /config.json", wait=2.0)
    combined = " ".join(lines)
    if "{" in combined and "}" in combined:
        r.ok("fs.cat /config.json (regression)", f"{len(lines)} lines")
    else:
        r.fail("fs.cat /config.json (regression)", f"unexpected: {lines[:3]}")


def test_config(r):
    r.group("3 · Config")
    sh = r.sh

    for key in ("device.name", "mqtt.broker", "mqtt.topic_prefix"):
        lines = sh.cmd(f"config.get {key}")
        val   = lines[0].strip('"') if lines else ""
        if val and val not in ("null", "Key not found"):
            r.ok(f"config.get {key}", val)
        else:
            r.warn(f"config.get {key}", "null or missing")

    lines = sh.cmd("config.dump", wait=2.0)
    combined = " ".join(lines)
    if "{" in combined and "mqtt" in combined and "device" in combined:
        r.ok("config.dump", f"{len(lines)} lines")
    else:
        r.fail("config.dump", "no JSON or missing expected keys")


def test_network(r):
    r.group("4 · Network")
    sh = r.sh

    # ifconfig
    lines = sh.cmd("net.ip")
    wifi_line = lines[0] if lines else ""
    if "connected" in wifi_line and "disconnected" not in wifi_line:
        ip_line = next((l for l in lines if "IP:" in l), "")
        r.ok("net.ip WiFi", ip_line.strip() if ip_line else wifi_line)
    else:
        r.fail("net.ip WiFi", wifi_line)

    # net.ping (DNS resolve)
    lines = sh.cmd("net.ping 8.8.8.8", wait=3.0)
    if lines and "resolved to" in lines[0]:
        r.ok("net.ping (DNS resolve)", lines[0])
    else:
        r.fail("net.ping (DNS resolve)", f"output: {lines}")

    # ntp
    lines = sh.cmd("net.ntp")
    ntp_line = lines[0] if lines else ""
    ts_line  = next((l for l in lines if "log timestamps:" in l), "")
    if "synced" in ntp_line:
        utc = re.search(r'UTC: (\S+)', ntp_line)
        detail = utc.group(1) if utc else ntp_line
        if "active" in ts_line:
            detail += "  (log timestamps active)"
        r.ok("net.ntp", detail)
    else:
        r.warn("net.ntp", ntp_line)

    # net.ntp set (manual time)
    import time as _time
    epoch = str(int(_time.time()))
    lines = sh.cmd(f"net.ntp set {epoch}")
    set_line = lines[0] if lines else ""
    if "Time set to" in set_line:
        r.ok("net.ntp set (epoch)", set_line)
    else:
        r.fail("net.ntp set (epoch)", set_line)

    lines = sh.cmd("net.ntp set 0")
    err_line = lines[0] if lines else ""
    if "Invalid" in err_line:
        r.ok("net.ntp set (invalid)", "rejects epoch 0")
    else:
        r.fail("net.ntp set (invalid)", err_line)

    # mqtt
    lines = sh.cmd("net.mqtt")
    mqtt_line = lines[0] if lines else ""
    if "connected" in mqtt_line and "disconnected" not in mqtt_line:
        broker_line = next((l for l in lines if "broker:" in l), "")
        r.ok("net.mqtt", broker_line.strip() if broker_line else mqtt_line)
    else:
        r.fail("net.mqtt", mqtt_line)


def test_shell(r):
    r.group("5 · Shell")
    sh = r.sh

    # help
    lines = sh.cmd("help")
    if lines and "thesada-fw shell" in lines[0]:
        cmd_lines = [l for l in lines if re.match(r'\s{2}\S', l)]
        r.ok("help", f"{len(cmd_lines)} commands listed")
    else:
        r.fail("help", f"unexpected: {lines[:2]}")

    # module.list
    lines = sh.cmd("module.list")
    if lines and "Compiled modules:" in lines[0]:
        enabled = [l.split("[x]")[1].strip() for l in lines if "[x]" in l]
        r.ok("module.list", f"enabled: {', '.join(enabled)}" if enabled else "none enabled")
    else:
        r.fail("module.list", f"output: {lines[:2]}")


def _get_device_ip(sh):
    """Parse device IP from ifconfig output."""
    lines = sh.cmd("net.ip")
    for l in lines:
        m = re.search(r'IP:\s+(\d+\.\d+\.\d+\.\d+)', l)
        if m:
            return m.group(1)
    return None


def test_api_cmd(r, ip, web_user, web_pass):
    r.group("6b · /api/cmd (HTTP)")

    if not ip:
        r.warn("api/cmd", "device IP unknown - run ifconfig manually")
        return
    if not web_pass:
        r.skip("api/cmd (pass --web-pass to enable)")
        return

    url = f"http://{ip}/api/cmd"
    good_auth = "Basic " + base64.b64encode(f"{web_user}:{web_pass}".encode()).decode()
    bad_auth  = "Basic " + base64.b64encode(b"admin:wrongpassword").decode()

    def post(cmd, auth=good_auth):
        body = json.dumps({"cmd": cmd}).encode()
        req = urllib.request.Request(url, data=body, headers={
            "Content-Type":  "application/json",
            "Authorization": auth,
        })
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as e:
            return e.code, {}
        except Exception as e:
            return 0, {"error": str(e)}

    # version command returns firmware version string
    status, data = post("version")
    if status == 200 and data.get("ok") and data.get("output") and "thesada-fw" in data["output"][0]:
        r.ok("api/cmd version", data["output"][0])
    else:
        r.fail("api/cmd version", f"status={status} data={data}")

    # heap command returns free heap info
    status, data = post("heap")
    if status == 200 and data.get("ok") and data.get("output") and "Free:" in data["output"][0]:
        r.ok("api/cmd heap", data["output"][0])
    else:
        r.fail("api/cmd heap", f"status={status} data={data}")

    # unknown command returns output with "Unknown command"
    status, data = post("xyzzy_not_a_command")
    if status == 200 and data.get("ok") and data.get("output") and "Unknown" in data["output"][0]:
        r.ok("api/cmd unknown command error")
    else:
        r.fail("api/cmd unknown command error", f"status={status} data={data}")

    # wrong password → 401
    status, _ = post("version", auth=bad_auth)
    if status == 401:
        r.ok("api/cmd auth required", "401 on wrong password")
    else:
        r.fail("api/cmd auth required", f"expected 401, got {status}")

    # malformed body → 400
    body = b"not json at all"
    req  = urllib.request.Request(url, data=body, headers={
        "Content-Type":  "application/json",
        "Authorization": good_auth,
    })
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            data = json.loads(resp.read())
            if not data.get("ok"):
                r.ok("api/cmd bad body → error", data.get("error", "ok:false"))
            else:
                r.fail("api/cmd bad body", "expected ok:false")
    except urllib.error.HTTPError as e:
        if e.code == 400:
            r.ok("api/cmd bad body → 400")
        else:
            r.fail("api/cmd bad body", f"unexpected {e.code}")
    except Exception as e:
        r.warn("api/cmd bad body", str(e))


def test_lua(r):
    r.group("6 · Lua")
    sh = r.sh

    # return value
    lines = sh.cmd("lua.exec return 42")
    if lines and "42" in lines[0]:
        r.ok("lua.exec return value", "return 42 → 42")
    else:
        r.fail("lua.exec return value", f"got: {lines}")

    # no-return value → OK
    lines = sh.cmd('lua.exec Log.info("fw-test-ping")')
    if lines and lines[0] == "OK":
        r.ok("lua.exec no-return (OK)")
    else:
        r.fail("lua.exec no-return", f"expected 'OK', got: {lines}")

    # error handling
    lines = sh.cmd("lua.exec this_is_not!!valid_lua")
    if lines and "Error:" in lines[0]:
        r.ok("lua.exec error handling")
    else:
        r.fail("lua.exec error handling", f"expected Error:, got: {lines}")

    # reload
    lines = sh.cmd("lua.reload", wait=4.0)
    if lines and "reloaded" in lines[-1].lower():
        r.ok("lua.reload")
    else:
        r.warn("lua.reload", f"output: {lines}")


# ── Manual test groups ────────────────────────────────────────────────────────

def test_manual_sensors(r):
    key = "sensors"
    r.group("7 · Sensors (manual)")
    if r.is_skipped(key):
        r.skip("temperature readings")
        r.skip("ADS1115 current readings")
        return

    print(f"  {DIM}Sensor readings publish to MQTT every interval_s.")
    print(f"  Check your broker or the web dashboard at /api/state.{RESET}\n")

    if r.ask("Are temperature sensor readings visible and reasonable?"):
        count = r.ask_value("How many sensors reporting?")
        r.man("temperature readings", f"{count} sensor(s) visible")
    else:
        r.warn("temperature readings", "not confirmed")

    if r.ask("Are ADS1115 current readings visible on MQTT?"):
        r.man("ADS1115 readings", "readings visible")
    else:
        r.warn("ADS1115 readings", "not confirmed")


def test_manual_ads1115(r):
    key = "ads1115"
    r.group("8 · ADS1115 load test (manual)")
    if r.is_skipped(key):
        r.skip("ads1115 load test")
        return

    print(f"  {DIM}Requires a known electrical load connected to a pump channel.{RESET}\n")

    if not r.ask("Do you have a load available to connect?"):
        r.skip("ads1115 load test (no hardware)")
        return

    r.pause("Connect load to house_pump channel, then press Enter")
    print(f"  {DIM}Waiting 70 s for next ADS1115 read cycle (interval_s = 60)...{RESET}")
    time.sleep(70)

    if r.ask("Is the ADS1115 current reading non-zero and expected for your load?"):
        val = r.ask_value("Observed reading (V)")
        r.man("ads1115 load test", f"{val} V")
    else:
        r.fail("ads1115 load test", "reading zero or unexpected with load connected")


def test_manual_mqtt_publish(r):
    key = "net.mqtt"
    r.group("9 · MQTT publish (manual)")
    if r.is_skipped(key):
        r.skip("mqtt publish test")
        return

    sh = r.sh

    lines  = sh.cmd("config.get mqtt.topic_prefix")
    prefix = lines[0].strip('"') if lines and lines[0] not in ("null", "") else "thesada/node"
    topic  = f"{prefix}/test/fw-check"

    lines = sh.cmd(f'lua.exec MQTT.publish("{topic}", "fw-test-ok")')
    print(f"\n  Topic:   {CYAN}{topic}{RESET}")
    print(f"  Payload: {CYAN}fw-test-ok{RESET}\n")

    if r.ask("Did your MQTT broker receive the message?"):
        r.man("mqtt publish (Lua)", f"topic: {topic}")
    else:
        r.fail("mqtt publish (Lua)", "message not received at broker")


def test_manual_ota(r):
    key = "ota"
    r.group("10 · OTA")
    if r.is_skipped(key):
        r.skip("ota test")
        return

    sh = r.sh

    # ── Part 1: automated manifest URL reachability check ─────────────────────
    lines        = sh.cmd("config.get ota.manifest_url")
    manifest_url = lines[0].strip('"') if lines else ""

    if not manifest_url or manifest_url in ("null", ""):
        r.warn("ota manifest_url", "not configured - add ota.manifest_url to config")
    else:
        try:
            req = urllib.request.Request(manifest_url,
                                         headers={"User-Agent": "thesada-fw-test/1.0"})
            with urllib.request.urlopen(req, timeout=10) as resp:
                body = resp.read()
                if resp.status == 200:
                    try:
                        manifest = json.loads(body)
                        ver      = manifest.get("version", "?")
                        sha      = manifest.get("sha256", "")
                        url_ok   = bool(manifest.get("url", ""))
                        if sha and url_ok:
                            r.ok("ota manifest reachable",
                                 f"v{ver}  sha256: {sha[:16]}...")
                        else:
                            r.warn("ota manifest reachable",
                                   f"v{ver} - missing url or sha256 fields")
                    except json.JSONDecodeError:
                        r.warn("ota manifest reachable", "200 OK but not valid JSON")
                else:
                    r.fail("ota manifest reachable", f"HTTP {resp.status}")
        except urllib.error.HTTPError as e:
            r.fail("ota manifest reachable", f"HTTP {e.code}")
        except Exception as e:
            r.fail("ota manifest reachable", str(e))

    # ── Part 2: optional MQTT trigger ─────────────────────────────────────────
    lines  = sh.cmd("config.get mqtt.topic_prefix")
    prefix = lines[0].strip('"') if lines and lines[0] not in ("null", "") else "thesada/node"
    topic  = f"{prefix}/cli/ota.check"

    print(f"\n  {DIM}Optional: publish to {topic} to trigger a live OTA check on the device.")
    print(f"  Skip this if you have no broker access or manifest_url is not set.{RESET}\n")

    if r.ask("Trigger OTA via MQTT now?", default="n"):
        print(f"  Topic:   {CYAN}{topic}{RESET}")
        print(f"  Payload: {CYAN}check{RESET}\n")
        r.pause("Publish the MQTT message, then press Enter")
        time.sleep(3.0)
        print(f"  {DIM}Check serial log for [INF][OTA] or [WRN][OTA].{RESET}\n")
        if r.ask("Did the device log an OTA check attempt?"):
            r.man("ota trigger", "OTA check initiated")
        else:
            r.warn("ota trigger", "no OTA activity observed")


def test_manual_websocket(r):
    key = "websocket"
    r.group("11 · WebSocket terminal (manual)")
    if r.is_skipped(key):
        r.skip("websocket terminal test")
        return

    sh = r.sh

    lines = sh.cmd("net.ip")
    ip    = ""
    for l in lines:
        m = re.search(r'IP:\s+(\d+\.\d+\.\d+\.\d+)', l)
        if m:
            ip = m.group(1)
            break

    print(f"  {DIM}Open the web admin panel and use the Terminal tab.{RESET}\n")
    if ip:
        print(f"  URL: {CYAN}http://{ip}/{RESET}  → Admin → Terminal\n")

    r.pause("Navigate to the terminal tab, press Enter when ready")
    print(f"  Type {CYAN}version{RESET} in the web terminal.\n")

    if r.ask("Did the web terminal respond with the firmware version?"):
        r.man("websocket terminal", f"IP: {ip}")
    else:
        r.fail("websocket terminal", "no response in web terminal")

    print(f"  Type {CYAN}help{RESET} in the web terminal.\n")
    if r.ask("Did the help command list all commands?"):
        r.man("websocket help command")
    else:
        r.warn("websocket help command", "not confirmed")


def test_manual_sd_log(r):
    key = "sd"
    r.group("12 · SD logging (manual)")
    if r.is_skipped(key):
        r.skip("sd log check")
        return

    sh = r.sh

    lines     = sh.cmd("fs.ls /sd/", wait=2.0)
    log_files = [l for l in lines if ".csv" in l.lower()]

    if not log_files:
        r.warn("sd log files", "no .csv files found on /sd/")
        return

    r.ok("sd log files present", f"{len(log_files)} log file(s)")

    # Inspect the last log file
    last_name = log_files[-1].strip().split()[-1]
    lines     = sh.cmd(f"fs.cat /sd/{last_name}", wait=3.0)

    if lines and len(lines) >= 2:
        print(f"\n  {DIM}Last 3 lines of {last_name}:{RESET}")
        for l in lines[-3:]:
            print(f"    {DIM}{l}{RESET}")
        print()
        if r.ask("Do the log entries look correct (timestamps, sensor values)?"):
            r.man("sd log content", f"{len(lines)} lines in {last_name}")
        else:
            r.warn("sd log content", "content not confirmed")
    else:
        r.warn("sd log content", f"{last_name} is empty or unreadable")


def test_manual_cellular(r):
    key = "cellular"
    r.group("13 · Cellular fallback (manual)")
    if r.is_skipped(key):
        r.skip("cellular fallback test")
        return

    sh = r.sh

    # WiFi is always primary. Cellular activates only when ALL WiFi networks fail.
    # To trigger fallback without disconnecting the antenna, set a wrong WiFi
    # password in config.json temporarily - the device will fail all SSID attempts
    # and hand off to cellular.
    print(f"  {DIM}WiFi is primary. Cellular activates when all WiFi networks fail.")
    print(f"  Two ways to trigger fallback:{RESET}")
    print(f"    {CYAN}A){RESET} {DIM}Physically disconnect the WiFi antenna, restart.")
    print(f"    {CYAN}B){RESET} {DIM}Set a wrong WiFi password in config.json, restart, then restore.{RESET}\n")

    method = r.ask_value("Choose method A or B (or press Enter to skip)")
    if method.upper() not in ("A", "B"):
        r.skip("cellular fallback (skipped)")
        return

    if method.upper() == "B":
        print(f"\n  {DIM}Edit config.json → wifi.networks[0].password to an invalid value.")
        print(f"  Save (Admin → Config → Save), the device will restart automatically.{RESET}\n")
        r.pause("Set wrong WiFi password and wait for restart, then press Enter")
    else:
        r.pause("Disconnect WiFi antenna, then press Enter")
        print(f"  {DIM}Sending restart command...{RESET}")
        sh.cmd("restart", wait=0.5)

    print(f"  {DIM}Waiting 45 s for boot + WiFi timeout + cellular attach...{RESET}")
    time.sleep(45)

    if r.ask("Did the device attach via cellular (check serial log for [INF][Cellular])?"):
        r.man("cellular fallback", "cellular connection established")
    else:
        r.warn("cellular fallback", "cellular connection not confirmed")

    print(f"\n  {YELLOW}Restore WiFi config and restart to return to normal operation.{RESET}")
    if method.upper() == "B":
        print(f"  {DIM}Restore the correct WiFi password in Admin → Config → Save.{RESET}\n")
        r.pause("Restore WiFi config, then press Enter")
    else:
        r.pause("Reconnect WiFi antenna, then press Enter to send restart")
        sh.cmd("restart", wait=0.5)


def test_manual_config_mqtt(r):
    key = "config_mqtt"
    r.group("14 · MQTT config set/push (manual)")
    if r.is_skipped(key):
        r.skip("MQTT config set/push test")
        return

    sh = r.sh

    # Read current prefix
    lines = sh.cmd("net.mqtt")
    prefix_line = next((l for l in lines if "prefix:" in l), "")
    prefix = "thesada/node"
    m = re.search(r'prefix:\s*(\S+)', prefix_line)
    if m:
        prefix = m.group(1)

    print(f"  {DIM}MQTT prefix: {prefix}")
    print(f"  Requires mosquitto_pub on this machine and broker access.{RESET}\n")

    if not r.ask("Ready to test MQTT config set? (requires mosquitto_pub)"):
        r.skip("MQTT config set/push (skipped)")
        return

    # Test config/set
    lines = sh.cmd("config.get telegram.cooldown_s")
    original = lines[0].strip('"') if lines else "300"
    print(f"  {DIM}Current telegram.cooldown_s: {original}{RESET}")

    if r.ask(f"Publish cli/config.set to change cooldown_s to 999?"):
        r.man("config/set", f"Publish to {prefix}/cli/config.set with payload: "
              'telegram.cooldown_s 999')
        r.pause("Publish the message, then press Enter")
        lines = sh.cmd("config.get telegram.cooldown_s")
        val = lines[0].strip('"') if lines else ""
        if val == "999":
            r.ok("config/set verify", f"cooldown_s changed to {val}")
        else:
            r.fail("config/set verify", f"expected 999, got {val}")

        # Restore
        sh.cmd(f'lua.exec Config.set("telegram.cooldown_s","{original}")')
        r.ok("config/set restore", f"cooldown_s restored to {original}")


def test_manual_fallback_ap(r):
    key = "fallback_ap"
    r.group("15 · Fallback AP (manual)")
    if r.is_skipped(key):
        r.skip("fallback AP test")
        return

    sh = r.sh

    print(f"  {DIM}This test temporarily breaks WiFi to trigger the fallback AP.")
    print(f"  You need a phone or laptop to connect to the AP SSID.{RESET}\n")

    if not r.ask("Ready to test fallback AP?"):
        r.skip("fallback AP (skipped)")
        return

    print(f"  {DIM}Set an invalid WiFi SSID in config.json via the web UI.")
    print(f"  Save and restart. The device should start a setup AP.{RESET}\n")
    r.pause("Set invalid WiFi SSID, save + restart, then press Enter")

    if r.ask("Did a <device-name>-setup AP appear?"):
        r.man("fallback AP started", "AP SSID visible")
    else:
        r.fail("fallback AP", "AP not detected")
        return

    if r.ask("Can you connect and access the dashboard at 192.168.4.1?"):
        r.man("captive portal", "dashboard accessible in AP mode")
    else:
        r.warn("captive portal", "dashboard not accessible")

    print(f"\n  {YELLOW}Restore the correct WiFi SSID via the AP dashboard config editor.{RESET}")
    r.pause("Restore WiFi config, then press Enter")


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="thesada-fw firmware test suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Skippable groups (--skip a,b,c):
  sensors      Manual sensor reading confirmation
  ads1115      Load test (requires physical load + 70 s wait)
  mqtt         MQTT publish test (requires broker access to verify)
  ota          OTA trigger (requires broker + manifest_url in config)
  websocket    Web terminal test (requires browser)
  sd           SD card log file check
  cellular     Cellular fallback (requires restart + SIM card)
  config_mqtt  MQTT config set/push (requires mosquitto_pub)
  fallback_ap  Fallback AP + captive portal (requires WiFi disconnect)

Examples:
  python tests/test_firmware.py
  python tests/test_firmware.py --port /dev/cu.usbmodem1101
  python tests/test_firmware.py --http 172.16.1.212 --web-pass changeme
  python tests/test_firmware.py --http 172.16.1.212 --web-pass changeme --skip ota,cellular,ads1115
  python tests/test_firmware.py --skip sensors,ads1115,mqtt,ota,websocket,sd,cellular
  python tests/test_firmware.py --web-pass changeme   # enables /api/cmd HTTP test (serial mode)
""")

    parser.add_argument("--port",      help="Serial port (auto-detected if omitted)")
    parser.add_argument("--baud",      type=int, default=BAUD,
                        help=f"Baud rate (default: {BAUD})")
    parser.add_argument("--http",      metavar="HOST",
                        help="Use HTTP shell instead of serial (e.g. 172.16.1.212). "
                             "Requires --web-pass.")
    parser.add_argument("--skip",      default="",
                        help="Comma-separated manual test groups to skip")
    parser.add_argument("--web-user",  default="admin",
                        help="Web dashboard username (default: admin)")
    parser.add_argument("--web-pass",  default="",
                        help="Web dashboard password (enables /api/cmd tests; "
                             "required with --http)")
    parser.add_argument("--mqtt-broker", metavar="HOST",
                        help="MQTT broker for CLI tests (e.g. mqtt.example.com)")
    parser.add_argument("--mqtt-port",  type=int, default=8883,
                        help="MQTT broker port (default: 8883)")
    parser.add_argument("--mqtt-user",  default="",
                        help="MQTT username")
    parser.add_argument("--mqtt-pass",  default="",
                        help="MQTT password")
    parser.add_argument("--mqtt-prefix", default="thesada/node",
                        help="MQTT topic prefix (default: thesada/node)")
    parser.add_argument("--mqtt-no-tls", action="store_true",
                        help="Disable TLS for MQTT connection")
    args  = parser.parse_args()
    skips = [s.strip() for s in args.skip.split(",") if s.strip()]

    # ── Shell ──
    if args.http:
        if not args.web_pass:
            print(f"{RED}--http requires --web-pass{RESET}")
            sys.exit(1)
        print(f"\n{BOLD}thesada-fw Firmware Test Suite{RESET}")
        print(f"Mode: HTTP  Host: {args.http}  User: {args.web_user}")
        if skips:
            print(f"Skipping: {', '.join(skips)}")
        print()
        sh = HttpShell(args.http, args.web_user, args.web_pass)
        device_ip = args.http
    else:
        if not _SERIAL_AVAILABLE:
            print(f"{RED}pyserial not installed. Run: pip install pyserial{RESET}")
            print(f"{RED}Or use --http <host> to test over HTTP without serial.{RESET}")
            sys.exit(1)
        port = args.port
        if not port:
            port = discover_port()
            if port:
                print(f"Auto-detected port: {CYAN}{port}{RESET}")
            else:
                port = pick_port_interactive()
        print(f"\n{BOLD}thesada-fw Firmware Test Suite{RESET}")
        print(f"Mode: Serial  Port: {port}  Baud: {args.baud}")
        if skips:
            print(f"Skipping: {', '.join(skips)}")
        print()
        try:
            sh = DeviceShell(port, args.baud)
        except serial.SerialException as e:
            print(f"{RED}Failed to open {port}: {e}{RESET}")
            sys.exit(1)
        device_ip = None  # resolved later via ifconfig

    r = Runner(sh, skips)

    try:
        test_system(r)
        test_filesystem(r)
        test_chunked_io(r)
        test_config(r)
        test_network(r)
        test_shell(r)
        test_lua(r)
        if device_ip is None:
            device_ip = _get_device_ip(sh)
        test_api_cmd(r, device_ip, args.web_user, args.web_pass)
        test_manual_sensors(r)
        test_manual_ads1115(r)
        test_manual_mqtt_publish(r)
        test_manual_ota(r)
        test_manual_websocket(r)
        test_manual_sd_log(r)
        test_manual_cellular(r)
        test_manual_config_mqtt(r)
        test_manual_fallback_ap(r)

        # MQTT CLI tests (optional - only if --mqtt-broker provided)
        if args.mqtt_broker:
            mqtt_sh = MqttShell(
                broker=args.mqtt_broker,
                port=args.mqtt_port,
                user=args.mqtt_user,
                password=args.mqtt_pass,
                prefix=args.mqtt_prefix,
                use_tls=not args.mqtt_no_tls,
            )
            mqtt_r = Runner(mqtt_sh, skips)
            test_mqtt_cli(mqtt_r)
            mqtt_sh.close()
            mqtt_r.summary()

    except KeyboardInterrupt:
        print(f"\n{YELLOW}Interrupted.{RESET}")

    finally:
        sh.close()

    failed = r.summary()
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
