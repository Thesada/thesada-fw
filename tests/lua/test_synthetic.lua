-- thesada-fw - tests/lua/test_synthetic.lua
-- Exercises the harness machinery (EventBus delivery, timer queue, MQTT
-- delivery, Config dot-walk, JSON.decode, Telegram capture) against small
-- synthetic scripts. Two jobs:
--   1. Regression-test mocks.lua itself.
--   2. Serve as the worked example for the private infrastructure repo,
--      whose per-device rules.lua carries the real alert logic (e.g. the
--      fire-streak hysteresis). That repo points its tests at this same
--      mocks.lua / harness.lua pair; this file is the copy-from template.
-- SPDX-License-Identifier: GPL-3.0-only

local H = require("harness")
local M = require("mocks")

-- Load a script given as a string in the mocked global env.
local function load_inline(src, name)
  local chunk, err = load(src, name or "inline", "t")
  H.assert_true(chunk ~= nil, "load failed: " .. tostring(err))
  chunk()
end

-- ── EventBus delivery ───────────────────────────────────────────────────────

H.test("EventBus.subscribe receives fired events", function()
  M.reset(); M.install()
  load_inline([[
    EventBus.subscribe("temperature", function(data)
      if data.celsius and data.celsius > 50 then
        Log.warn("hot: " .. data.celsius)
      end
    end)
  ]])
  M.fire_event("temperature", { celsius = 60 })
  M.fire_event("temperature", { celsius = 20 })
  H.assert_eq(M.count("Log.warn"), 1, "only the >50 reading should warn")
  H.assert_contains(M.last_call("Log.warn").args[1], "hot: 60")
end)

-- ── Fire-streak hysteresis (the canonical rules.lua pattern) ────────────────
-- Mirrors the suppression logic #164 calls out: only alert after N
-- consecutive low readings, and only once per streak.

local FIRE_STREAK_SCRIPT = [[
  local LOW_C   = 55
  local STREAK  = 3
  local lows    = 0
  local alerted = false
  EventBus.subscribe("temperature", function(data)
    local c = data.celsius or 999
    if c < LOW_C then
      lows = lows + 1
      if lows >= STREAK and not alerted then
        alerted = true
        Telegram.send("ops", "low-temp fire risk: " .. c .. "C")
      end
    else
      lows = 0
      alerted = false
    end
  end)
]]

H.test("fire-streak: no alert before 3 consecutive lows", function()
  M.reset(); M.install()
  load_inline(FIRE_STREAK_SCRIPT, "fire_streak")
  M.fire_event("temperature", { celsius = 40 })
  M.fire_event("temperature", { celsius = 40 })
  H.assert_eq(M.count("Telegram.send"), 0, "2 lows must not alert")
end)

H.test("fire-streak: alert fires on the 3rd consecutive low", function()
  M.reset(); M.install()
  load_inline(FIRE_STREAK_SCRIPT, "fire_streak")
  M.fire_event("temperature", { celsius = 40 })
  M.fire_event("temperature", { celsius = 38 })
  M.fire_event("temperature", { celsius = 35 })
  H.assert_eq(M.count("Telegram.send"), 1, "3rd low should alert")
  H.assert_contains(M.last_call("Telegram.send").args[2], "fire risk")
end)

H.test("fire-streak: alert is suppressed for the rest of the streak", function()
  M.reset(); M.install()
  load_inline(FIRE_STREAK_SCRIPT, "fire_streak")
  for _ = 1, 6 do
    M.fire_event("temperature", { celsius = 30 })
  end
  H.assert_eq(M.count("Telegram.send"), 1, "one alert per streak, not per reading")
end)

H.test("fire-streak: a normal reading resets the streak", function()
  M.reset(); M.install()
  load_inline(FIRE_STREAK_SCRIPT, "fire_streak")
  M.fire_event("temperature", { celsius = 30 })
  M.fire_event("temperature", { celsius = 30 })
  M.fire_event("temperature", { celsius = 70 })  -- reset
  M.fire_event("temperature", { celsius = 30 })
  M.fire_event("temperature", { celsius = 30 })
  H.assert_eq(M.count("Telegram.send"), 0, "interrupted streak must not alert")
  M.fire_event("temperature", { celsius = 30 })  -- now 3 in a row
  H.assert_eq(M.count("Telegram.send"), 1, "fresh streak alerts again")
end)

-- ── Node.setTimeout / timer queue ───────────────────────────────────────────

H.test("Node.setTimeout fires after the clock advances", function()
  M.reset(); M.install()
  load_inline([[
    Node.setTimeout(1000, function() MQTT.publish("t/done", "1") end)
  ]])
  M.advance(500)
  H.assert_eq(M.count("MQTT.publish"), 0, "timer must not fire early")
  M.advance(600)
  H.assert_eq(M.count("MQTT.publish"), 1, "timer should fire after 1000 ms")
end)

H.test("Node.setTimeout queue caps at 8 (matches firmware)", function()
  M.reset(); M.install()
  for _ = 1, 8 do
    H.assert_true(Node.setTimeout(100, function() end), "first 8 should queue")
  end
  H.assert_false(Node.setTimeout(100, function() end), "9th should be rejected")
end)

-- ── MQTT delivery ───────────────────────────────────────────────────────────

H.test("MQTT.subscribe receives delivered messages", function()
  M.reset(); M.install()
  load_inline([[
    MQTT.subscribe("cmd/relay", function(topic, payload)
      if payload == "on" then MQTT.publish("state/relay", "1") end
    end)
  ]])
  M.deliver_mqtt("cmd/relay", "on")
  H.assert_eq(M.count("MQTT.publish"), 1)
  H.assert_eq(M.last_call("MQTT.publish").args[2], "1")
end)

-- ── Config.get dot-walk ─────────────────────────────────────────────────────

H.test("Config.get walks dot-notation keys", function()
  M.reset(); M.install()
  M.config = { mqtt = { broker = "test.local", port = 8883 }, device = { name = "n1" } }
  H.assert_eq(Config.get("mqtt.broker"), "test.local")
  H.assert_eq(Config.get("mqtt.port"), 8883)
  H.assert_eq(Config.get("device.name"), "n1")
  H.assert_nil(Config.get("mqtt.missing"), "missing key returns nil")
  H.assert_nil(Config.get("nope.nope"), "missing branch returns nil")
end)

-- ── JSON.decode ─────────────────────────────────────────────────────────────

H.test("JSON.decode parses objects, arrays, scalars", function()
  M.reset(); M.install()
  local obj = JSON.decode('{"a":1,"b":"two","c":true,"d":[1,2,3]}')
  H.assert_eq(obj.a, 1)
  H.assert_eq(obj.b, "two")
  H.assert_eq(obj.c, true)
  H.assert_eq(obj.d[3], 3)
end)

H.test("JSON.decode returns nil on malformed input", function()
  M.reset(); M.install()
  H.assert_nil(JSON.decode("{not json"), "malformed JSON should decode to nil")
end)
