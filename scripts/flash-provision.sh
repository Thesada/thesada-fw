#!/usr/bin/env bash

# allow-long-comment
# flash-provision.sh - flash a board and seed its sticker secrets.
#
# WiFiManager refuses to raise the fallback AP without a real passphrase
# (ap_policy.h), so a unit that never had one seeded has no recovery path
# short of a serial cable. This is the step that gives it one: upload the
# firmware, generate a random per-device passphrase, push it into NVS over
# the serial shell, and write the join artifact to a gitignored directory.
#
# The same pass seeds the 8-digit claim code (enroll.claim_code) that lets an
# account claim the unit in the app, and points enroll.url at the app base
# (--app-url, default https://thesada.app; self-hosted installs pass
# their own). The claim code can only be set over serial and never rotates
# on the device, so the sticker printed from these artifacts stays valid.
#
# Secrets are generated inside the Python block and never cross argv, stdout
# or a tracked file. They land in 0600 files under the artifact directory and
# nowhere else. Losing those files means the AP cannot be joined and the unit
# cannot be claimed - re-run with --force to rotate both and re-print.
#
# Idempotent per secret: one already held by the device AND on disk is left
# alone, so a board provisioned before claim codes existed only gains one.
#
# Usage:
#   scripts/flash-provision.sh --env esp32-owb --port /dev/cu.usbmodem1101
#   scripts/flash-provision.sh --env esp32-owb --skip-upload     # seed only
#   scripts/flash-provision.sh --env esp32-owb --force           # rotate
#   scripts/flash-provision.sh --skip-upload --app-url https://app.example.org
#
# Requires pyserial. Also renders join-wifi.png and claim.png when the
# `qrcode` module is installed; the payload files scan the same either way
# (qrencode -o join.png < join-wifi.txt).

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

ENV_NAME=""
PORT=""
SKIP_UPLOAD=0
FORCE=0
OUT_DIR="${PROVISION_DIR:-build/provision}"
APP_URL="${THESADA_APP_URL:-https://thesada.app}"

while [ $# -gt 0 ]; do
  case "$1" in
    --env)         ENV_NAME="${2:-}"; shift 2 ;;
    --port)        PORT="${2:-}"; shift 2 ;;
    --out)         OUT_DIR="${2:-}"; shift 2 ;;
    --app-url)     APP_URL="${2:-}"; shift 2 ;;
    --skip-upload) SKIP_UPLOAD=1; shift ;;
    --force)       FORCE=1; shift ;;
    -h|--help)     sed -n '4,34p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

python3 - "$APP_URL" <<'PY' || exit 2
import sys
from urllib.parse import urlsplit

url = sys.argv[1]
parts = urlsplit(url)
bad = (not url.startswith("https://") or not parts.hostname or len(url) >= 128
       or any(c in url for c in "?#@ ") or any(ord(c) < 0x21 for c in url))
if bad:
    print(f"flash-provision: --app-url must be https://host[/path], got {url!r}",
          file=sys.stderr)
    sys.exit(1)
PY

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

python3 - "$OUT_DIR" "$PORT" "$FORCE" "$APP_URL" <<'PY'
import os
import secrets
import string
import sys
import time
from urllib.parse import urlencode

out_dir, port, force, app_url = (sys.argv[1], sys.argv[2], sys.argv[3] == "1",
                                 sys.argv[4].rstrip("/"))

# Same serial framing the HIL harness uses; there is no second implementation.
sys.path.insert(0, os.path.join(os.getcwd(), "tests"))
from test_firmware import DeviceShell, discover_port   # noqa: E402

AP_FIELD = "wifi.ap_password"
CLAIM_FIELD = "enroll.claim_code"
URL_KEY = "enroll.url"
# WPA2 takes 8..63 printable ASCII. Alphanumeric only: ; and : are separators
# in the WIFI: QR payload and would otherwise need escaping.
ALPHABET = string.ascii_letters + string.digits
PASS_LEN = 24
CLAIM_LEN = 8


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


def write_qr(path, payload):
    # Renders payload as a 0600 PNG. in: path, payload. out: True if written.
    try:
        import qrcode
    except ImportError:
        return False
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as fh:
        qrcode.make(payload).save(fh)
    return True


def needs_seed(sh, key, artifact, device_id):
    # Decides whether key gets a fresh value. in: shell, NVS field, artifact
    # path. out: True to seed; exits when the device holds one we cannot print.
    # A config-backed value counts as seeded; reading only "nvs" as seeded
    # would rotate a valid credential without --force.
    source = secret_state(sh, key)
    seeded = source in ("nvs", "config")
    if not seeded or force:
        return True
    if os.path.exists(artifact):
        return False
    fail(f"{device_id} holds {key} ({source}) but {artifact} is missing. "
         "It cannot be read back - re-run with --force to rotate")


def seed(sh, key, value):
    out = sh.cmd(f"secret.set {key} {value}", wait=2.0)
    if not any("secret stored in NVS" in line for line in out):
        fail(f"secret.set {key} refused: {out}")
    # Confirm it landed rather than trusting the response line.
    if secret_state(sh, key) != "nvs":
        fail(f"secret.set {key} reported success but secret.info still reads config/none")


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
    ssid = field(info, "ap_ssid:") or f"{device_id}-setup"

    dev_dir = os.path.join(out_dir, device_id)
    creds = os.path.join(dev_dir, "ap-credentials.txt")
    claim = os.path.join(dev_dir, "claim.txt")
    seed_ap = needs_seed(sh, AP_FIELD, creds, device_id)
    seed_claim = needs_seed(sh, CLAIM_FIELD, claim, device_id)

    current_url = "".join(sh.cmd(f"config.get {URL_KEY}")).strip().strip('"')
    set_url = current_url != app_url
    if set_url:
        out = sh.cmd(f"config.set {URL_KEY} {app_url}", wait=2.0)
        if not any(line.startswith(f"Set {URL_KEY} = ") for line in out):
            fail(f"config.set {URL_KEY} refused: {out}")

    if not (seed_ap or seed_claim or set_url):
        print(f"flash-provision: {device_id} already provisioned, artifacts in {dev_dir}")
        sys.exit(0)

    os.makedirs(dev_dir, exist_ok=True)
    os.chmod(dev_dir, 0o700)
    written = []
    stamp = time.strftime('%Y-%m-%dT%H:%M:%S%z')

    if seed_ap:
        password = "".join(secrets.choice(ALPHABET) for _ in range(PASS_LEN))
        seed(sh, AP_FIELD, password)
        join = f"WIFI:T:WPA;S:{ssid};P:{password};;"
        join_txt = os.path.join(dev_dir, "join-wifi.txt")
        write_private(creds,
                      f"device_id: {device_id}\n"
                      f"ap_ssid:   {ssid}\n"
                      f"ap_pass:   {password}\n"
                      f"seeded_at: {stamp}\n")
        write_private(join_txt, join + "\n")
        written += [creds, join_txt]
        png = os.path.join(dev_dir, "join-wifi.png")
        if write_qr(png, join):
            written.append(png)

    if seed_claim or (set_url and os.path.exists(claim)):
        if seed_claim:
            code = "".join(secrets.choice(string.digits) for _ in range(CLAIM_LEN))
            seed(sh, CLAIM_FIELD, code)
        else:
            with open(claim) as fh:
                code = field(fh.read().splitlines(), "claim_code:")
            if len(code) != CLAIM_LEN or not code.isdigit():
                fail(f"{claim} has no usable claim_code - re-run with --force")
        link = f"{app_url}/devices/claim?" + urlencode({"device_id": device_id, "code": code})
        link_txt = os.path.join(dev_dir, "claim-link.txt")
        write_private(claim,
                      f"device_id:  {device_id}\n"
                      f"claim_code: {code}\n"
                      f"app_url:    {app_url}\n"
                      f"claim_link: {link}\n"
                      f"seeded_at:  {stamp}\n")
        write_private(link_txt, link + "\n")
        written += [claim, link_txt]
        png = os.path.join(dev_dir, "claim.png")
        if write_qr(png, link):
            written.append(png)

    print(f"flash-provision: {device_id} provisioned, ssid {ssid}, app {app_url}")
    for path in written:
        print(f"  {path}")
    if written:
        print("flash-provision: the secrets are only in those files - back them up")
finally:
    sh.close()
PY
