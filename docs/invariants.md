# thesada-fw invariants

The load-bearing rules this firmware relies on. Every PR that touches a
listed area must keep these true. Violations require this file to be
updated with a justification, not silent landing.

Dated 2026-05-12 (initial). Bump the date on every edit.

---

## Filesystem access

### All external file paths pass through `Shell::pathSafe()` before any `LittleFS.open()`

Policy: leading `/` required, reject `..`, reject `//`, reject empty.
Single source in `lib/thesada-core/src/Shell.cpp` (declaration in
`Shell.h`). HTTP `/api/file` delegates via the HttpServer-local
`_pathSafe` helper. MQTT binary handlers (`fs.write`, `fs.append`,
`fs.cat` chunked) and every Shell `fs.*` handler call it directly.

How enforced: every new handler that accepts a path must call
`Shell::pathSafe()` before the FS call. Reviewers grep for new
`LittleFS.open` outside the allow-listed locations.

Source: `lib/thesada-core/src/Shell.h`, `Shell.cpp`,
`lib/thesada-core/src/MQTTClient.cpp` (cli binary handlers),
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_pathSafe`).

---

## TLS and OTA

### OTA refuses to bring up when `/ca.crt` is missing unless `ota.allow_insecure=true`

Refusal happens at both `OTAUpdate::begin()` (boot time) and
`OTAUpdate::check()` (manual trigger via `cli/ota.check` or MQTT
`cmd_topic`). The check() guard mirrors begin() so a triggered check
cannot bypass the boot-time refusal. SHA256 verification of the
binary against the manifest is NOT a substitute for cert verification:
a MITM controls both manifest and binary.

How enforced: any change to `OTAUpdate::configureSecureClient` must
keep both guards. Reviewers grep for new `setInsecure()` calls.

Source: `lib/thesada-core/src/OTAUpdate.cpp` `begin()`, `check()`,
`configureSecureClient()`.

### OTA downloaded binary integrity verified by SHA256 streamed during download

Incremental hash update during `Update.write`; mismatch -> `Update.abort()`
without flipping the boot partition. Inactive flash partition stays
invalidated cleanly.

Source: `lib/thesada-core/src/OTAUpdate.cpp` flashFromCallback path.

---

## mTLS client identity

### Private key material in heap is zeroed before `free()`

Use `mbedtls_platform_zeroize(buf, len)` (not `memset` - the compiler
is allowed to elide a memset on a buffer about to be freed; the
mbedtls helper is explicitly volatile-pointer to defeat that). Applies
to every transient cert/key buffer: cellular cert upload to modem FS,
the cert.set MQTT CLI handler that writes per-device material to NVS.

Persistent buffers held for the life of the TLS session
(`_clientCert`, `_clientKey`) are NOT freed during normal operation;
they live in heap until reboot. This is intentional - `WiFiClientSecure`
holds the raw pointer for the lifetime of the session.

How enforced: any new code that loads a key into a heap buffer for
short-term use must zero+free. Add `#include <mbedtls/platform_util.h>`.

Source: `lib/thesada-mod-cellular/src/Cellular.cpp::writeClientCert`,
`lib/thesada-core/src/MQTTClient.cpp` cert.set handler.

### Cert+key pair are validated together at cert.set (WIP)

Target state. Currently cert and key are parsed independently; a
mismatched pair is caught later at TLS handshake. Aspirational
invariant: `mbedtls_pk_check_pair(&crt.pk, &pk)` runs before any
cert.set returns ok.

### mTLS context reset between connect attempts

`_wifiClient.stop()` before every reconnect releases the stale mbedtls
context. arduino-esp32 silently keeps a half-configured context across
failed handshakes; subsequent `setCACert` / `setCertificate` /
`setPrivateKey` calls are no-ops on a still-allocated context. Fix
is `stop()` first on every connect.

Source: `lib/thesada-core/src/MQTTClient.cpp::connect`.

---

## HTTP server

### `/api/cmd` never executes shell commands inside the AsyncTCP callback

`Shell::execute` is dispatched via `Shell::enqueueDeferred` to the
main-loop drain. The async-task stack is sized for WS frame dispatch;
any command that reaches LittleFS / MQTT / TLS / Lua can overflow it
from there. Response is held via shared_ptr until the ring drains or
a 5 s ceiling expires (503 if ring busy, 504 on timeout).

Same rule applies to WebSocket shell input.

How enforced: any new handler that runs a shell command must use
`Shell::enqueue` or `Shell::enqueueDeferred`, never `Shell::execute`
directly.

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp::cmdHandler`,
`lib/thesada-core/src/Shell.cpp::enqueueDeferred`.

### Bearer token comparison is constant-time (WIP)

Currently `strcmp`, which returns early on first byte mismatch and
leaks effective leading-byte count via timing. Tokens are 128-bit
`esp_random` so unexploitable in practice, but the right pattern is
to fix.

### HTTP rate limiter: 5 fails -> 30 s lockout, 16 IP table, auto-reset on success

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp::_rlCheck` /
`_rlReset`.

---

## Shell deferred-execution ring

### Slot std::function payloads are moved out under the lock, destructed outside

`Shell::loop()` moves `slot.sink` / `slot.fn` into locals inside the
portMUX critical section, then exits the section before the locals fall
out of scope. The captured heap state (std::string captures etc) is
freed on the main-loop task, not under the spinlock. The portMUX
disables interrupts; `free()` inside it can deadlock the ESP-IDF
allocator if it takes its internal lock.

Do NOT null `slot.sink` / `slot.fn` under the lock - they are already
in the moved-from (empty) state after `std::move`. Nulling would run
the previous payload's destructor inside the lock.

How enforced: any new SlotMode or new payload type must follow the
move-then-exit pattern.

Source: `lib/thesada-core/src/Shell.cpp::loop`.

---

## Concurrency

### Config and EventBus are single-task only

`Config::_doc` (JsonDocument) and `EventBus::_subscribers` (std::map)
have no locking. Today every reader and writer is on the main-loop
task: Shell handlers, MQTT callbacks, ModuleRegistry init - all run
inside `Shell::loop()` or the main `setup()` path. Cellular AT-bus
work is on its own task but does not touch Config / EventBus directly.

When a new module runs on a dedicated FreeRTOS task (likely BLE),
this invariant breaks. The fix at that point is a recursive mutex
modelled on `ATGuard` in `Cellular.cpp`. Until then, headers document
the constraint and reviewers reject any new task-spawning module that
calls into these singletons.

Source: `lib/thesada-core/src/Config.cpp`,
`lib/thesada-core/src/EventBus.cpp`,
`lib/thesada-mod-cellular/src/Cellular.cpp::ATGuard`.

### Cellular AT bus is protected by a recursive mutex with RAII guard

`ATGuard` is a recursive mutex wrapper around the SIM7080 AT
serial line. Every AT exchange takes the guard for the duration of
its `sendAT` / `waitResponse` pair so URC parsing and inbound
`+SMSUB` handling cannot interleave with command dispatch. The guard
has a `pause()` method that releases-sleeps-reacquires for long
backoffs so other tasks (Shell over serial, network timeout polls)
can preempt without starving.

Source: `lib/thesada-mod-cellular/src/Cellular.cpp::ATGuard`.

---

## Lua sandbox

### `os` library is explicitly minimal

`os.execute` and `os.exit` are not exposed. Lua scripts can read time
and date but cannot fork processes or call into syscalls. `pcall`
wraps every entry point so a script error never crashes the host
firmware.

Source: `lib/thesada-mod-scriptengine/src/ScriptEngine.cpp`
`sandbox` setup.

---

## MQTT

### Cellular MQTT subscriptions mirror the WiFi-side `MQTTClient` registry

`MQTTClient::subscribe` writes to a single subscription table. WiFi
session subscribes from this table on connect; `Cellular::smsubAll`
iterates the same table on cellular handoff. Adding a new subscribe
goes via `MQTTClient::subscribe` only - never call `smsub` directly.

Constraint: SIM7080 supports at most a handful of `+SMSUB` topics per
session. Current cap = 4 (`MAX_SUBS` in `Cellular.cpp`). Above the cap
extra subs run on WiFi only and are silently absent on cellular.

Source: `lib/thesada-core/src/MQTTClient.cpp::subscribe`,
`lib/thesada-mod-cellular/src/Cellular.cpp::smsubAll`.

### Dashboard / shell HTML output is escaped via the browser's serializer

Dashboard XSS path uses `_escEl.textContent = s; return _escEl.innerHTML`
to escape any user-influenced string before it lands in HTML. Avoids
hand-rolling escape tables.

Source: `data/dashboard.html` `_esc` helper.

---

## State machines must emit structured transitions

Aspirational. Every state-machine transition gets a key=value log
line so a reader can reconstruct a fault at 3am from the log without
reading the code.

Format: `Log::kv("subsystem.state_change", "from", old, "to", new,
"reason", why)`. The `Log::kv` helper is WIP.

Today the cellular state machine (STANDBY / ACTIVATING / ACTIVE)
emits per-transition lines but they are free text, not structured.

---

## What this list is not

- Not a feature spec.
- Not a coverage map.
- Not a roadmap.

It is the list of properties this firmware must keep true to remain
defensible. Reviewers consult it on every PR that touches the named
files. Update it before merging anything that violates an entry, or
the entry is wrong.

Related: [CODE-GUIDELINES.md](../CODE-GUIDELINES.md).
