-- thesada-fw - tests/lua/test_display_example.lua
-- examples/scripts/display.lua.example against the temperature payload shape:
-- a stale probe arrives with no temp and must draw as "--", never as -127.
-- SPDX-License-Identifier: GPL-3.0-only

local H = require("harness")
local M = require("mocks")

local EXAMPLE = SCRIPT_DIR .. "../../examples/scripts/display.lua.example"
local REMOTE  = "test/owb"

-- Display binding is module-side (ENABLE_DISPLAY); only text() is inspected.
local function install_display()
  local noop = function() end
  Display = {
    ready = function() return true end,
    text  = function(x, y, s) M.calls[#M.calls + 1] = { api = "Display.text", args = { x, y, s } } end,
    font = noop, clear = noop, show = noop,
  }
end

-- Load the example, deliver one temperature payload, rotate to the temp page.
local function draw_temps(payload)
  M.reset(); M.install(); install_display()
  M.config = { display = { remote_prefix = REMOTE, page_interval_s = 3 }, temperature = { unit = "C" } }
  H.run_script(EXAMPLE)
  M.deliver_mqtt(REMOTE .. "/sensor/temperature", payload)
  M.advance(3000)  -- start: status page
  M.advance(3000)  -- rotate: temperature page
  local lines = {}
  for _, c in ipairs(M.calls_to("Display.text")) do lines[#lines + 1] = tostring(c.args[3]) end
  return table.concat(lines, "\n")
end

H.test("display example: a stale probe draws as --, not -127", function()
  local out = draw_temps('{"sensors":[{"name":"Outdoor","address":"28AA000000000001","stale":true},'
    .. '{"name":"Supply","address":"28AA000000000002","temp_c":50.5,"temp":50.5}]}')
  H.assert_contains(out, "Outdoor        --", "stale probe line")
  H.assert_contains(out, "Supply       50.5C", "live probe line")
  H.assert_nil(out:find("-127", 1, true), "no -127 sentinel on screen")
end)
