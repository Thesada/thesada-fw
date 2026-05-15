-- thesada-fw - tests/lua/run_all.lua
-- Host-side Lua test runner. Loads the harness, every test_*.lua module,
-- and runs them under lua5.3. Exits non-zero if any test fails so CI can
-- gate on it.
--
-- Run from the repo root:
--   lua5.3 tests/lua/run_all.lua
--
-- SPDX-License-Identifier: GPL-3.0-only

-- Resolve our own directory from arg[0] so the runner works regardless of
-- the caller's CWD. arg[0] is e.g. "tests/lua/run_all.lua".
local self_path = arg[0] or "tests/lua/run_all.lua"
local here      = self_path:match("^(.*)[/\\][^/\\]+$") or "."

-- Make harness.lua / mocks.lua / test_*.lua require()-able.
package.path = here .. "/?.lua;" .. package.path

-- Absolute-ish path to the canonical firmware scripts, exported as a
-- global the test_*.lua files read.
SCRIPT_DIR = here .. "/../../data/scripts/"

local H = require("harness")

-- Test modules, in run order. Add new test_*.lua files here.
local TEST_MODULES = {
  "test_main",
  "test_rules",
  "test_synthetic",
}

print("thesada-fw Lua test suite")
print(string.rep("=", 56))

for _, mod in ipairs(TEST_MODULES) do
  local ok, err = pcall(require, mod)
  if not ok then
    print("  [LOAD FAIL] " .. mod .. ": " .. tostring(err))
    H.failed = H.failed + 1
    H.failures[#H.failures + 1] = { name = mod .. " (load)", err = err }
  end
end

local failed = H.run()
os.exit(failed == 0 and 0 or 1)
