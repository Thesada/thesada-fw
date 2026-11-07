-- thesada-fw - tests/lua/test_rules_example.lua
-- examples/scripts/rules.lua.example against the temperature payload shape:
-- a probe with no reading arrives as { name, address, stale = true } with no
-- temp_c/temp, and must neither error nor hide the other probes.
-- SPDX-License-Identifier: GPL-3.0-only

local H = require("harness")
local M = require("mocks")

local EXAMPLE = SCRIPT_DIR .. "../../examples/scripts/rules.lua.example"

-- Older firmware sets these globals to their names, so the example must run
-- with them clobbered: only method syntax on strings works.
local function load_example()
  M.reset(); M.install()
  M.config = { mqtt = { topic_prefix = "test/node" }, temperature = { unit = "C" } }
  local env = setmetatable({ string = "string", table = "table", math = "math" }, { __index = _G })
  local chunk = assert(loadfile(EXAMPLE, "t", env))
  chunk()
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

H.test("rules example: two probes with one name keep their own streaks", function()
  load_example()
  local live = { name = "Outdoor", address = "28AA000000000009", temp_c = 1, temp = 1 }
  for _ = 1, 5 do
    M.fire_event("temperature", { sensors = { STALE_OUTDOOR, live } })
  end
  H.assert_eq(broadcasts_containing("Outdoor: probe not reading"), 1, "live twin must not clear the stale streak")
  H.assert_eq(broadcasts_containing("Outdoor: 1C - Freeze risk!"), 1, "stale twin must not clear the freeze streak")
end)

H.test("rules example: the MQTT alert is the JSON the app stores", function()
  load_example()
  local odd = { name = 'Out"door\\\n\1', address = "28AA00000000000A", temp_c = 1, temp = 1 }
  for _ = 1, 5 do
    M.fire_event("temperature", { sensors = { odd } })
  end
  local pubs = M.calls_to("MQTT.publish")
  H.assert_eq(#pubs, 1, "one alert publish")
  H.assert_eq(pubs[1].args[1], "test/node/alert")
  local raw = pubs[1].args[2]
  local alert = M.json_decode(raw)
  H.assert_true(type(alert) == "table", "payload must be JSON: " .. tostring(raw))
  H.assert_eq(alert.severity, "warn")
  H.assert_eq(alert.code, "freeze")
  H.assert_eq(alert.message, 'Out"door\\\n?: 1C - Freeze risk!')
  H.assert_contains(raw, "\\u0001", "control byte escaped")
end)

local FREEZE_1 = { name = "Outdoor", address = "28AA00000000000B", temp_c = 1, temp = 1 }

H.test("rules example: an alert cut off in the Telegram send has reached MQTT and holds its cooldown", function()
  load_example()
  Telegram.broadcast = function() error("script.budget_exceeded budget_ms=10000") end
  for _ = 1, 10 do
    pcall(M.fire_event, "temperature", { sensors = { FREEZE_1 } })
  end
  local pubs = M.calls_to("MQTT.publish")
  H.assert_eq(#pubs, 1, "one alert in two sustain windows")
  H.assert_contains(pubs[1].args[2], "Freeze risk!")
end)

H.test("rules example: a failed Telegram send re-alerts after sustain", function()
  load_example()
  local sent = 0
  Telegram.broadcast = function() sent = sent + 1 return false end
  for _ = 1, 10 do
    M.fire_event("temperature", { sensors = { FREEZE_1 } })
  end
  H.assert_eq(sent, 2, "a failed send must not start the cooldown")
  H.assert_eq(M.count("MQTT.publish"), 2)
end)

H.test("rules example: going stale ends a temperature streak", function()
  load_example()
  for _ = 1, 4 do
    M.fire_event("temperature", { sensors = { SUPPLY_50 } })
  end
  M.fire_event("temperature", { sensors = { { name = "House Supply", address = "28AA000000000002", stale = true } } })
  M.fire_event("temperature", { sensors = { SUPPLY_50 } })
  H.assert_eq(broadcasts_containing("Low temp, fire dying"), 0, "one low read after a stale gap must not alert")
end)
