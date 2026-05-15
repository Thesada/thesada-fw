-- thesada-fw - tests/lua/test_main.lua
-- Smoke tests for the canonical data/scripts/main.lua (runs once at boot).
-- SPDX-License-Identifier: GPL-3.0-only

local H = require("harness")
local M = require("mocks")

local MAIN = SCRIPT_DIR .. "main.lua"  -- SCRIPT_DIR is set by run_all.lua

H.test("main.lua loads and runs without error", function()
  M.reset()
  M.install()
  M.version = "1.4.7-test"
  H.run_script(MAIN)
end)

H.test("main.lua queries the firmware version", function()
  M.reset()
  M.install()
  M.version = "1.4.7-test"
  H.run_script(MAIN)
  H.assert_true(M.count("Node.version") >= 1, "main.lua should call Node.version()")
end)

H.test("main.lua logs a boot line carrying the version", function()
  M.reset()
  M.install()
  M.version = "9.9.9-test"
  H.run_script(MAIN)
  local logs = M.calls_to("Log.info")
  H.assert_true(#logs >= 1, "main.lua should log at least once")
  local found = false
  for _, c in ipairs(logs) do
    if type(c.args[1]) == "string"
       and c.args[1]:find("main.lua", 1, true)
       and c.args[1]:find("9.9.9-test", 1, true) then
      found = true
    end
  end
  H.assert_true(found, "expected a Log.info line mentioning main.lua + the version")
end)

H.test("main.lua does not reboot or publish at boot", function()
  M.reset()
  M.install()
  H.run_script(MAIN)
  H.assert_false(M.restarted, "main.lua must not call Node.restart()")
  H.assert_eq(M.count("MQTT.publish"), 0, "main.lua must not publish at boot")
end)
