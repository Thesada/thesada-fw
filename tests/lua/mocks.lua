-- thesada-fw - tests/lua/mocks.lua
-- Host-side fakes for the firmware Lua bindings, so canonical scripts
-- (data/scripts/main.lua, rules.lua) can be exercised under plain
-- lua5.3 with no ESP hardware.
--
-- The mocked surface mirrors the C++ bindings in
-- lib/thesada-mod-scriptengine/src/ScriptEngine.cpp (Log / MQTT / Node /
-- Config / EventBus / JSON / os.remove) and the module-side bindings
-- (Telegram in lib/thesada-mod-telegram/src/TelegramModule.cpp). Keep
-- this file in sync when a binding's signature changes.
--
-- This module is intentionally dependency-free so the private
-- infrastructure repo can require() the exact same file to test its
-- per-device scripts against the same fakes.
--
-- SPDX-License-Identifier: GPL-3.0-only

local M = {}

-- ── Captured state ──────────────────────────────────────────────────────────
-- M.calls is a flat, ordered log of every binding call:
--   { api = "Log.info", args = { "some message" } }
M.calls       = {}
-- M.config is the fake config tree Config.get() walks (set by the test).
M.config      = {}
-- Firmware identity reported by Node.version().
M.version     = "0.0.0-test"
-- Fake millis() clock returned by Node.uptime(); advance with M.advance().
M.now_ms      = 0
-- Set true once Node.restart() is called.
M.restarted   = false

M._event_subs = {}   -- event name  -> list of callbacks
M._mqtt_subs  = {}    -- topic       -> list of callbacks
M._timers     = {}    -- list of { fire_at = ms, cb = fn }

local MAX_TIMERS = 8  -- matches MAX_TIMERS in ScriptEngine.cpp

-- ── Internal helpers ────────────────────────────────────────────────────────

local function record(api, ...)
  M.calls[#M.calls + 1] = { api = api, args = { ... } }
end

-- Walk a dot-notation key through M.config. Object keys index by string;
-- a numeric token indexes the Lua table directly (Lua tables are 1-based,
-- so a test that needs the firmware's 0-based JSON-array behaviour should
-- lay its fake config out 1-based). Returns nil when any step misses.
local function config_walk(key)
  local node = M.config
  for token in tostring(key):gmatch("[^.]+") do
    if type(node) ~= "table" then return nil end
    local num = tonumber(token)
    if num ~= nil then
      node = node[num]
    else
      node = node[token]
    end
    if node == nil then return nil end
  end
  return node
end

-- ── Minimal JSON decoder (for JSON.decode) ──────────────────────────────────
-- Recursive-descent, good enough for objects / arrays / strings / numbers /
-- bool / null. Returns nil on any parse error, matching the firmware
-- binding (deserializeJson failure -> nil).
local function json_decode(str)
  local i, n = 1, #str
  local parse_value

  local function skip_ws()
    while i <= n do
      local c = str:sub(i, i)
      if c == " " or c == "\t" or c == "\n" or c == "\r" then i = i + 1 else break end
    end
  end

  local function parse_string()
    -- assumes str:sub(i,i) == '"'
    i = i + 1
    local out = {}
    while i <= n do
      local c = str:sub(i, i)
      if c == '"' then
        i = i + 1
        return table.concat(out)
      elseif c == "\\" then
        local e = str:sub(i + 1, i + 1)
        local map = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/",
                      b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
        if map[e] then
          out[#out + 1] = map[e]; i = i + 2
        elseif e == "u" then
          -- collapse \uXXXX to '?' - the firmware does not emit unicode
          -- escapes in its own payloads, so faithful enough for tests.
          out[#out + 1] = "?"; i = i + 6
        else
          error("bad escape")
        end
      else
        out[#out + 1] = c; i = i + 1
      end
    end
    error("unterminated string")
  end

  local function parse_object()
    i = i + 1  -- consume '{'
    local obj = {}
    skip_ws()
    if str:sub(i, i) == "}" then i = i + 1; return obj end
    while true do
      skip_ws()
      if str:sub(i, i) ~= '"' then error("expected key") end
      local key = parse_string()
      skip_ws()
      if str:sub(i, i) ~= ":" then error("expected ':'") end
      i = i + 1
      obj[key] = parse_value()
      skip_ws()
      local c = str:sub(i, i)
      if c == "," then i = i + 1
      elseif c == "}" then i = i + 1; return obj
      else error("expected ',' or '}'") end
    end
  end

  local function parse_array()
    i = i + 1  -- consume '['
    local arr = {}
    skip_ws()
    if str:sub(i, i) == "]" then i = i + 1; return arr end
    while true do
      arr[#arr + 1] = parse_value()
      skip_ws()
      local c = str:sub(i, i)
      if c == "," then i = i + 1
      elseif c == "]" then i = i + 1; return arr
      else error("expected ',' or ']'") end
    end
  end

  parse_value = function()
    skip_ws()
    local c = str:sub(i, i)
    if c == "{" then return parse_object()
    elseif c == "[" then return parse_array()
    elseif c == '"' then return parse_string()
    elseif c == "t" and str:sub(i, i + 3) == "true"  then i = i + 4; return true
    elseif c == "f" and str:sub(i, i + 4) == "false" then i = i + 5; return false
    elseif c == "n" and str:sub(i, i + 3) == "null"  then i = i + 4; return nil
    else
      local num = str:match("^%-?%d+%.?%d*[eE]?[%+%-]?%d*", i)
      if num and #num > 0 then
        i = i + #num
        return tonumber(num)
      end
      error("unexpected token")
    end
  end

  local ok, result = pcall(function()
    local v = parse_value()
    skip_ws()
    if i <= n then error("trailing data") end
    return v
  end)
  if ok then return result end
  return nil
end

M.json_decode = json_decode  -- exposed for tests that want to build fixtures

-- ── Test-side controls ──────────────────────────────────────────────────────

-- Clear all captured state. Call this in setup before every test case.
function M.reset()
  M.calls       = {}
  M.config      = {}
  M.now_ms      = 0
  M.restarted   = false
  M._event_subs = {}
  M._mqtt_subs  = {}
  M._timers     = {}
end

-- All recorded calls to a given api name, in order.
function M.calls_to(api)
  local out = {}
  for _, c in ipairs(M.calls) do
    if c.api == api then out[#out + 1] = c end
  end
  return out
end

-- Number of times an api was called.
function M.count(api)
  return #M.calls_to(api)
end

-- The most recent call to an api, or nil.
function M.last_call(api)
  local all = M.calls_to(api)
  return all[#all]
end

-- Deliver a synthetic EventBus event to every subscriber registered for it.
function M.fire_event(name, data)
  for _, cb in ipairs(M._event_subs[name] or {}) do
    cb(data or {})
  end
end

-- Deliver a synthetic MQTT message to every subscriber on a topic.
function M.deliver_mqtt(topic, payload)
  for _, cb in ipairs(M._mqtt_subs[topic] or {}) do
    cb(topic, payload)
  end
end

-- Advance the fake clock by `ms` and fire any timers that come due.
function M.advance(ms)
  M.now_ms = M.now_ms + ms
  -- Fire in order; a fired timer is removed before its callback runs so a
  -- callback re-arming a timer does not re-fire within the same advance.
  local fired = true
  while fired do
    fired = false
    for idx, t in ipairs(M._timers) do
      if t.fire_at <= M.now_ms then
        table.remove(M._timers, idx)
        t.cb()
        fired = true
        break
      end
    end
  end
end

-- ── install(): set the firmware's global bindings ───────────────────────────
-- Call once before loading a script under test.
function M.install()
  Log = {
    info  = function(msg) record("Log.info",  msg) end,
    warn  = function(msg) record("Log.warn",  msg) end,
    error = function(msg) record("Log.error", msg) end,
  }

  MQTT = {
    publish = function(topic, payload)
      record("MQTT.publish", topic, payload)
    end,
    subscribe = function(topic, callback)
      record("MQTT.subscribe", topic)
      M._mqtt_subs[topic] = M._mqtt_subs[topic] or {}
      table.insert(M._mqtt_subs[topic], callback)
    end,
  }

  Node = {
    restart = function()
      record("Node.restart")
      M.restarted = true
    end,
    version    = function() record("Node.version"); return M.version end,
    uptime     = function() record("Node.uptime");  return M.now_ms end,
    ip         = function() record("Node.ip");      return "0.0.0.0" end,
    setTimeout = function(ms, callback)
      record("Node.setTimeout", ms)
      if #M._timers >= MAX_TIMERS then
        return false  -- queue full, matches firmware
      end
      table.insert(M._timers, { fire_at = M.now_ms + ms, cb = callback })
      return true
    end,
  }

  Config = {
    get = function(key)
      record("Config.get", key)
      return config_walk(key)
    end,
  }

  EventBus = {
    subscribe = function(event, callback)
      record("EventBus.subscribe", event)
      M._event_subs[event] = M._event_subs[event] or {}
      table.insert(M._event_subs[event], callback)
    end,
  }

  JSON = {
    decode = function(str)
      record("JSON.decode", str)
      return json_decode(str)
    end,
  }

  os = os or {}
  os.remove = function(path)
    record("os.remove", path)
    return true
  end

  -- Module-side bindings. Telegram is registered by the telegram module;
  -- mocked here so scripts that send alerts work under the harness.
  Telegram = {
    send = function(chat_id, msg)
      record("Telegram.send", chat_id, msg)
      return true
    end,
    broadcast = function(msg)
      record("Telegram.broadcast", msg)
      return true
    end,
  }
end

return M
