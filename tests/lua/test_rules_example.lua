-- thesada-fw - tests/lua/test_rules_example.lua
-- examples/scripts/rules.lua.example against the temperature payload shape:
-- a probe with no reading arrives as { name, address, stale = true } with no
-- temp_c/temp, and must neither error nor hide the other probes.
-- SPDX-License-Identifier: GPL-3.0-only

local H = require("harness")
local M = require("mocks")

local EXAMPLE = SCRIPT_DIR .. "../../examples/scripts/rules.lua.example"

local function load_example()
  M.reset(); M.install()
  M.config = { mqtt = { topic_prefix = "test/node" }, temperature = { unit = "C" } }
  H.run_script(EXAMPLE)
end

local function broadcasts_containing(text)
  local n = 0
  for _, c in ipairs(M.calls_to("Telegram.broadcast")) do
    if tostring(c.args[1]):find(text, 1, true) then n = n + 1 end
  end
  return n
end

local STALE_OUTDOOR = { name = "Outdoor", address = "28AA000000000001", stale = true }
local SUPPLY_50     = { name = "House Supply", address = "28AA000000000002", temp_c = 50, temp = 50 }

H.test("rules example: a stale probe ahead of a live one does not stop its checks", function()
  load_example()
  for _ = 1, 5 do
    M.fire_event("temperature", { sensors = { STALE_OUTDOOR, SUPPLY_50 } })
  end
  H.assert_eq(broadcasts_containing("House Supply: 50C - Low temp, fire dying"), 1,
    "fire-dying alert behind a stale probe")
end)

H.test("rules example: a stale probe alerts only once sustained", function()
  load_example()
  for _ = 1, 4 do
    M.fire_event("temperature", { sensors = { STALE_OUTDOOR } })
  end
  H.assert_eq(broadcasts_containing("probe not reading"), 0, "4 stale reads must not alert")
  M.fire_event("temperature", { sensors = { STALE_OUTDOOR } })
  H.assert_eq(broadcasts_containing("Outdoor: probe not reading"), 1, "5th stale read alerts")
end)

H.test("rules example: a reading clears the stale streak", function()
  load_example()
  for _ = 1, 4 do
    M.fire_event("temperature", { sensors = { STALE_OUTDOOR } })
  end
  M.fire_event("temperature", { sensors = { { name = "Outdoor", address = "28AA000000000001", temp_c = 10, temp = 10 } } })
  for _ = 1, 4 do
    M.fire_event("temperature", { sensors = { STALE_OUTDOOR } })
  end
  H.assert_eq(broadcasts_containing("probe not reading"), 0, "streak restarts after a reading")
end)
