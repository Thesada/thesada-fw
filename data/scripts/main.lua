-- thesada-fw - main.lua (canonical placeholder)
-- Runs once at boot. Generic stub - no device logic, core bindings only
-- (Log, Node). Real per-device scripts live in
-- infrastructure/devices/<name>/scripts/ and are flashed separately.
-- This file exists so the firmware image and the host-side Lua test
-- harness have a known-good default.
-- SPDX-License-Identifier: GPL-3.0-only

Log.info("main.lua loaded - firmware " .. Node.version())
