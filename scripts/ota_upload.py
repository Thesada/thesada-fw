#!/usr/bin/env python3
"""
ota_upload.py - Push firmware binary to a Thesada node via HTTP OTA.

Usage:
    python3 scripts/ota_upload.py [host] [--user USER] [--bin PATH]

    host     Device IP or hostname (default: 172.16.1.212)
    --user   Web admin username (default: admin)
    --bin    Path to firmware binary (default: build/firmware.bin)

Password is read from stdin (hidden input, not echoed).

Must be run from the base/ directory (or adjust --bin path).
"""

import argparse
import getpass
import sys
import urllib.request
import urllib.error
import json
import os
import io


def check_access(host: str, user: str, password: str) -> dict:
    """GET /api/info with Basic Auth. Returns parsed JSON or raises."""
    url = f"http://{host}/api/info"
    mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    mgr.add_password(None, url, user, password)
    handler = urllib.request.HTTPBasicAuthHandler(mgr)
    opener = urllib.request.build_opener(handler)

    try:
        with opener.open(url, timeout=8) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        if e.code == 401:
            raise PermissionError("Authentication failed - wrong username or password")
        raise RuntimeError(f"HTTP {e.code} from {url}")
    except urllib.error.URLError as e:
        raise ConnectionError(f"Cannot reach {host}: {e.reason}")


def upload_firmware(host: str, user: str, password: str, bin_path: str) -> bool:
    """POST firmware binary to /ota as multipart/form-data. Returns True on success."""
    url = f"http://{host}/ota"

    with open(bin_path, "rb") as f:
        firmware_data = f.read()

    boundary = "----ThesadaOTABoundary7080"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; filename="firmware.bin"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + firmware_data + f"\r\n--{boundary}--\r\n".encode()

    import base64
    credentials = base64.b64encode(f"{user}:{password}".encode()).decode()

    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
            "Authorization": f"Basic {credentials}",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            result = json.loads(resp.read().decode())
            return result.get("ok", False)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            raise PermissionError("Authentication failed")
        raise RuntimeError(f"Upload failed: HTTP {e.code}")


def main():
    parser = argparse.ArgumentParser(description="Push OTA firmware to a Thesada node")
    parser.add_argument("host", nargs="?", default="172.16.1.212", help="Device IP or hostname")
    parser.add_argument("--user", default="admin", help="Web admin username")
    parser.add_argument("--bin", default="build/firmware.bin", help="Path to firmware .bin")
    args = parser.parse_args()

    if not os.path.isfile(args.bin):
        print(f"ERROR: Firmware binary not found: {args.bin}", file=sys.stderr)
        print("Run 'pio run' first to build the firmware.", file=sys.stderr)
        sys.exit(1)

    bin_size = os.path.getsize(args.bin)

    password = getpass.getpass(f"Password for {args.user}@{args.host}: ")

    # Access check
    print(f"Connecting to {args.host}...", end=" ", flush=True)
    try:
        info = check_access(args.host, args.user, password)
    except (PermissionError, ConnectionError, RuntimeError) as e:
        print("FAILED")
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    current_version = info.get("version", "unknown")
    device_name = info.get("device", "unknown")
    print(f"OK")
    print(f"  Device : {device_name}")
    print(f"  Current: v{current_version}")
    print(f"  Binary : {args.bin} ({bin_size / 1024:.0f} KB)")
    print()

    # Upload
    print("Uploading firmware...", end=" ", flush=True)
    try:
        ok = upload_firmware(args.host, args.user, password, args.bin)
    except (PermissionError, RuntimeError) as e:
        print("FAILED")
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    if ok:
        print("OK")
        print("Device is restarting. Verify with:")
        print(f"  curl http://{args.host}/api/info")
    else:
        print("FAILED")
        print("ERROR: Device reported upload failure.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
