# thesada-fw code guidelines

How to write firmware in this repo. Reference material for PR review.
Sibling to [docs/invariants.md](docs/invariants.md) (the load-bearing
rules).

Dated 2026-05-13. Bump on every edit.

---

## Comments

### Default: no comment

Names do the talking. `bool isMTLSActive = ...` does not need a
comment.

### Comment only WHY, not WHAT

The code already shows what. Comments cover hidden constraints,
subtle invariants, workarounds for specific bugs, or behaviour that
would surprise a reader. If removing the comment would not confuse
the next reader, do not write it.

Good:
```cpp
// arduino-esp32 WiFiClientSecure retains the mbedtls context across
// failed connects; a subsequent setCertificate/setPrivateKey on a
// still-allocated context is silently ignored and the next handshake
// reuses stale config. stop() releases the context.
_wifiClient.stop();
```

Bad:
```cpp
// Stop the wifi client
_wifiClient.stop();
```

### Function header comments

Every public method (and large file-local helpers) gets a multi-line
header: what it does, inputs, outputs. The in/out lines are the
contract.

```cpp
// Validate a filesystem path before any LittleFS call. Centralised
// so every transport (Shell, HTTP, MQTT cli) shares one policy.
// Policy: leading '/' required, reject "..", reject "//", empty.
// in:  null-terminated path. out: true if safe.
static bool Shell::pathSafe(const char* path);
```

Big functions get split into helpers. Three similar lines is better
than a premature abstraction; a 200-line function with five
responsibilities is not.

### Do NOT comment

- WHAT the code does (names do that).
- The current task / fix / callers ("used by X", "added for Y flow") -
  these rot as the codebase evolves and belong in the commit body.
- TODOs without an owner.

---

## Logging

### Structured key=value, never interpolated

Every `Log::info` / `Log::warn` / `Log::error` call uses structured
fields, not `snprintf` into a freeform message.

Today many call sites still do snprintf; this is migration territory.
New code lands structured.

Target form (helper TBD):
```cpp
Log::kv(TAG, "cellular.state_change",
        "from", "ACTIVATING", "to", "ACTIVE", "elapsed_ms", elapsed);
```

Current form (acceptable until helper lands, mark with TODO):
```cpp
char msg[80];
snprintf(msg, sizeof(msg), "state_change from=%s to=%s elapsed_ms=%lu",
         "ACTIVATING", "ACTIVE", (unsigned long)elapsed);
Log::info(TAG, msg);
```

Event names are dotted: `subsystem.action`. Stable across releases.

### State machine transitions ALWAYS emit `<subsystem>.state_change`

Cellular (`STANDBY` -> `ACTIVATING` -> `ACTIVE`), MQTT reconnect, OTA
progress, alert delivery - any state machine emits a transition log
on every edge. At 3am with no debugger, the log is the only path.

### Level discipline

- `Log::debug` - tracing during development; opt-in via build flag.
- `Log::info` - normal lifecycle events.
- `Log::warn` - system continues but a human should know.
- `Log::error` - something failed; include diagnostic in fields.

Do not log raw key material, raw bearer tokens, or PEM bodies at
any level. Cert metadata (CN, serial, validity) is fine; the bytes
are not.

---

## Silent fallbacks: reject them

Every `setInsecure()`, every default value passed to a TLS / OTA
path, every "this probably never happens" branch: ask "is this
hiding a real condition?"

### Prefer loud failure with a clear error log

OTA used to fall back to `setInsecure()` when `/ca.crt` was missing.
SHA256 verification of the binary is NOT MITM protection because the
manifest comes over the same insecure HTTPS connection. The fix was
to refuse OTA bring-up entirely unless `ota.allow_insecure=true` is
set in config.

```cpp
if (_otaCaCert.isEmpty() && !allowInsecure) {
  Log::error(TAG, "OTA disabled - no /ca.crt and ota.allow_insecure not set");
  _enabled = false;
  return;
}
```

### Document any fallback you keep

If a fallback is the right choice, write the comment that explains
why. Otherwise the next reader will think it is sloppy and "improve"
it into a bug.

```cpp
// NTP not synced yet - cert validation will fail (clock at epoch =
// every cert looks expired). Temporary insecure mode; restore once
// NTP syncs in the background. Without this the device cannot reach
// any MQTT broker on cold boot before NTP fires.
_wifiClient.setInsecure();
_insecureFallback = true;
```

---

## Memory discipline

### Zero key material before `free()`

Every transient buffer that held a private key or other secret gets
`mbedtls_platform_zeroize(buf, len)` before the `free()` call.
`memset` is legal for the compiler to elide on a soon-to-be-freed
buffer; the mbedtls helper has a volatile pointer that defeats
elision.

```cpp
#include <mbedtls/platform_util.h>

mbedtls_platform_zeroize(keyBuf, maxLen);
free(keyBuf);
```

Persistent buffers held for the life of the TLS session (`_clientKey`
in `MQTTClient`) are NOT freed during normal operation; they live in
heap until reboot. That is intentional - `WiFiClientSecure` keeps the
raw pointer. The zero-then-free rule is for transient copies (cert.set
handler, modem upload buffers).

### Use `String::reserve` for known-size growth

Avoids reallocation churn on hot paths. Especially the dashboard
output assembly and MQTT response builders.

### LittleFS handles must be closed

Every `LittleFS.open()` pairs with a `f.close()` on every exit path.
Holding a file handle across a long async operation will leak descriptors.

---

## Shell deferred dispatch

### Any handler that runs a shell command uses `Shell::enqueue` or `Shell::enqueueDeferred`

Never `Shell::execute` from a callback that runs on the AsyncTCP
task, the PubSubClient callback task, or any FreeRTOS task with a
small stack. The deferred ring runs the command on the main-loop
task with full stack.

```cpp
// AsyncTCP onRequest handler - small stack, must defer
auto output = std::make_shared<String>("");
bool ok = Shell::enqueueDeferred([cmdStr, output]() {
  Shell::execute(cmdStr.c_str(), [output](const char* line) {
    *output += line;
  });
});
```

The exception: the serial reader's `Shell::pumpConsole` calls
`execute()` directly because it runs on the main loop already.

---

## Path safety

### All filesystem paths from external sources go through `Shell::pathSafe()` before any `LittleFS.open()`

Centralised in [lib/thesada-core/src/Shell.h](lib/thesada-core/src/Shell.h).
Policy: leading `/` required, reject `..`, reject `//`, reject empty.
Applies at every transport: HTTP, Shell over serial/WS, MQTT cli binary
handlers.

```cpp
if (!Shell::pathSafe(argv[1])) { out("Invalid path"); return; }
```

If you add a new handler that takes a path, the check is the first
line of the handler. No exceptions.

---

## Concurrency

### Config and EventBus are single-task only

Today every reader and writer is on the main-loop task. When a new
module runs on a dedicated FreeRTOS task (likely BLE), this breaks.
The fix at that point is a recursive mutex modelled on `ATGuard` in
`Cellular.cpp`. Until then, the headers document the constraint and
reviewers reject any new task-spawning module that calls into these
singletons.

### Cellular AT bus is protected by ATGuard

Recursive mutex around the SIM7080 serial line. Every AT exchange
takes the guard for the duration of `sendAT` / `waitResponse`.
`pause()` releases-sleeps-reacquires for long backoffs so other
tasks can preempt.

```cpp
ATGuard g(pdMS_TO_TICKS(2000));
if (!g.ok()) {
  Log::warn(TAG, "AT bus busy - retry next tick");
  return;
}
```

---

## Test naming

### Tests are documentation of the contract

Native unit tests live in `tests/` (Lua) and the PlatformIO `native`
env. Test names describe what the system promises.

Good:
```
test_pathSafe_RejectsParentTraversal
test_pathSafe_RejectsRelativePath
test_OtaCheck_RefusesWithoutCaCert
```

Bad:
```
test_thing
test_pathsafe1
```

### Security-sensitive code: write the contract test even when nothing is broken

Coverage gates target Shell input parsing, `pathSafe`, OTA verification,
mbedtls cert validation. Start floor 60 %, raise as test infrastructure
matures.

---

## When you find a bug, find its siblings

Before fixing one occurrence, grep for the same pattern across the
codebase.

Example: a path-traversal review found `_pathSafe()` was only applied
at one of three transports. Fixing only the HTTP path would have left
MQTT and Shell wide open. The fix was to centralise the policy + apply
at every transport in one commit.

Process:
1. Find the bug.
2. Read the surrounding code for the pattern that allowed it.
3. Grep the rest of the codebase for the same pattern.
4. Fix all instances in one PR (or document why some are different).
5. Update [docs/invariants.md](docs/invariants.md) in the SAME commit -
   add the pattern if missing, and bump the `Dated` line. See "Ledger
   discipline" below.

---

## Ledger discipline

### The invariants ledger is updated in the same commit that establishes the invariant

[docs/invariants.md](docs/invariants.md) lists load-bearing rules this
firmware relies on. Every commit that establishes a new invariant -
or relies on one not already there - updates the ledger in the same
commit. Not in a follow-up. Not in the next PR.

The documentation and the fix are the same deliverable. If the ledger
drifts behind the code, it stops being load-bearing: a reviewer six
months from now cannot tell which rules the code currently depends on
versus which ones got quietly removed.

How enforced:

- Pre-commit hook (`scripts/hooks/pre-commit`) refuses commits that
  touch load-bearing source paths (OTA, MQTT, Shell, Config, HTTP
  server, Cellular) unless `docs/invariants.md` is also staged. Bypass
  is explicit: `INVARIANT_OK=1 git commit ...` (audit-traceable).
- Install via `./scripts/hooks/install.sh` after clone.
- When a new file becomes load-bearing, add its path to the
  `SENSITIVE_REGEX` in `scripts/hooks/pre-commit`.

What counts as "establishes a new invariant":

- New silent fallback you're keeping (document why - see "Silent
  fallbacks" above).
- New security-sensitive code path (TLS, cert handling, path
  validation, deferred dispatch, key zeroing).
- New cross-module assumption that would break if violated
  (single-task readers, retained-message semantics, hash provenance).
- Refactor that moves an existing invariant to a new file or function -
  update the `Source:` pointer in the ledger.

What does not need a ledger entry (use the `INVARIANT_OK=1` bypass):

- Doc-only changes to source comments.
- Pure refactor that preserves all invariants in their existing files.
- New feature behind a flag that has no security/correctness coupling.

Date bump: every edit to `docs/invariants.md` updates the `Dated`
line to today's date. That line is the heartbeat - a stale date is
a signal the ledger has drifted.

---

## Board-specific code

### S3-only as of 2026-05-12

thesada-fw targets the ESP32-S3 family. Classic ESP32 (WROOM-32 /
CYD / WT32-ETH01) support lives in a separate fork. New code does
not need to compile on classic ESP32; existing classic-only paths
are being removed.

### Multi-board sdkconfig

Each board env has its own `sdkconfig.<env>` for IDF tuning (mbedtls
record buffer sizes, etc). Common defaults in `sdkconfig.defaults`.
Adding a new board env: write its sdkconfig + add to the deploy
manifest list + document in the README supported-boards table.

---

## OTA discipline

### Pull-based with SHA256 verification + signed CA

Manifest at known URL, JSON with `version` + `url` + `sha256`. Device
polls at `ota.check_interval_s`. Manifest must be served over HTTPS
verified by `/ca.crt`; refuse insecure unless `ota.allow_insecure=true`
in config.

### Rescue partitions exist for a reason

`esp32-owb-rescue` + `esp32-s3-debug-rescue` envs strip every
optional module so the binary is small enough to complete OTA on a
weak link. Use them when the full binary is failing mid-download.

### Never push a build to production OTA without flashing it locally first

Build + bench-flash before deploy is the only thing that catches a
binary that bricks the device on first boot.

---

## Stop-doings

### Stop treating "I know this works" as documentation

The implicit knowledge in your head is the single biggest gap
between "this code works" and "this code stays defensible." If
something would surprise a reader 6 months from now, comment it.

### Stop deferring small fixes when you're already in the file

When you touch a file, fix any open low-priority polish item that
lives in it. Leaves the codebase cleaner than you found it.

### Stop letting test coverage drift on security paths

`pathSafe`, OTA verification, cert validation - if you change these,
the test changes with them. Hard rule.

### Stop pushing without testing all supported boards

The supported-board matrix is small (S3-family). Smoke-flash each
target before tagging a release.

---

## What this list is not

- Not a style guide (clang-format covers style).
- Not architecture documentation
  ([docs/invariants.md](docs/invariants.md) covers load-bearing
  rules).
- Not a tutorial.

It is the set of habits that distinguish firmware that ships from
firmware that holds up. Read it on the way into a new file.

Related: [docs/invariants.md](docs/invariants.md).
