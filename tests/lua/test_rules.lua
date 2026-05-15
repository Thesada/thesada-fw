-- thesada-fw - tests/lua/test_rules.lua
-- Smoke tests for the canonical data/scripts/rules.lua (runs in the main
-- loop). The in-repo rules.lua is the minimal stub shipped with the
-- firmware image; per-device rules.lua scripts with real alert logic
-- (fire-streak hysteresis etc) live in the private infrastructure repo
-- and are tested there against this same mocks.lua / harness.lua pair -
-- see test_synthetic.lua for the worked event/timer/Telegram pattern.
-- SPDX-License-Identifier: GPL-3.0-only

local H = require("harness")
local M = require("mocks")

local RULES = SCRIPT_DIR .. "rules.lua"  -- SCRIPT_DIR is set by run_all.lua

H.test("rules.lua loads and runs without error", function()
  M.reset()
  M.install()
  H.run_script(RULES)
end)

H.test("rules.lua logs a run line", function()
  M.reset()
  M.install()
  H.run_script(RULES)
  H.assert_true(M.count("Log.info") >= 1, "rules.lua should log at least once")
end)

H.test("rules.lua is side-effect free in the stub form", function()
  M.reset()
  M.install()
  H.run_script(RULES)
  -- The shipped stub only logs - it must not reboot, publish, or send.
  H.assert_false(M.restarted, "rules.lua must not call Node.restart()")
  H.assert_eq(M.count("MQTT.publish"), 0, "stub rules.lua must not publish")
  H.assert_eq(M.count("Telegram.send"), 0, "stub rules.lua must not send Telegram")
end)

H.test("rules.lua is re-runnable (loop semantics)", function()
  M.reset()
  M.install()
  -- rules.lua runs every main-loop tick; running it repeatedly must not
  -- error or accumulate unexpected state.
  for _ = 1, 5 do
    H.run_script(RULES)
  end
  H.assert_true(M.count("Log.info") >= 5, "each run should log")
end)
