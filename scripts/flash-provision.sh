#!/usr/bin/env bash

# allow-long-comment
# flash-provision.sh - flash a board and seed its fallback-AP passphrase.
#
# WiFiManager refuses to raise the fallback AP without a real passphrase
# (ap_policy.h), so a unit that never had one seeded has no recovery path
# short of a serial cable. This is the step that gives it one: upload the
# firmware, generate a random per-device passphrase, push it into NVS over
# the serial shell, and write the join artifact to a gitignored directory.
#
# The passphrase is generated inside the Python block and never crosses argv,
# stdout or a tracked file. It lands in one 0600 file under the artifact
# directory and nowhere else. Losing that file means the AP cannot be joined
# again - re-run with --force to rotate.
#
# Idempotent: a device that already holds a passphrase AND has its artifact
# on disk is left alone.
#
# Usage:
#   scripts/flash-provision.sh --env esp32-owb --port /dev/cu.usbmodem1101
#   scripts/flash-provision.sh --env esp32-owb --skip-upload     # seed only
#   scripts/flash-provision.sh --env esp32-owb --force           # rotate
#
# Requires pyserial. Also renders join-wifi.png when the `qrcode` module is
# installed; the payload file scans the same either way
# (qrencode -o join.png < join-wifi.txt).

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

ENV_NAME=""
PORT=""
SKIP_UPLOAD=0
FORCE=0
OUT_DIR="${PROVISION_DIR:-build/provision}"

while [ $# -gt 0 ]; do
  case "$1" in
    --env)         ENV_NAME="${2:-}"; shift 2 ;;
    --port)        PORT="${2:-}"; shift 2 ;;
    --out)         OUT_DIR="${2:-}"; shift 2 ;;
    --skip-upload) SKIP_UPLOAD=1; shift ;;
    --force)       FORCE=1; shift ;;
    -h|--help)     sed -n '4,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)             echo "flash-provision: unknown argument $1" >&2; exit 2 ;;
  esac
done

if [ -z "$ENV_NAME" ] && [ "$SKIP_UPLOAD" != "1" ]; then
  echo "flash-provision: --env is required (or pass --skip-upload)" >&2
  exit 2
fi

command -v python3 >/dev/null || { echo "flash-provision: python3 not found"; exit 2; }
python3 -c 'import serial' 2>/dev/null || {
  echo "flash-provision: pyserial not installed (pip install pyserial)"; exit 2; }

if [ "$SKIP_UPLOAD" != "1" ]; then
  command -v pio >/dev/null || { echo "flash-provision: pio not found"; exit 2; }
  echo "flash-provision: uploading $ENV_NAME"
  if [ -n "$PORT" ]; then
    pio run -e "$ENV_NAME" -t upload --upload-port "$PORT"
  else
    pio run -e "$ENV_NAME" -t upload
  fi
  # Native-USB boards re-enumerate after the reset, so the port is briefly gone.
  echo "flash-provision: waiting for the board to come back"
  sleep 5
fi

python3 - "$OUT_DIR" "$PORT" "$FORCE" <<'PY'
import os
import secrets
import string
import sys
import time

out_dir, port, force = sys.argv[1], sys.argv[2], sys.argv[3] == "1"

# Same serial framing the HIL harness uses; there is no second implementation.
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
from test_firmware import DeviceShell, discover_port   # noqa: E402

AP_FIELD = "wifi.ap_password"
# WPA2 takes 8..63 printable ASCII. Alphanumeric only: ; and : are separators
# in the WIFI: QR payload and would otherwise need escaping.
ALPHABET = string.ascii_letters + string.digits
PASS_LEN = 24


def fail(msg):
    print(f"flash-provision: {msg}", file=sys.stderr)
    sys.exit(1)


def field(lines, key):
    for line in lines:
        if line.startswith(key):
            return line[len(key):].strip()
    return ""


def secret_state(sh, key):
    for line in sh.cmd("secret.info"):
        parts = line.split()
        if len(parts) == 2 and parts[0] == key:
            return parts[1]
    return ""


def write_private(path, data, mode="w"):
    # The open mode only applies on create. A rerun over an existing artifact
    # would otherwise write the passphrase into whatever mode it already had.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, mode) as fh:
        os.fchmod(fh.fileno(), 0o600)
        fh.write(data)


if not port:
    port = discover_port()
    if not port:
        fail("no serial port found - pass --port")

sh = DeviceShell(port, command_mode=True)
try:
    info = sh.cmd("identity.info")
    device_id = field(info, "device_id:")
    if not device_id or device_id == "(none)":
        fail("device has no identity - boot a non-rescue image once, then retry")
    ssid_reported = field(info, "ap_ssid:")

    # A config-backed passphrase is a seeded unit too. Reading only "nvs" as
    # seeded rotates a valid fallback-AP credential without --force.
    secret_source = secret_state(sh, AP_FIELD)
    seeded = secret_source in ("nvs", "config")
    dev_dir = os.path.join(out_dir, device_id)
    creds = os.path.join(dev_dir, "ap-credentials.txt")
    have_artifact = os.path.exists(creds)

    if seeded and have_artifact and not force:
        print(f"flash-provision: {device_id} already provisioned, artifact in {dev_dir}")
        sys.exit(0)
    if seeded and not have_artifact and not force:
        fail(f"{device_id} holds a passphrase ({secret_source}) but {creds} is "
             "missing. It cannot be read back - re-run with --force to rotate")

    ssid = ssid_reported or f"{device_id}-setup"
    password = "".join(secrets.choice(ALPHABET) for _ in range(PASS_LEN))

    out = sh.cmd(f"secret.set {AP_FIELD} {password}", wait=2.0)
    if not any("secret stored in NVS" in line for line in out):
        fail(f"secret.set refused: {out}")
    # Confirm it landed rather than trusting the response line.
    if secret_state(sh, AP_FIELD) != "nvs":
        fail("secret.set reported success but secret.info still reads config/none")

    os.makedirs(dev_dir, exist_ok=True)
    os.chmod(dev_dir, 0o700)

    join = f"WIFI:T:WPA;S:{ssid};P:{password};;"
    written = [creds, os.path.join(dev_dir, "join-wifi.txt")]
    write_private(creds,
                  f"device_id: {device_id}\n"
                  f"ap_ssid:   {ssid}\n"
                  f"ap_pass:   {password}\n"
                  f"seeded_at: {time.strftime('%Y-%m-%dT%H:%M:%S%z')}\n")
    write_private(written[1], join + "\n")

    try:
        import qrcode
        png = os.path.join(dev_dir, "join-wifi.png")
        fd = os.open(png, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as fh:
            qrcode.make(join).save(fh)
        written.append(png)
    except ImportError:
        pass

    print(f"flash-provision: {device_id} seeded, ssid {ssid}")
    for path in written:
        print(f"  {path}")
    print("flash-provision: the passphrase is only in those files - back them up")
finally:
    sh.close()
PY
