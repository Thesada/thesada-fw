-- thesada-fw - tests/lua/harness.lua
-- Tiny host-side test framework for the Lua script tests. Dependency-free
-- so the private infrastructure repo can require() it unchanged to test
-- its per-device scripts.
--
-- Usage (in a test_*.lua file):
--   local H = require("harness")
--   H.test("does the thing", function()
--     H.assert_eq(2 + 2, 4)
--   end)
--
-- run_all.lua requires every test_*.lua then calls H.run().
--
-- SPDX-License-Identifier: GPL-3.0-only

local H = {}

H._tests   = {}    -- ordered list of { name = , fn = }
H.passed   = 0
H.failed   = 0
H.failures = {}    -- list of { name = , err = }

-- ANSI colour, disabled when stdout is not a tty-ish environment. Kept
-- simple - CI captures plain text fine either way.
local GREEN, RED, DIM, RESET = "\27[92m", "\27[91m", "\27[2m", "\27[0m"

-- Register a test case. Body should raise (via the assert_* helpers or
-- error()) to fail.
function H.test(name, fn)
  H._tests[#H._tests + 1] = { name = name, fn = fn }
end

-- ── Assertions (all raise on failure, with caller line info) ────────────────

function H.assert_true(cond, msg)
  if not cond then
    error(msg or "expected truthy value", 2)
  end
end

function H.assert_false(cond, msg)
  if cond then
    error(msg or "expected falsy value", 2)
  end
end

function H.assert_eq(got, want, msg)
  if got ~= want then
    error(string.format("%sexpected [%s], got [%s]",
      msg and (msg .. ": ") or "", tostring(want), tostring(got)), 2)
  end
end

function H.assert_nil(got, msg)
  if got ~= nil then
    error(string.format("%sexpected nil, got [%s]",
      msg and (msg .. ": ") or "", tostring(got)), 2)
  end
end

-- Substring (plain, not pattern) match.
function H.assert_contains(haystack, needle, msg)
  if type(haystack) ~= "string" or not haystack:find(needle, 1, true) then
    error(string.format("%sexpected [%s] to contain [%s]",
      msg and (msg .. ": ") or "", tostring(haystack), tostring(needle)), 2)
  end
end

-- ── Script loading ──────────────────────────────────────────────────────────

-- Load and execute a firmware script (e.g. data/scripts/rules.lua) in the
-- current global environment - call M.install() from mocks.lua first so
-- the script's Log/Node/MQTT/... references resolve. Returns whatever the
-- script returns. Raises with context on a load or runtime error.
function H.run_script(path)
  local chunk, err = loadfile(path)
  if not chunk then
    error("loadfile " .. path .. ": " .. tostring(err), 2)
  end
  local ok, result = pcall(chunk)
  if not ok then
    error("runtime error in " .. path .. ": " .. tostring(result), 2)
  end
  return result
end

-- ── Runner ──────────────────────────────────────────────────────────────────

-- Run every registered test, print a per-test line + summary, and return
-- the failure count (0 = all passed). run_all.lua exits with this code.
function H.run()
  print("")
  for _, t in ipairs(H._tests) do
    local ok, err = pcall(t.fn)
    if ok then
      H.passed = H.passed + 1
      print(string.format("  %s[PASS]%s %s", GREEN, RESET, t.name))
    else
      H.failed = H.failed + 1
      H.failures[#H.failures + 1] = { name = t.name, err = err }
      print(string.format("  %s[FAIL]%s %s", RED, RESET, t.name))
      print(string.format("         %s%s%s", DIM, tostring(err), RESET))
    end
  end

  print("")
  print(string.rep("-", 56))
  print(string.format("  Lua tests: %s%d passed%s, %s%d failed%s",
    GREEN, H.passed, RESET,
    H.failed > 0 and RED or DIM, H.failed, RESET))
  print(string.rep("-", 56))
  return H.failed
end

return H
