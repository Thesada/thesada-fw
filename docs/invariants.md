# thesada-fw invariants

The load-bearing rules this firmware relies on. Every PR that touches a
listed area must keep these true. Violations require this file to be
updated with a justification, not silent landing.

Dated 2026-06-11. Bump the date on every edit.

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

### fs.* command routing goes through `Shell::resolveFS` / `Shell::stripPrefix`

Every `fs.*` Shell command and every MQTT cli binary handler that
opens a file must resolve the backing filesystem via
`Shell::resolveFS(path)` and translate the path via
`Shell::stripPrefix(path)` before the `fs::FS->open()` call. The
registry maps mount prefixes to `fs::FS*` pointers; SDModule
registers `/sd` -> `SD_MMC` (or `SD` in SPI mode) at the end of a
successful mount. Unknown prefixes fall through to LittleFS.

Hardcoded `LittleFS.open` is allow-listed only for paths the firmware
itself owns end-to-end: `/ca.crt`, `/config.json`, `/scripts/*.lua`.
Any new caller that accepts an externally supplied path must route
through the registry so SD-path support stays automatic.

Path validation via `Shell::pathSafe` runs BEFORE resolution - so
`/sd/../config.json` is rejected on the `..` rule regardless of which
filesystem the prefix would have routed to. Cross-FS escape via the
prefix is structurally impossible.

How enforced: core does not depend on the SD module (one-way: SD
module depends on core). Modules call `Shell::registerFS` in their
own `begin()` after mount; core never imports SD-mod headers.

Source: `lib/thesada-core/src/Shell.cpp::resolveFS`, `stripPrefix`,
`registerFS`; `lib/thesada-mod-sd/src/SDModule.cpp::begin` (caller);
`lib/thesada-core/src/MQTTClient.cpp::runCli` (consumer).

---

## TLS and OTA

### OTA refuses to bring up when `/ca.crt` is missing unless `ota.allow_insecure=true`

Refusal happens at both `OTAUpdate::begin()` (boot time) and
`OTAUpdate::check()` (manual trigger via `cli/ota.check` or MQTT
`cmd_topic`). The check() guard mirrors begin() so a triggered check
cannot bypass the boot-time refusal. SHA256 verification of the
binary against the manifest is NOT a substitute for cert verification:
a MITM controls both manifest and binary.

The `/ca.crt`-missing path itself is not silent: when LittleFS lacks
`/ca.crt`, both `OTAUpdate::loadCaCert` and `MQTTClient::begin` fall
back to the baked-in `OTA_CA_PROGMEM` bundle (ISRG X1+X2, DigiCert
RSA+G2+G3, USERTrust ECC). The refusal above only fires when both the
LittleFS file AND the PROGMEM bundle are empty (build misconfig).
This is a pure fallback - `/ca.crt` in LittleFS always overrides, so
the rotation path stays flash-based.

How enforced: any change to `OTAUpdate::configureSecureClient` must
keep both guards. Reviewers grep for new `setInsecure()` calls.

Source: `lib/thesada-core/src/OTAUpdate.cpp` `begin()`, `check()`,
`configureSecureClient()`, `loadCaCert()`; `lib/thesada-core/src/MQTTClient.cpp`
CA-load block; `lib/thesada-core/src/ota_ca_progmem.h`.

### Every `OTAUpdate::check()` exit emits exactly one `<prefix>/status/ota` record

`refused` (with `reason` = `no-transport` | `no-manifest-url` | `no-ca` |
`heap-low` | `manifest-fetch-failed`), `up-to-date`, `updating`, or
`failed`. Operators see the result of every check without needing serial.
Silent returns are a regression - any new bailout path in `check()` must
add a matching `publishOtaRefusal` (or other state) call.

How enforced: code review checks new early-return branches in `check()`.
Bench test: drive each path via `cli/ota.check` and watch `status/ota`.

Source: `lib/thesada-core/src/OTAUpdate.cpp` `check()` + `publishOtaRefusal()`.

### OTA downloaded binary integrity verified by SHA256 streamed during download

Incremental hash update during `Update.write`; mismatch -> `Update.abort()`
without flipping the boot partition. Inactive flash partition stays
invalidated cleanly.

Source: `lib/thesada-core/src/OTAUpdate.cpp` flashFromCallback path.

### OTA-over-cellular shares the WiFi cert-verification gate

The cellular OTA path (manifest fetch + binary fetch over the SIM7080
HTTPS stack) is gated by the same `/ca.crt`-present-or-allow-insecure
refusal as WiFi OTA. The check runs in `OTAUpdate::configureSecureClient`
before any transport is selected, so cellular cannot quietly bypass
the gate that was written for WiFi. `setInsecure()` on the cellular
HTTPS client is rejected under the same rule.

How enforced: when a new transport gets added to OTA, route it through
`configureSecureClient` rather than letting the transport hold its own
TLS config.

Source: `lib/thesada-core/src/OTAUpdate.cpp::configureSecureClient`,
`lib/thesada-mod-cellular/src/Cellular.cpp` HTTPS client wiring.

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

### Cert+key pair are validated together at cert.set

A client cert + key only reach NVS / the TLS stack if they are a
matching pair. `validateClientCertKey` parses both, then runs
`mbedtls_pk_check_pair(&crt.pk, &pk, ...)` against the cert's public
key before returning ok - the `crt` context is kept live through the
check for exactly that. A mismatched pair (cert A + key B) is rejected
at cert.set instead of surfacing later as an opaque TLS handshake
failure.

How enforced: every path that installs client mTLS material calls
`validateClientCertKey` first. The check is version-guarded -
`mbedtls_pk_check_pair` and `mbedtls_pk_parse_key` take RNG callback
args on mbedtls 3.x (pioarduino / IDF 5.x), the shorter forms on 2.x.

Source: `lib/thesada-core/src/MQTTClient.cpp::validateClientCertKey`.

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

### Long blocking inner loops in core modules pump the console between iterations

Any inner loop in `lib/thesada-core` or `lib/thesada-mod-cellular` that
can hold the main-loop task for more than ~1 s must call
`Shell::pumpConsole()` between iterations. Without it, typed serial
commands queue in the OS RX FIFO until the loop exits and the board
appears frozen during normal operation (WiFi association, NTP sync,
OTA download, cellular registration polling, inter-chunk pacing).

Pump call is a one-line drop-in: reentrant-safe (single console,
single static buffer), idempotent on partial-line state, and
dispatches whole lines via `Shell::execute()` which has its own ring.
Worst case a typed command runs inside the inner loop's stack frame
instead of the main one - acceptable for debug operations.

How enforced: every new blocking loop with a per-iteration delay
>= 100 ms in core / cellular modules adds the pump call. Reviewers
check this on PRs that introduce new wait loops. Async/state-machine
refactors of these modules are out of scope - the pump call is the
contract.

Source: `lib/thesada-core/src/Shell.cpp::pumpConsole` and call sites
in `WiFiManager.cpp`, `OTAUpdate.cpp`, `Cellular.cpp`, `main.cpp`.

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

### Cellular activation is incremental - one phase per loop tick

Cellular bring-up must not run as a single blocking call. `Cellular::
tickActivation()` advances at most one phase (`POWER_ON`, `SIM`,
`RADIO_CFG`, `REGISTER`, `BEARER`, `MQTT`) per call and returns
`PENDING`; `CellularModule` polls it each loop while ACTIVATING so
every other module loop keeps ticking between phases. Registration is
polled ~1 Hz, never as one 30-180 s blocking wait.

The whole cycle is bounded by `cellular.activation_timeout_ms`
(default 30 min): on expiry the modem is hardware-reset and
`tickActivation()` returns `FAILED`, dropping the module to STANDBY -
a wedged modem or bad broker can never pin the device in ACTIVATING.

Why: the old blocking `Cellular::begin()` froze MQTT keepalive,
sensors, Telegram and the shell for 30-120 s during a WiFi-to-cellular
failover, exactly when remote intervention matters most.

How enforced: `CellularModule` ACTIVATING calls only
`tickActivation()` - never a blocking bring-up. The synchronous
`networkConnect()` survives for the steady-state `loop()` recovery
path only.

Source: `lib/thesada-mod-cellular/src/Cellular.cpp::tickActivation`,
`lib/thesada-mod-cellular/src/CellularModule.cpp::loop`.

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

### `<prefix>/info config_hash` is sha256 of `/config.json` on-disk bytes

Not the re-serialized in-memory `Config`. The companion app's drift
detection compares this hash against the file it stores; hashing the
canonical in-memory form (key ordering, whitespace, numeric formatting
from `serializeJson`) produces a different value for identical
content and breaks drift detection silently.

How enforced: hash via `sha256File("/config.json")`, never
`serializeJson(Config::get(), ...)` into a hash. Any future
config-hash producer (e.g. a `cli/info` handler) must use the file.

Source: `lib/thesada-core/src/MQTTClient.cpp::publishDeviceInfo`.

### `cli/response` echoes caller-supplied `req_id` when payload is JSON

When the `cli/<cmd>` payload parses as a JSON object with a top-level
`req_id` (string or number), every response message published to
`cli/response` for that command carries the same `req_id` verbatim.
Multiple in-flight CLI commands share the single `cli/response` topic
and the broker delivers them in publish order; without correlation
the consumer cannot match a response to its request, and a response
arriving within milliseconds of the next request is mis-routed.

Non-JSON payloads (binary protocols: `fs.write`, `fs.append`,
`cert.set`; legacy plain-text args) have no `req_id` and the response
simply omits the field. `{"req_id": N}` with no other keys is a
no-arg invocation - the envelope is correlation metadata, not args.

How enforced: a single `deserializeJson` at the top of `runCli`; the
`JsonDocument` stays in scope so the variant remains valid at every
publish site. Every `resp["cmd"] = cmd` is paired with
`if (hasReqId) resp["req_id"] = reqId`.

Source: `lib/thesada-core/src/MQTTClient.cpp::runCli`.

### `cli/response` paginates oversized command output

The general shell path in `runCli` measures the running serialized
JSON size as it collects output lines; when the next line would
overflow the `_bufferOut` publish buffer it ships the current page
with `"more": true` and starts a fresh one. The final page carries
`"more": false`. Every page carries a 0-indexed `"page"` field.
Single-page output - the common case - is `page: 0, more: false`,
the same shape a consumer that ignores both fields already sees.

Without this, `serializeJson` silently truncates any command whose
output exceeds the buffer (`fs.ls` on a large SD directory, `help`,
module dumps) and the consumer receives clipped, unparseable JSON.

How enforced: the general path publishes only through the paginating
sink (`startPage` / `publishPage` lambdas in `runCli`). The
special-case handlers (`fs.write`, `fs.cat` chunked, `cert.set`)
publish single fixed-shape responses and are exempt - their output
is bounded by construction.

Source: `lib/thesada-core/src/MQTTClient.cpp::runCli`.

### Dashboard / shell HTML output is escaped via the browser's serializer

Dashboard XSS path uses `_escEl.textContent = s; return _escEl.innerHTML`
to escape any user-influenced string before it lands in HTML. Avoids
hand-rolling escape tables.

Source: `data/dashboard.html` `_esc` helper.

---

### Broker-exhaustion reboots are bounded - no perpetual reboot loop

A persistently failing broker (bad host/port/credentials) must not
reboot the device forever. After `mqtt.reboot_after_fails` failed
reconnects (default 30) `connect()` reboots, but only while an NVS
counter (`thesada-boot` namespace) is below `mqtt.max_exhaust_reboots`
(default 3). Once the budget is spent
the device sets `_rebootHalted`, stops rebooting, and stays alive -
locally reachable via serial/web - while still retrying MQTT at the
capped backoff. The counter clears on the first successful connect; a
streak older than 6 h ages out so unrelated outages never accumulate.

Why: a reboot loop leaves the device reachable only briefly each
~30 min cycle, making remote recovery from a config mistake nearly
impossible. The TLS-OOM fast-reboot (`MaxAllocHeap` low) is a separate
path and is intentionally not counted - it genuinely defragments.

How enforced: the `_retryCount >= 30` branch in `connect()` is gated by
`!_rebootHalted` and the `mqttRebootCount()` budget check. Any new
reboot trigger in this file must justify why it cannot loop.

Source: `lib/thesada-core/src/MQTTClient.cpp::connect`.

---

## Transport abstraction

### `net.*` shell commands reach cellular only through the `Net` provider hook

`net.ip` / `net.ping` / `net.ntp` / `net.http` live in `thesada-core`,
which must not depend on the optional cellular module. The cellular
module registers a `Net::CellularProvider` (function-pointer table) in
its `begin()`; the `net.*` commands consult `Net::cellular()` and fall
back to the modem path when WiFi is down. When no cellular module is
compiled in, `Net::cellular()` returns `nullptr` and the commands stay
WiFi-only with no dead code.

Same one-way-dependency rule as the `Shell::FSMount` registry: core
declares the hook, the module fills it, core never imports module
headers. Any new transport-aware shell command routes through the
provider rather than `#include`-ing `Cellular.h`.

Source: `lib/thesada-core/src/Net.h`, `Net.cpp`;
`lib/thesada-core/src/Shell.cpp` (`cmd_ifconfig`, `cmd_ping`, `cmd_ntp`,
`cmd_mqtt`, `cmd_http`);
`lib/thesada-mod-cellular/src/CellularModule.cpp::begin` (registrant).

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

## Module activation

### A compiled-in module stays dark unless its config gate allows it

Compile-time presence (`ENABLE_*`) only puts a module in the binary. It does
NOT run. `ModuleRegistry::beginAll()` is the single gate: before calling a
module's `begin()`, it reads `config[module->configKey()]["enabled"]`,
defaulting to `module->coreModule()`. A module that resolves to disabled never
has `begin()` or `loop()` called - it must not probe hardware, register
handlers, spawn tasks, or log.

Two tiers, differing only in the default applied when the key is absent:
- **Core** (`coreModule() == true`: CellularModule, PowerManager) default ON -
  an absent key means run. Only an explicit `enabled: false` disables.
- **Optional** (everything else) default OFF - an absent or `false` key means
  the module never inits. `enabled: true` is the only path to activation.

`configKey()` is the module's `/config.json` subtree (e.g. `SDModule` ->
`"sd"`), not `name()`. The gate is the ONLY place activation is decided:
modules must not re-check `enabled` inside their own `begin()` (that would
fork the default and reintroduce the scattered, inconsistent gating this
replaced).

How enforced: new modules inherit optional-OFF for free (the base
`coreModule()` returns false). A module that needs to default on overrides
`coreModule()`. `module.status` reports `disabled` for gated-off modules so
the runtime decision is observable. The test suite asserts the invariant
(`module.status` vs `config[key].enabled`) for every module.

Source: `lib/thesada-core/src/Module.h` (`configKey`, `coreModule`),
`lib/thesada-core/src/ModuleRegistry.cpp::beginAll`/`loopAll`/`enabled`,
`lib/thesada-core/src/Shell.cpp::cmd_module_status`.

### Core statics honour the same `<key>.enabled` gate from `main.cpp`

The always-compiled core statics (WiFiManager, MQTTClient, OTAUpdate,
HeartbeatLED) are not `ModuleRegistry` modules, so `setup()` applies the same
scheme directly: it reads `wifi`/`mqtt`/`ota`/`heartbeat` `.enabled` once after
`Config::load()`, all defaulting ON, and gates both the `begin()` and the
per-tick `loop()` call. The headline lever is `wifi.enabled: false`, which
skips WiFi bring-up so CellularModule becomes the active transport without
removing WiFi credentials.

Source: `src/main.cpp` (`_wifiEnabled`/`_mqttEnabled`/`_otaEnabled`/
`_heartbeatEnabled`, set in `setup()`, gating `setup()` and `loop()`).

### Shell is never gateable

The on-device recovery CLI (`Shell`) has no `enabled` gate at any tier. It is
hard-mandatory so a bad config can never lock out the serial/MQTT recovery
path. Reduced/headless command surface is a separate concern (`shell.mode`),
not an on/off switch.

Source: `src/main.cpp` (`Shell::begin()` called unconditionally).

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
