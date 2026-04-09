#!/usr/bin/env python3
# scripts/check_deps.py - thesada-fw dependency update checker
#
# Checks PlatformIO registry and GitHub for newer versions of all
# dependencies declared in platformio.ini. No extra packages needed
# (stdlib urllib only).
#
# Usage:
#   python scripts/check_deps.py
#
# Exit code 0 - all up to date or manual-only items
# Exit code 1 - at least one pinned dependency has an update available
#
# SPDX-License-Identifier: GPL-3.0-only

import sys
import re
import json
import urllib.request
import urllib.error
import urllib.parse

# ── ANSI colors ───────────────────────────────────────────────────────────────
GREEN  = "\033[92m"
YELLOW = "\033[93m"
RED    = "\033[91m"
CYAN   = "\033[96m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RESET  = "\033[0m"

# ── Dependency table ──────────────────────────────────────────────────────────
# (owner/name, pinned_version)  - owner/name must match PlatformIO registry slug
PIO_LIBS = [
    ("knolleary/PubSubClient",        "2.8"),
    ("bblanchon/ArduinoJson",         "7.4.3"),
    ("paulstoffregen/OneWire",        "2.3.8"),
    ("milesburton/DallasTemperature", "4.0.6"),
    ("vshymanskyy/TinyGSM",          "0.12.0"),
    ("adafruit/Adafruit ADS1X15",    "2.6.2"),
    ("olikraus/U8g2",                "2.35.30"),
    ("bodmer/TFT_eSPI",              "2.5.43"),
]

# GitHub-only libs (no pinned version - always tracks HEAD/latest release).
# We report the latest tag so the developer can decide whether to act.
GITHUB_LIBS = [
    "lewisxhe/XPowersLib",
    "mathieucarbou/ESPAsyncWebServer",
    "sfranzyshen/ESP-Arduino-Lua",
    "PaulStoffregen/XPT2046_Touchscreen",
]

# PlatformIO platform package
PIO_PLATFORM_OWNER = "platformio"
PIO_PLATFORM_NAME  = "espressif32"
PIO_PLATFORM_PIN   = "6.13.0"

# Vendored libs - never checked automatically
VENDORED = []


# ── HTTP helper ───────────────────────────────────────────────────────────────

def _get(url):
    try:
        req = urllib.request.Request(
            url, headers={"User-Agent": "thesada-fw-depcheck/1.0",
                          "Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read())
    except Exception:
        return None


# ── Version helpers ───────────────────────────────────────────────────────────

def _vtuple(v):
    """Convert '1.2.3' → (1, 2, 3). Non-numeric parts become 0."""
    parts = re.sub(r'[^0-9.]', '', str(v)).split(".")
    try:
        return tuple(int(x) for x in parts if x)
    except ValueError:
        return (0,)


def _newer(latest, pinned):
    """Return True if latest > pinned."""
    return _vtuple(latest) > _vtuple(pinned)


# ── Registry queries ──────────────────────────────────────────────────────────

def pio_lib_latest(owner_name):
    owner, name = owner_name.split("/", 1)
    slug = urllib.parse.quote(name, safe="")
    data = _get(f"https://api.registry.platformio.org/v3/packages/{owner}/library/{slug}")
    if isinstance(data, dict):
        ver = data.get("version")
        if isinstance(ver, dict):
            return ver.get("name")
        if isinstance(ver, str):
            return ver
    return None


def pio_platform_latest(owner, name):
    data = _get(f"https://api.registry.platformio.org/v3/packages/{owner}/platform/{name}")
    if isinstance(data, dict):
        ver = data.get("version")
        if isinstance(ver, dict):
            return ver.get("name")
        if isinstance(ver, str):
            return ver
    return None


def github_latest_tag(owner_repo):
    # Try releases first (semver tags), fall back to raw tags list
    data = _get(f"https://api.github.com/repos/{owner_repo}/releases/latest")
    if data and "tag_name" in data:
        return data["tag_name"].lstrip("v")
    data = _get(f"https://api.github.com/repos/{owner_repo}/tags")
    if isinstance(data, list) and data:
        return data[0]["name"].lstrip("v")
    return None


# ── Output ────────────────────────────────────────────────────────────────────

COL_NAME   = 42
COL_PINNED = 10

def _row(label, pinned, latest_str, status):
    if status == "ok":
        icon = f"{GREEN}[OK]    {RESET}"
    elif status == "update":
        icon = f"{YELLOW}[UPDATE]{RESET}"
        latest_str = f"{BOLD}{latest_str}{RESET}"
    elif status == "manual":
        icon = f"{CYAN}[MAN]   {RESET}"
    else:
        icon = f"{DIM}[?]     {RESET}"
    print(f"  {icon} {label:<{COL_NAME}} {pinned:<{COL_PINNED}} {latest_str}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    print(f"\n{BOLD}thesada-fw Dependency Check{RESET}")
    print("─" * 72)
    print(f"  {'':8} {'Package':<{COL_NAME}} {'Pinned':<{COL_PINNED}} Latest")
    print("─" * 72)

    updates = []

    # ── Platform ──
    latest = pio_platform_latest(PIO_PLATFORM_OWNER, PIO_PLATFORM_NAME)
    label  = f"[platform] {PIO_PLATFORM_OWNER}/{PIO_PLATFORM_NAME}"
    if latest is None:
        _row(label, PIO_PLATFORM_PIN, "?", "unknown")
    elif _newer(latest, PIO_PLATFORM_PIN):
        updates.append((label, PIO_PLATFORM_PIN, latest))
        _row(label, PIO_PLATFORM_PIN, latest, "update")
    else:
        _row(label, PIO_PLATFORM_PIN, latest, "ok")

    # ── PlatformIO registry libs ──
    for owner_name, pinned in PIO_LIBS:
        latest = pio_lib_latest(owner_name)
        label  = owner_name
        if latest is None:
            _row(label, pinned, "?", "unknown")
        elif _newer(latest, pinned):
            updates.append((label, pinned, latest))
            _row(label, pinned, latest, "update")
        else:
            _row(label, pinned, latest, "ok")

    # ── GitHub libs (unversioned) ──
    for owner_repo in GITHUB_LIBS:
        latest = github_latest_tag(owner_repo)
        _row(owner_repo, "git/HEAD", latest or "?", "manual")

    # ── Vendored ──
    for name, note in VENDORED:
        _row(f"[vendored] {name}", "patched", note, "manual")

    print("─" * 72)

    if updates:
        print(f"\n{YELLOW}{BOLD}Updates available - review changelogs before upgrading:{RESET}")
        for label, pinned, latest in updates:
            print(f"  {YELLOW}!{RESET} {label}  {DIM}{pinned} → {RESET}{BOLD}{latest}{RESET}")
        print(f"\n{DIM}Edit platformio.ini, run 'pio pkg update', build and test before committing.{RESET}")
        print()
        sys.exit(1)
    else:
        print(f"\n{GREEN}All pinned dependencies are up to date.{RESET}\n")
        sys.exit(0)


if __name__ == "__main__":
    main()
