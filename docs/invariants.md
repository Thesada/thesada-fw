# thesada-fw invariants

The load-bearing rules this firmware relies on. Every PR that touches a
listed area must keep these true. Violations require this file to be
updated with a justification, not silent landing.

Dated 2026-09-06 (wildcard fs.rm needs --yes and never takes config.json
or ca.crt. Prior: certificate validity dates are not enforced by the
shipped mbedtls build. Prior: shell.mode gates all three command transports. Prior:
Basic auth is refused on cross-site state-changing requests, including the
two side-effect GETs, and the login lockout counts only real credential
guesses. Prior: Config.h carries the single-task note. Prior: OTA verification and cert/key structural decisions extracted
to `ota_verify_policy.h` and `cert_policy.h`, host-tested under a 95% coverage
floor; `certKeyInputsUsable` newly requires PEM structure on the mTLS install
path; the mbedtls pair check itself is unchanged and still untested. Prior:
MQTT CLI authorization gate; the mTLS verdict is bound
to the mTLS listener port; `cert.clear` recovery on a broken stored cert;
first-boot device identity; fallback AP refuses a default or absent
passphrase and the recovery window that leaves; LiteServer module
removed; the HTTP bind waits for a netif). Bump the date on every edit.

---

## Filesystem access

### All external file paths pass through `Shell::pathSafe()` before any `LittleFS.open()`

Policy: leading `/` required, reject `..`, reject `//`, reject empty.
Single source is the pure unit `pathSafePolicy()` in
`lib/thesada-core/src/path_safety_policy.h`; `Shell::pathSafe` is the
Arduino-side wrapper that every transport calls, and it does nothing but
delegate. HTTP `/api/file` delegates via the HttpServer-local
`_pathSafe` helper. MQTT binary handlers (`fs.write`, `fs.append`,
`fs.cat` chunked) and every Shell `fs.*` handler call it directly.

How enforced: `scripts/check-path-safety.sh`, run by the
`static-analysis` CI job. It fails the build on any
`LittleFS.open()` whose first argument is not a string literal unless
that exact call is in the script's `ALLOWED` list with a comment naming
the guard that makes it safe. Literal paths are safe by construction and
ignored, which keeps the list to the 5 dynamic sites rather than all 28
opens. Moving code does not break it; adding or changing a dynamic call
does. This replaces "reviewers grep for it", which is what let
`lua.load` ship without a check.

Known gap this closed: `lua.load` (`ScriptEngine.cpp`) opened `argv[1]`
with no `pathSafe()` call, reachable from serial, WS, HTTP and the MQTT
CLI - every other path-taking command had one. Guarded 2026-08-10.

Also unit-tested host-side: `test/test_path_safety/` covers traversal,
empty segments, relative paths and the deliberate false positive on a
filename containing `..`. Coverage floored in `scripts/coverage-floors.txt`.

Source: `lib/thesada-core/src/path_safety_policy.h`, `Shell.h`, `Shell.cpp`,
`lib/thesada-core/src/MQTTClient.cpp` (cli binary handlers),
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_pathSafe`),
`lib/thesada-mod-scriptengine/src/ScriptEngine.cpp` (`lua.load`),
`scripts/check-path-safety.sh`.

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
keep both guards. Reviewers grep for new `setInsecure()` calls. The
one sanctioned `setInsecure()` is the Telegram webhook client, and
only when no `/webhook-ca.crt` exists in LittleFS: the endpoint is an
arbitrary operator-chosen URL that cannot be pinned ahead of time.
Uploading `/webhook-ca.crt` (the endpoint's root, self-signed
included) switches it to verified TLS - same override pattern as
`/telegram-ca.crt` and `/ca.crt`.

Source: `lib/thesada-core/src/OTAUpdate.cpp` `begin()`, `check()`,
`configureSecureClient()`, `loadCaCert()`; `lib/thesada-core/src/MQTTClient.cpp`
CA-load block; `lib/thesada-core/src/ota_ca_progmem.h`. The refuse/insecure/
verified decision itself is `otaTlsMode()` in
`lib/thesada-core/src/ota_verify_policy.h` (host-tested).

### Every `OTAUpdate::check()` exit emits exactly one `<prefix>/status/ota` record

`refused` (with `reason` = `no-transport` | `no-manifest-url` | `no-ca` |
`heap-low` | `manifest-fetch-failed`), `up-to-date`, `updating`, or
`failed`. Operators see the result of every check without needing serial.
Silent returns are a regression - any new bailout path in `check()` must
add a matching `publishOtaRefusal` (or other state) call.

`publishOtaRefusal` must NOT short-circuit on `!connected()`:
`MQTTClient::publish` queues on the WiFi ring (or routes to the cellular
forwarder) while the broker is down, so a refusal raised mid-outage is
still delivered on reconnect. A `connected()` guard there silently drops
exactly the diagnostic an offline operator needs.

How enforced: code review checks new early-return branches in `check()`.
Bench test: drive each path via `cli/ota.check` and watch `status/ota`.

Source: `lib/thesada-core/src/OTAUpdate.cpp` `check()` + `publishOtaRefusal()`.

### OTA downloaded binary integrity verified by SHA256 streamed during download

Incremental hash update during `Update.write`; mismatch -> `Update.abort()`
without flipping the boot partition. Inactive flash partition stays
invalidated cleanly.

Source: `lib/thesada-core/src/OTAUpdate.cpp` flashFromCallback path. The digest
compare is `otaShaMatches()` in `lib/thesada-core/src/ota_verify_policy.h`, which
now gates digest length before comparing; the streaming `mbedtls_sha256`
accumulation stays in OTAUpdate.cpp and is not host-tested.

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

### MQTT TLS is always certificate-validated - no pre-NTP insecure window

`MQTTClient::connect()` has no `setInsecure()` fallback for an unsynced
clock. Instead, `begin()` applies a boot clock floor before the first
handshake: `settimeofday(max(persisted NVS floor, firmware build stamp))`
when the clock sits below it, so certificate validity checks pass without
NTP. The floor is re-persisted from `loop()` at most daily; the clock only
advances at real-time rate, so the stored value is always a valid lower
bound of real time - across reboots, NTP or not. A certificate the floored
clock rejects (e.g. `NotBefore` ahead of a stale floor) fails closed and
retries until NTP corrects. The clock is never moved backwards.

No-CA is also fail-closed, matching OTA. The CA comes from `/ca.crt` if
LittleFS has it, otherwise from the baked `OTA_CA_PROGMEM` bundle, which
is handed to `setCACert` **by pointer**: it is `.rodata` with static
lifetime, so that path allocates nothing and cannot fail on low heap.
That matters because it used to: both the `/ca.crt` read and the PROGMEM
copy went through `malloc`, and either one failing dropped through to
`setInsecure()`. A memory-pressured node therefore downgraded itself to
an unverified channel, and `runCli` has no auth of its own, so that
channel carries a shell. Fixed 2026-08-10.

With the bundle path infallible, the only way to reach no-CA is an empty
bundle, which is a build fault. So it is caught at build time by a
`static_assert` in `ota_ca_progmem.h`, and at runtime it needs an
explicit `mqtt.allow_insecure` opt-in, mirroring `ota.allow_insecure`.
Without the opt-in `begin()` sets `_tlsRefused` and `connect()` returns
early, so no unverified connection is ever established.

How enforced: decision logic is pure (`clock_floor_policy.h`,
host-tested in `test/test_clock_floor`). Do not reintroduce a
clock-conditional `setInsecure()` in the connect path. The one remaining
`setInsecure()` in MQTTClient is opt-in only. Ownership is in the types:
`_caHeap` is the owned buffer, `_caCert` is a `const char*` view that may
point at `.rodata`, so it must never be passed to `free()`.

Source: `lib/thesada-core/src/clock_floor_policy.h`,
`lib/thesada-core/src/MQTTClient.cpp` begin()/connect()/loop(),
`lib/thesada-core/src/ota_ca_progmem.h`.

---

## mTLS client identity

### The fallback AP refuses to start without a real passphrase

The soft AP is not a first-boot feature. `WiFiManager` raises it on any WiFi
failure for the device's entire service life, so it is a permanent surface, and
the portal behind it writes WiFi credentials. An open or publicly-keyed AP is
therefore an unauthenticated console, not a degraded fallback.

| Rule | Why |
|---|---|
| refuse to start when the passphrase is absent, under 8 chars, or the shipped placeholder | the placeholder is exactly 8 characters, so a length-only gate admits it and the AP comes up keyed by a value that is in the repository |
| never silently drop to an open AP | the previous behaviour did exactly this, and no log distinguished it from a protected one |
| SSID is built from `Identity::deviceId()`, not the operator label | the label ships as a fixed string, so every unit would broadcast the same SSID and a per-device join QR is ambiguous with two units in range |
| the passphrase is seeded per device at flash time and never rotates | it is the recovery token for a WiFi outage; rotating it would require the device reachable during the outage being recovered from |
| the passphrase is never derived from the MAC | the AP BSSID is the STA MAC plus one and is broadcast, so anything derived from it is computable by anyone in range |

Refusing is the fail-closed direction and it is deliberate: a unit whose
passphrase was never seeded must announce that loudly rather than come up
looking provisioned.

The refusal only works because there is a way to seed one.
`scripts/flash-provision.sh` is that way: it wraps `pio run -t upload`,
generates the passphrase itself, pushes it with `secret.set wifi.ap_password`
over the serial shell, and writes the join artifact to a gitignored 0600 file.
The passphrase never crosses argv or stdout, and NVS is write-only, so that
file is the only copy. A unit already holding one is left alone; `--force`
rotates.

Source: `lib/thesada-core/src/ap_policy.h`, `test/test_ap_policy/`,
`lib/thesada-core/src/WiFiManager.cpp::startFallbackAP`,
`scripts/flash-provision.sh`.

### A unit fielded without a seeded passphrase has one remote window left

The refusal above is the right default and it lands hardest on units flashed
before there was a way to seed one. Previously a missing `wifi.ap_password`
still raised an open AP, so an operator whose WiFi had changed could join the
fallback and re-enter credentials. That unit now raises nothing, and the WiFi
it was provisioned for is the only way to reach it.

| Way in | Requires | Open while |
|---|---|---|
| `secret.set wifi.ap_password <value>` over the MQTT cli | the unit still talking to the broker | the unit is online |
| `secret.set wifi.ap_password <value>` over the serial shell | the USB port, so physical access | always |
| `scripts/flash-provision.sh` | a serial port - it drives `DeviceShell(port)` | always |

Row one is the whole remote window. `secret.set` sits on the password row of
the cli authorization table above, so it reaches a device that never paired,
and `startFallbackAP` resolves the value on the next raise, so no restart is
needed. Once WiFi drops, rows two and three both mean somebody standing at
the hardware with the enclosure open.

Operational rule: seed `wifi.ap_password` while the unit is reachable, not
when the AP is wanted. `wifi.ap_refused` logs only at the moment the fallback
was needed, which is already the outage; `secret.info` reports presence ahead
of time but needs a cert session over MQTT.

Source: `lib/thesada-core/src/WiFiManager.cpp::startFallbackAP`,
`lib/thesada-core/src/Shell.cpp` (`secret.set`, `secret.info`),
`scripts/flash-provision.sh`.

### Device identity lives in its own NVS namespace and never leaves it

First boot derives `device_id` from the full six-byte factory MAC and mints
an Ed25519 keypair. Both live in `thesada-ident`, not `thesada-secrets`.

| Item | Rule |
|---|---|
| namespace | `thesada-ident` (15-char NVS limit, cannot grow) |
| private key | read only inside `Identity::sign`, zeroized before return |
| CLI reach | none - `secretNvsKeyFor` maps no field to this namespace, so no `secret.*` command can address it |
| `chip.info` | exposes `device_id` and the public key only |

Write order in `generate()` is secret key, public key, then id. `loadExisting()`
gates on the id, so a write interrupted midway reads back as absent and
regenerates rather than yielding a half identity. An all-zero key read is
treated as absent for the same reason.

The id uses all six MAC bytes, not a suffix: Espressif assigns sequentially,
so a short suffix collides across OUIs.

`Identity::nodeName()` is `device.name` when set, otherwise the generated id.
The shared literal `thesada-node` remains only while no identity exists in
NVS: a build that cannot mint (rescue), or a boot whose mint failed
(`identity.mac_read_failed` / `identity.sodium_init_failed` /
`identity.keygen_failed` / `identity.persist_failed`), stays on the literal
until the next successful mint. It is
unique per unit only while `device.name` is unset: the label ships as a fixed
string in the config image, so a fleet flashed from one image answers to one
name. The MQTT clientId reads it, and two units answering to one clientId evict
each other from the broker, so no broker path may run on the literal.
`identityBrokerNameUsable(device_id)` is the gate: `setup()` clears
`_mqttEnabled` (`boot.mqtt_disabled reason=node_name_not_unique`) and
`Cellular::mqttConnect` refuses before configuring the modem session
(`cellular.mqtt.refused`). The cellular recovery loop returns on that refusal
rather than retrying, because it cannot clear without a reboot. `Shell` still
starts either way - serial is the recovery route.

The gate reads the minted id only. A set `device.name` is not proof of a unique
unit: the label ships as a fixed string in the config image, so a fleet flashed
from one image would all pass while sharing one clientId. Two units given the
same label deliberately still collide - that remains the operator rule above,
not something the firmware can detect.

Home Assistant discovery does not read the node name. `MQTTClient::publishDiscovery`
and `SHT31Module::publishHaDiscovery` key `dev.ids`, `uniq_id` and the retained
`homeassistant/.../config` topic on `Identity::deviceId()`, so renaming a device
cannot orphan its entities; both skip discovery when the id is empty. Migration:
a unit that already published discovery under a `device.name` keeps those
retained topics until they are cleared broker-side.

The fallback AP name does not read it either: `startFallbackAP` calls
`apSsidFor(..., Identity::deviceId(), name)`, and `ap_policy.h` prefers the
device id, dropping back to the node name only when the id is empty.

`erase()` clears the in-RAM id and key only after `nvs_erase_all` AND
`nvs_commit` both return `ESP_OK`. A failed erase that dropped the RAM copy
would strand the unit on the fallback name while the old keypair is still on
flash.

The keypair exists to prove possession during claiming: the device signs a
challenge, and the holder of the public key can verify it. Claiming does not
go over MQTT - a factory-fresh device has no broker credential at all, so the
cert is delivered over the device's own access point and the device never
contacts the broker until it already holds one.

Caveat, flash encryption: with it enabled on S3 the NVS partition is bound to
the chip, so identity cannot be read out or transplanted - but a chip swap or
an eFuse key loss makes it unrecoverable, and the unit must be re-paired.
`ENABLE_IDENTITY` is off in rescue builds, which saves ~97 KB of libsodium.
The flag cuts minting only: `Identity::begin()` reads what NVS holds on every
image, rescue included, because a rescue image that could not read its own id
would fall back to the shared literal and re-create the clientId eviction on
the unbricking path. `Identity::canMint()` is false there, so `generate()` and
`sign()` are the parts that go away. `identity.reset` is registered on every
build and refuses at runtime instead: it checks `canMint()` and answers "this
build cannot mint - reset would leave no identity", because erasing into a
build that cannot re-mint drops the unit onto the shared fallback name.

Source: `lib/thesada-core/src/device_identity_policy.h`,
`lib/thesada-core/src/Identity.cpp`, `test/test_device_identity/`.

### MQTT CLI commands are authorized against the session's auth mode

`runCli` dispatches straight to `Shell`, so broker publish rights on a
device's topic tree mean command execution. The shared onboarding
credential sits in every device's flash, so it names no device.

| Session auth | Reaches |
|---|---|
| client cert, on the mTLS listener | full command surface |
| password (shared credential) | `cert.set`, `cert.apply`, `cert.info`, `secret.set`, `restart`, `version`, `chip.info`, `heap` |
| password, stored cert broken | the above plus `cert.clear` |
| password, `config.set` | key `mqtt.port` only |
| either, `config.set mqtt.port` | value must parse as 1..65535 |

Any valid port is writable from either row on purpose. Pairing writes
`8884` and recovery writes `8883`, both on the shared credential, so pinning
the value would break one of them. The control is the derived mode, not the
writable value: see the next invariant for what makes a session mTLS.

The value rule sits on both rows for a different reason: authorization is
asymmetric, validity is not. `config.set` stores what it is handed, and
`mqtt.port 8884}` saved as a string strands the device at next reload -
whoever sent it, and a paired device on its own cert is the one nobody can
reach afterwards. The password row is exactly what the pair and recovery
flows publish while a device is on the shared credential.

The mode is a property of the session that carried the command, and it is
frozen at dispatch, before the Shell deferred ring. Each transport owns its
own answer: the WiFi session's is `_mtlsActive`, recomputed from scratch on
every `connect()` attempt ahead of the early returns; the fallback
transport's arrives through `MQTTClient::setFallbackSessionMTLS` and is set
on every `Cellular::mqttConnect` outcome. One flag cannot serve both - the
two sessions hold different credentials and stay live at the same time, so a
device that failed over from WiFi mTLS to a cellular password session would
otherwise carry the mTLS verdict onto the password path and leave the gate
wide open. Password is the default in every ambiguous case.

Not device authentication. A leaked per-device cert still gets the full
surface - the CLI carries no signature and no replay protection.

`secret.set` stays on the password row because pairing pushes the whole
scalar secret set (`mqtt.password`, `telegram.bot_token`, `web.password`,
`wifi.ap_password`) plus per-SSID wifi passwords through it before mTLS is
live. The gate holds the field to exactly that provisioning set
(`cliAuthzSecretFieldAllowed`, backed by `secret_keymap.h`) - a containment
line, not a defence: a holder of the armed pairing credential can still
rewrite the web console and fallback-AP passphrases, because pairing must.
That residual is why the password listener retires with portal-based
enrollment rather than being hardened further.

`MQTT_TLS` undefined means no per-device identity exists, so the gate is
a no-op. That build is the local plaintext-broker escape hatch.

How enforced: `cliAuthzAllowed()` in `MQTTClient::runCli`, after envelope
unwrap, before every handler including the binary special cases. The mode
is a parameter, not a global read at drain time. Command names case-fold
exactly as `Shell::execute` dispatches them - a gate stricter than the
dispatcher in a different casing is how `CONFIG.SET` once slipped the
port-value rule. Serial and HTTP are
ungated by design - serial implies physical access, HTTP has
`web_auth_policy`.

Source: `lib/thesada-core/src/cli_authz_policy.h`, `test/test_cli_authz/`,
floor in `scripts/coverage-floors.txt`.

### A session only counts as mTLS when it dialled `MQTT_MTLS_PORT`

`_mtlsActive` used to mean "a client cert was found in NVS and parsed". That
is a statement about flash, not about the session. `mqtt.port` is the one
config key a password session may write, so such a session could point the
device at the broker's password listener, let it reconnect and authenticate
with the shared credential, and still be judged `CLI_AUTH_MTLS` because the
cert had loaded. Full command surface on a shared credential.

The verdict is now the conjunction of three terms:

| Term | Meaning |
|---|---|
| cert loads | both halves present in NVS and read back |
| cert validates | `validateClientCertKey` parses both and matches the pair |
| `_brokerPort == MQTT_MTLS_PORT` | the port last handed to `setServer`, which is the one this session dials |

`_brokerPort` is written at both `setServer` call sites (`begin()` and the
deferred reinit reconnect), never read out of `Config` at some other moment,
so a reconnect on a changed port re-evaluates the verdict along with
everything else `connect()` recomputes. On a non-matching port the client
material is not attached either, so the session really is password auth and
sends user/pass. The cellular session applies the same term to `wantMTLS`
before `setFallbackSessionMTLS`, taken from the port it hands to
`SMCONF="URL"`.

The check is positive and fail-closed: `== MQTT_MTLS_PORT`, never
`!= password_port`. Some deployments run a plaintext listener, and "not the
password port" would read a plaintext password session as mTLS.

What it guarantees: a password session cannot use the one key it may write to
promote itself to an mTLS session. What it does not: it is no proof that a
handshake actually presented the cert. It is the port the device dialled plus
a cert that parses, not a broker-confirmed identity.

`MQTT_MTLS_PORT` defaults to 8884 and is `#ifndef`-guarded so a build flag can
override it per env. The broker listener and whatever pairs devices against it
pin the same number; all of them move together or paired devices stop being
judged as paired.

Source: `src/thesada_config.h`, `lib/thesada-core/src/MQTTClient.cpp::connect`,
`lib/thesada-mod-cellular/src/Cellular.cpp::mqttConnect`.

### `cert.clear` opens to a password session only on a broken stored cert

The delete and recovery flow publishes `cert.clear` at a device that is on the
shared credential, and the password row denied it - the one path it exists
for. Putting it on the row unconditionally is worse than the bug: the shared
credential sits in every device's flash, so one holder could wipe certs
fleet-wide and push the whole fleet back onto the credential being retired.

The permission is conditional on a fact the firmware already computes:

| Stored cert state | `cert.clear` from a password session |
|---|---|
| loads and validates | denied |
| complete pair, will not load or will not validate | allowed |
| absent, or only one half stored | denied |

"Broken" is exactly the state that logs `mqtt.mtls_cert_invalid` or
`mqtt.mtls_load_failed`. A failed buffer allocation logs
`mqtt.mtls_load_oom` and leaves the last verdict standing - heap pressure
says nothing about the cert, and must not open `cert.clear` to a password
session while a good cert sits in NVS. Absent is deliberately not broken: there is nothing
to recover and nothing to clear, so the permission would buy the operator
nothing while widening what a shared-credential holder reaches. A half-stored
pair is the same call, and `cert.set` re-pushes the missing half from the
password row anyway. The flag drops back to false inside `clearClientCert()`
and on any successful `storeClientCert()`, so the permission ends with the cert
that granted it rather than outliving a repair until the next connect.

The fact is an explicit parameter of `cliAuthzAllowed`, not a global read from
inside the policy header, and it is a snapshot from the last `connect()`
attempt rather than re-derived per command - re-parsing a PEM pair on every
inbound CLI message is not worth the heap. Two consequences: a device whose
WiFi `connect()` has not run carries `false` and is denied on the cellular
session, which is the fail-closed direction; and a cert the broker revoked
still parses locally, so it is not broken here and `cert.clear` stays
mTLS-only for it.

Source: `lib/thesada-core/src/cli_authz_policy.h::cliAuthzPasswordCmdAllowed`,
`lib/thesada-core/src/MQTTClient.cpp` (`_storedCertBroken`),
`test/test_cli_authz/`.

### Private key material in heap is zeroed before `free()`

Use `mbedtls_platform_zeroize(buf, len)` (not `memset` - the compiler
is allowed to elide a memset on a buffer about to be freed; the
mbedtls helper is explicitly volatile-pointer to defeat that). Applies
to every transient cert/key buffer: cellular cert upload to modem FS,
the cert.set MQTT CLI handler that writes per-device material to NVS,
the secret.set MQTT CLI handler that writes per-device secrets to NVS.

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

Source: `lib/thesada-core/src/MQTTClient.cpp::validateClientCertKey`. It is now
fronted by `certKeyInputsUsable()` in `lib/thesada-core/src/cert_policy.h`, a
structural PEM pre-flight that is strictly more rejecting than the previous
non-null/non-empty guard. **The pair check itself is unchanged and remains
untested** - `cert_policy.h` covers structure and CN extraction only, not
`mbedtls_pk_check_pair`.

### mTLS context reset between connect attempts

`_wifiClient.stop()` before every reconnect releases the stale mbedtls
context. arduino-esp32 silently keeps a half-configured context across
failed handshakes; subsequent `setCACert` / `setCertificate` /
`setPrivateKey` calls are no-ops on a still-allocated context. Fix
is `stop()` first on every connect.

Source: `lib/thesada-core/src/MQTTClient.cpp::connect`.

---

## HTTP server

### The server binds only once a netif exists

`server.begin()` opens an lwIP socket, and lwIP is initialised by the WiFi
start, not by us. With `wifi.enabled: false` nothing ever starts it, so the
bind hits `assert failed: tcpip_api_call ... (Invalid mbox)` and panics.

That panic lands inside `ModuleRegistry::beginAll()`, which is still `setup()`.
`Shell::pumpConsole()` only runs from `loop()`, so the shell is registered and
never pumped: the unit boot-loops with no serial way in, and reflashing is the
only recovery. This is why the bind is guarded rather than left to fail loudly.

| State | Behaviour |
|---|---|
| WiFi STA up | binds in `begin()` |
| Fallback AP raised later | `begin()` defers, `loop()` binds when the AP netif appears |
| `wifi.enabled: false` | `web.server_deferred reason=no_netif`, never binds, boot completes |

Routes and the log handler are registered either way; only the bind waits.
`web.server_started` in the log is the proof it bound - its absence next to a
`registry.module_init ... HttpServer` line means the assert fired.

How enforced: nothing may call an lwIP or socket API from `begin()` without
checking `WiFi.getMode() != WIFI_MODE_NULL`. Cellular does not help here - the
SIM7080 does modem-native TCP over AT and never creates an lwIP netif.

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp::begin` / `::loop`.

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

### Default/empty `web.password` serves no authenticated route

While `web.password` resolves to the shipped default (`changeme`),
empty, or missing, `_checkAuth` refuses everything - Basic auth with
the default credentials AND Bearer tokens, so a token minted before a
password reset cannot outlive the reset. Public routes (dashboard,
`/api/info`, `/api/state`) stay up; `/api/auth/check` answers 403 with
an explanation so the login modal can say why. The veto lifts the
moment `secret.set web.password` lands - credentials are resolved per
request, no restart needed.

How enforced: the veto is the pure predicate `webAuthPassIsDefault` /
`webAuthAllowed` (`web_auth_policy.h`, host-tested in
`test/test_web_auth`). New auth schemes must route through
`webAuthAllowed`, never around it.

Source: `lib/thesada-core/src/web_auth_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp::_checkAuth`.

### Basic auth is refused on a cross-site state-changing request

Bearer tokens are read from a header the page must set, so a foreign
page cannot mint one. Basic credentials are cached by the browser and
replayed automatically, so a malicious LAN page can auto-submit a form
at the device and reach `POST /api/restart` or `DELETE /api/file` with
the operator's own credentials. When `Sec-Fetch-Site: cross-site`
arrives on a state-changing request, Basic no longer counts; Bearer
still does. Requests with no `Sec-Fetch-Site` at all - curl,
`tests/test_firmware.py`, pre-2020 browsers - are untouched, which is
the deliberate limit of the mitigation: it closes the browser-replay
path, not scripted access.

State-changing is the method OR a route that declares a side effect,
because two GETs have one. `GET /api/ws/token` mints a 30 s IP-bound
WS grant and `/ws/serial` reaches `Shell::enqueue`, so a cross-site
call there is worse than any POST: WebSocket handshakes are not
CORS-gated, so the attacker page opens the socket itself. `GET
/api/auth/check` answers whether cached credentials are valid for this
device, which is an enumeration oracle, and moves the rate-limit
counter. Both pass `hasSideEffect=true`.

How enforced: `webAuthBasicAllowed` / `webAuthMethodChangesState`
(`web_auth_policy.h`, host-tested in `test/test_web_auth`), fed into
the `basicOk` term of `_checkAuth`. An unknown or missing method counts
as state-changing, so a new verb is refused rather than waved through.
New routes inherit this for free - they must keep going through
`_checkAuth`, never call `req->authenticate` directly. A new GET that
changes state must pass `hasSideEffect=true`; the method cannot tell.

The login rate limiter counts only a real guess (`webAuthCountsAsGuess`):
a wrong credential that was actually offered. A cross-site refusal never
reached the password check, and an anonymous request attempted nothing,
so neither is counted - otherwise a foreign page could lock the operator
out of their own device with five requests it cannot even read.

Source: `lib/thesada-core/src/web_auth_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp::_checkAuth`.

### Certificate validity dates are not enforced, at any clock value

`notBefore` and `notAfter` are never checked on this firmware. Not
"skipped when the clock is floored" - the check is compiled out
entirely, so an expired or not-yet-valid certificate is accepted on
every TLS client the device opens: the OTA manifest and binary fetch,
and the MQTT broker connection, both through `WiFiClientSecure`.

The chain: `CONFIG_MBEDTLS_HAVE_TIME_DATE` is unset in the sdkconfig of
every arduino-esp32 variant we ship against; `mbedtls/esp_config.h`
turns that into `#undef MBEDTLS_HAVE_TIME_DATE`; and in
`x509_crt.c` the `MBEDTLS_X509_BADCERT_EXPIRED` and
`..._BADCERT_FUTURE` flags are only ever set inside
`#if defined(MBEDTLS_HAVE_TIME_DATE)`. Upstream's own
`mbedtls_config.h` does define it - IDF overrides that through
`MBEDTLS_CONFIG_FILE`, and arduino-esp32 ships mbedtls precompiled, so
the sdkconfig is what governs. This is the IDF default, not a change
we made.

What still holds: the chain of trust to the pinned CA (`setCACert`,
`otaTlsMode` -> `OTA_TLS_VERIFIED` whenever `/ca.crt` is present) and
the OTA payload SHA256 from the manifest. What does not: expiry, and
with no CRL or OCSP either, certificate rotation is an operational
control rather than something the fleet enforces.

Do not write code that treats an expiry date as a security boundary,
and do not "fix" this by enabling the option: `CLOCK_FLOOR_SANE_EPOCH`
is 2023-11-14, so a device that has not reached NTP would read every
certificate issued since then as not-yet-valid and refuse both OTA and
MQTT. Any real fix is gated on a confirmed NTP sync and must fail open
while unsynced.

Source: `lib/thesada-core/src/ota_verify_policy.h::otaTlsMode`,
`lib/thesada-core/src/OTAUpdate.cpp::configureSecureClient`,
`lib/thesada-core/src/MQTTClient.cpp` (`_wifiClient`),
`lib/thesada-core/src/clock_floor_policy.h::CLOCK_FLOOR_SANE_EPOCH`.
### A wildcard `fs.rm` is gated and cannot take the two boot-critical files

`fs.rm` accepts `*` and `?` in the last path segment only - a wildcard
earlier in the path is refused rather than treated as a literal, which
would delete a different set than the operator typed. A wildcard remove
additionally requires `--yes`, and reports removed/failed/protected
counts rather than going quiet.

`/config.json` and `/ca.crt` at the LittleFS root are skipped by any
wildcard, whatever the pattern: `fs.rm /* --yes` clears the root but
leaves the device bootable and still able to verify TLS. An exact
`fs.rm /config.json` still works - the veto is about a pattern reaching
them by accident, not about making them undeletable.

How enforced: `globMatch` / `globSplit` / `globRmProtected`
(`glob_policy.h`, host-tested in `test/test_glob` under a 95% floor).
`cmd_rm` routes to the glob path only when `globHasWildcard` says so, so
the exact-path behaviour is unchanged. The matcher is iterative with a
single backtrack point - no recursion, because this runs on the shell's
4 KB stack.

Source: `lib/thesada-core/src/glob_policy.h`,
`lib/thesada-core/src/Shell.cpp` (`cmd_rm`, `_rmGlob`, `cmd_ls`).

### Auth-state TTLs compare rollover-safe, never `now < expiry`

Bearer-token expiry, the 30 s WS pre-auth window, and the login
rate-limit lockout are persistent auth state gated on `millis()`. A
plain `now < expiry` flips its answer at the ~49.7 d wrap: expired
entries spring back to life (stale WS grants, dead tokens) and
lockouts re-open or extend (F4). All such compares go through the
signed-subtraction helpers in `ttl_policy.h`; oldest-slot eviction
compares remaining time, never raw expiry values, which sort wrongly
across the wrap. Transient sub-minute `while (millis() < deadline)`
poll loops (cellular AT, OTA pacing, shell drains) are exempt - worst
case there is one early timeout at the wrap, self-healing on retry.

How enforced: the helpers are pure and host-tested
(`test/test_ttl_policy`, including both wrap directions). New
`millis()`-based auth state must use `ttlActive` / `ttlReached` /
`ttlRemaining`; reviewers grep new auth code for raw expiry compares.

Source: `lib/thesada-core/src/ttl_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_rlAllow`,
`_createToken`, `_validateToken`, `_grantWsAuth`, `_consumeWsAuth`).

### OTA upload chunks are auth-gated before any `Update` call

ESPAsyncWebServer invokes the `/ota` upload callback for every
multipart chunk BEFORE the onRequest handler runs, so an auth check in
onRequest alone fires only after the attacker's image is already
staged and the boot partition switched. The upload callback therefore
checks `_checkAuth` itself at chunk 0 and records the verdict in
`req->_tempObject` (heap marker, freed by the request destructor;
NULL = unauthorized); no chunk touches `Update.begin/write/end` without
it. The rule is about callback ordering rather than one server: any
upload API that streams chunks before a completion handler runs must
authenticate at the first chunk, not at the end.

How enforced: both upload callbacks bail before their first `Update`
call when the auth marker is absent. Any new firmware-accepting
endpoint (HTTP or otherwise) authenticates before the first byte
reaches `Update`, not in a completion handler.

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp` (`/ota` upload
lambda).

### Bearer token comparison is constant-time

`_validateToken` compares via `_constTimeEq` (no early return) and
walks every token slot regardless of an early match, so neither the
per-byte compare nor the slot loop leaks token correctness via
response timing.

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp::_validateToken`.

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

## Serial console output

### All serial console writes go through `Console` under one mutex

Both `Log::write` and the `Shell` serial sink (`pumpConsole`) emit
through `Console`, which holds a single output mutex. Concurrent writes
from different tasks (an async log vs a command response) can no longer
byte-interleave into one mangled line. Boot prints in `main.cpp` use
`Log::` for the same path.

How enforced: no raw `Serial.print*` for console output - logs go
through `Log::`, command responses through `Console::reply`.

Source: `lib/thesada-core/src/Console.cpp`, `Console.h`, `Log.cpp`,
`Shell.cpp::pumpConsole`, `src/main.cpp`.

### Command mode frames each response with a sequence marker

`console.mode command` keeps async logs off the serial console (the ring
buffer + remote/WS handler still receive them) and ends every command
response with `\x04CMD-DONE <seq>`. Automated readers rely on the marker
plus the monotonic seq to delimit responses and discard a late one from a
timed-out command. Mode resets to normal on reboot and never gates the
recovery CLI.

`_mode` and `_seq` are read and written only under the Console output
mutex (`log`, `endReply`, `setMode`, `mode`). A mode flip on the
main-loop task cannot race a concurrent `Log::write` from another task:
the gate decision (suppress-in-command-mode) and the frame marker emit
are atomic with the write they guard, so a log line can never splice
into a command frame. `endReply` takes the mutex directly rather than
calling `writeLocked` - the mutex is non-recursive.

Source: `lib/thesada-core/src/Console.cpp::endReply`,
`Shell.cpp::pumpConsole` / `cmd_console_mode`.

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
modelled on `ATGuard` in `Cellular.cpp`. Until then, both headers
(`Config.h`, `EventBus.h`) carry the constraint above the class, and
reviewers reject any new task-spawning module that calls into these
singletons.

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

### Modem FS chunk uploads are bounded - no unbounded retry on the AT bus

The CFSWFILE chunk loops (`writeCACert`, `writeModemFile`) abort after
3 consecutive failed chunks, and a missing DOWNLOAD prompt is a hard
failure (the payload is never written blind after a prompt timeout).
Both paths CFSTERM and return false so the caller re-walks the state
machine. Unbounded, a wedged modem FS session spun these loops forever
while holding the AT-bus mutex - and because `waitChunked` feeds the
TWDT internally, the watchdog never fired and the activation deadline
(evaluated between `tickActivation` calls) never got the chance to:
the whole main loop hung permanently with no recovery path.

How enforced: every retry loop around a modem AT exchange carries a
bounded failure counter and a hard-fail return; feeding the TWDT inside
a loop obliges the loop to bound itself.

Source: `lib/thesada-mod-cellular/src/Cellular.cpp::writeCACert`,
`writeModemFile`.

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

### CLI responses publish outside the command wildcard

The device subscribes to `<prefix>/cli/#` for commands and publishes every
response to `<prefix>/cli_response`, which that wildcard cannot match. The
pre-split topic `<prefix>/cli/response` did match, so the broker forwarded
every response the device had just published back to it; over cellular that
echo arrives as a URC larger than the modem line buffer and is dropped with
an overflow warning, wasting AT-bus cycles and risking a real concurrent URC
in the same window.

The firmware publishes on the new topic only - it does not dual-publish. The
platform reads both, so it must be deployed before firmware carrying this
change or those devices go mute on CLI.

How enforced: every CLI topic is built by `lib/thesada-core/src/cli_topics.h`;
no topic literal is assembled at a call site. `test_cli_topics` asserts the
response topic does not match the input subscription, and that command topics
still do. Every builder reports truncation and callers must check it - a
truncated subscription kills CLI, and a truncated input prefix matches short
and slices the wrong command out of the topic.

Source: `lib/thesada-core/src/cli_topics.h`,
`lib/thesada-core/src/MQTTClient.cpp::publishCliResponse`.

### `cli_response` echoes caller-supplied `req_id` when payload is JSON

When the `cli/<cmd>` payload parses as a JSON object with a top-level
`req_id` (string or number), every response message published to
`cli_response` for that command carries the same `req_id` verbatim.
Multiple in-flight CLI commands share the single `cli_response` topic
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

### `cli_response` paginates oversized command output

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

### The shell command line in `runCli` is never silently truncated

The general path builds the shell input as
`snprintf(line, sizeof(line), "%s %s", cmd, payload)` into a fixed
`line[1024]`. The `snprintf` return is captured in `lineN`; if it is
negative or `>= sizeof(line)` the line would be clipped, so `runCli`
publishes an error response (`ok: false`, `"Command line too long for
shell - use chunked variant"`) and bails via `cleanup` instead of
dispatching a truncated command string to the shell.

Without this, an oversized cmd+payload is silently cut to malformed
shell input and executed. Today's binary handlers (`fs.write`,
`cert.set`) intercept before this path, but any future CLI command
carrying large JSON args would be bitten. This is the input-side
counterpart to the output-side pagination invariant above.

How enforced: both `snprintf` branches store into `lineN`; the length
guard runs before the command reaches `Shell::enqueueDeferred`.

Source: `lib/thesada-core/src/MQTTClient.cpp::runCli`.

### The offline publish queue rejects oversized messages, never truncates

`enqueue()` refuses a topic or payload that does not fit the
`MQTTMessage` buffers (topic 64, payload 256 - sized to cover the alert
serialization buffer) and logs a warn. A clipped payload would be
flushed to the platform after reconnect as malformed JSON - and the
queue holds precisely the alerts that fire during connectivity trouble,
so silent truncation corrupted the highest-value messages first. Same
reject-over-truncate policy as the `fs.write`/`config.set` guards.

Source: `lib/thesada-core/src/MQTTClient.cpp::enqueue`,
`lib/thesada-core/src/MQTTClient.h::MQTTMessage`.

### Meshtastic frame bounds checks are wrap-proof

`dataDecode` compares an attacker-controlled length varint against the
remaining bytes (`l > len - i`, with `i <= len` held at every site) -
never the additive form `i + l > len`, which wraps 32-bit `size_t` on
the ESP32 (l = 0xFFFFFFFF, i = 5 -> passes) and hands the consumer a
~4 GB `plen` over a 240-byte radio buffer; the text-copy loop then
walks off the stack and panics. Reachable by anyone with a LoRa radio
on the well-known default channel key, so this parser is a security
surface, not a convenience.

How enforced: host-unit-tested with crafted 5-byte 0xFFFFFFFF length
varints on both the payload and unknown-field-skip paths
(`test/test_meshtastic_frame::test_data_decode_rejects_overflowing_length`).
New length checks in wire parsers use the remaining-bytes form.

Source: `lib/thesada-core/src/meshtastic_frame.h::dataDecode`.

### `Config::set` rejects a dot-path it cannot store, never truncates it

`config.set` (serial and MQTT cli) copies the dot-path into a fixed
`buf[128]`. An empty or over-long path is rejected up front
(`return false`) instead of being truncated: a silent truncation would
resolve to a *different* key than the operator named and write the
value there, corrupting config from a remote command. Config-write
counterpart to the runCli line-length and pagination guards above.

How enforced: the `!path || !*path || !value || strlen(path) >=
sizeof(buf)` guard runs before the `strncpy` into `buf` (a null `value`
would also deref in the `strcmp`/`strtod` coercion below). Any new
fixed-buffer copy of an operator-supplied key adds the same length
check. The MQTT binary handlers apply the same rule: `fs.write` rejects
a path that would not fit `path[64]` and `fs.cat` rejects args longer
than `pbuf[256]`, publishing an error rather than acting on a clipped
path/range. `secret.set` and `cert.set` likewise reject an over-long
`<field>` / `<type>` before the `\n` rather than clipping it - a clipped
length makes the value/PEM length too large and reads past the payload
end.

Source: `lib/thesada-core/src/Config.cpp::set`,
`lib/thesada-core/src/MQTTClient.cpp::runCli` (`fs.write`, `fs.cat`),
`lib/thesada-core/src/cli_payload.h::cliSplitFieldValue` (`secret.set`,
`cert.set`, host-unit-tested in `test/test_cli_payload`).

### `Config::load` leaves a clean doc on a malformed `/config.json`

A deserialize failure clears `_doc` and logs, rather than leaving it
half-parsed. `Config::get()` then returns an empty object and every
`cfg[...] | default` read falls back to its default - a corrupt config
file degrades to defaults, never to garbage values read out of a
partially-populated document.

Source: `lib/thesada-core/src/Config.cpp::load`.

### `Config::set` / `replace` never report success on a failed persist

`Config::save()` returns `false` when `/config.json` cannot be opened
or the serialize comes up short of `measureJson` (LittleFS full,
truncated write). `set()` returns `false` and skips its success log;
`replace()` logs an error instead of "replaced". A config write that
did not hit flash must never surface to the operator as applied -
otherwise a full filesystem silently discards every change while the
device reports success.

`set()` also rolls the in-memory `_doc` back (via `load()`) when
`save()` fails, so the mutation it applied before persisting cannot
linger: a rejected set leaves both flash and `_doc` at the last
persisted state, never a value that was never written.

How enforced: every caller that persists config checks the `save()`
return before logging or returning success; a failed persist in a
mutate-then-save path reloads the persisted state.

Source: `lib/thesada-core/src/Config.cpp::save`, `set`, `replace`.

### `/config.json` is never truncated in place - every writer goes tmp + rename

Opening `/config.json` with `"w"` truncates the last good config before
the new bytes are known to fit; a short write (LittleFS full, flash
error) then leaves partial JSON on disk, and the `load()` rollback path
parses that corpse and wipes the live in-memory config too - persistent
loss of WiFi/MQTT/prefix from one full filesystem. Every config writer
therefore serializes to `/config.json.tmp`, verifies the written length,
and `LittleFS.rename()`s over the original (lfs rename atomically
replaces the destination). On any failure the tmp is removed and the
original is untouched.

How enforced: the writers are `Config::save()` and `Shell.cpp::
shellConfigWrite` (config.set / config.del / config.save). Any new code
path that persists config routes through one of these, never a direct
`LittleFS.open("/config.json", "w")`. This applies to the provisioning
portal too: it writes WiFi credentials through `Config::save()`.

Source: `lib/thesada-core/src/Config.cpp::save`,
`lib/thesada-core/src/Shell.cpp::shellConfigWrite`.

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
impossible.

The TLS-OOM fast-reboot (3 fails + `MaxAllocHeap` < 40 KB) consumes the
SAME budget: heap-constrained boards sit under 40 KB in steady state,
so uncounted it rebooted every ~15 s through any plain broker outage -
exactly the loop this invariant exists to prevent. It additionally
fires only on `MQTT_CONNECT_FAILED` (TCP/TLS never came up); an rc >= 1
means the broker answered, the heap was sufficient, and a defrag fixes
nothing.

How enforced: BOTH reboot branches in `connect()` are gated by
`!_rebootHalted` and the `mqttRebootCount()` budget check. Any new
reboot trigger in this file must justify why it cannot loop.

Source: `lib/thesada-core/src/MQTTClient.cpp::connect`.

### A critical mqtt config that never connects rolls back to the last-good snapshot

The connection-critical keys (`broker`, `port`, `user`, `password`)
snapshot to NVS (`thesada-boot`, `mqtt_lg`) on every successful connect.
When an exhaustion reboot fires, the config that is failing is recorded to
`mqtt_rb_cfg`. At boot `rollbackIfUncommitted()` restores the snapshot only
when the still-current critical config equals `mqtt_rb_cfg` (this exact
config rebooted without connecting) AND differs from `mqtt_lg`. Keying on
the recorded failing candidate - not merely a nonzero reboot counter - is
what keeps two cases from rolling back: a merely-offline broker (config
unchanged, equals `mqtt_lg`), and a user's recovery edit made after an
unrelated offline streak (config differs from the recorded candidate).
`mqtt_rb_cfg` is cleared on the next successful connect and after a
rollback. Boot-time only, mirrors OTA commit / pending-verify.

How enforced: the decision lives in the pure `mqttRollbackShould()` predicate
(host-unit-tested in `test/test_rollback`); `rollbackIfUncommitted()` only does
the NVS I/O around it and runs before the first `connect()` in `main.cpp`.

Source: `lib/thesada-core/src/mqtt_rollback_policy.h::mqttRollbackShould`,
`MQTTClient.cpp::snapshotGoodConfig`, `rollbackIfUncommitted`;
`src/main.cpp` boot call.

---

## Device secrets

### Secret fields resolve NVS -> config.json -> empty, and NVS never reaches the cli bus

The secret fields (each `wifi.networks[].password`, `mqtt.password`,
`telegram.bot_token`, `web.password`, `wifi.ap_password`) resolve through
`Secret::resolve` at every read site: the `thesada-secrets` NVS namespace
first, the config.json plaintext second, empty last. NVS is not reachable
via `config.dump` or `fs.cat` (both LittleFS-only), so a platform-managed
device provisions the value into NVS and blanks the config.json field -
`config.dump` then leaks nothing on `cli_response`. A standalone firmware
user with no platform keeps the config.json plaintext path; that fallback
is permanent, not a deprecation.

Provisioned via the `secret.set` MQTT cli handler (binary payload
`<field>\n<value>`, mirrors cert.set) and the serial `secret.set` /
`secret.clear`. `secret.info` reports presence only and never echoes a
stored value (write-only contract). Per-network wifi passwords key off a
short SSID hash so the NVS key fits the 15-char limit.

`Secret` uses the raw IDF `nvs` API, not Arduino `Preferences`: a read-only
open of an absent namespace, or a get of an absent key, is the normal
standalone path and must not ERROR-log on every connect/scan/auth.
`Preferences` logs those under the un-maskable `ARDUINO` tag; the raw nvs
calls return `ESP_ERR_NVS_NOT_FOUND` silently.

`Secret::set` rejects a value `>= Secret::MAX_LEN` and every read-site
buffer is sized to `MAX_LEN`, so any value `set()` accepts fits `resolve()`.
An undersized read buffer would make `nvs_get_str` fail and `resolve()`
silently fall back to config.json, breaking NVS-wins. `web.password` is
resolved per request in `HttpServer::_checkAuth`, not captured at route
registration, so `secret.set web.password` takes effect without a restart.

How enforced: any new plaintext-secret read site routes through
`Secret::resolve` with a `MAX_LEN` buffer; any new provisioning path zeroes
the transient value buffer (see the mTLS-zeroize invariant). `secret.info`
must never print a value.

Source: `lib/thesada-core/src/Secret.h`, `Secret.cpp`;
`lib/thesada-core/src/MQTTClient.cpp` (connect read site + secret.set
handler); `lib/thesada-core/src/Shell.cpp` (`secret.*`);
`lib/thesada-core/src/WiFiManager.cpp`,
`lib/thesada-mod-telegram/src/TelegramModule.cpp`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp`.

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

Every state-machine transition emits a structured key=value event so a
reader can reconstruct a fault at 3am from the log without reading the
code.

Format: `<subsystem>.state_change from=<old> to=<new> reason=<why>`
(reason only where a real cause exists), emitted via the printf-style
`Log::kvf` / `kvfw` / `kvfe` helpers (info/warn/error). The formatting
core is pure (`log_kv_policy.h`, host-tested in `test/test_log_kv`):
a line past `LOG_LINE_LEN` truncates NUL-terminated, never overruns.

Wired today: CellularModule policy (STANDBY/ACTIVATING/ACTIVE),
Cellular MQTT session flags (`cellular.mqtt.state_change`), MQTTClient
connect/disconnect (`mqtt.state_change`, broker host only - never
credentials), WiFiManager (CONNECTED/SCANNING/ALL_FAILED), OTA phases
(`ota.phase_change`: idle/check/fetch_manifest/download/verify/apply
plus failure exits with `reason=`), LoRa init->ready. New state
machines and transition writes follow the same convention.

Coverage: as of 2026-08-20 every Log::info/warn/error call site in
src/ and lib/ emits a dotted `module.event key=value` line (prefixes:
boot, wifi, mqtt, ota, cellular, web (HttpServer),
telegram, script, lora, temp, gnss, sd, config, identity, sht31, pwm,
ads, battery, power, sleep, shell, sensors, registry, heartbeat).
Deliberate free-text exceptions: the Lua `Log.*` script bindings
(relay user-authored text), the `Sensors` JSON state dump in
HttpServer::printState, raw AT/serial traces inside
`THESADA_CELL_DEBUG` blocks, and debug-level lines (no kv helper at
DEBUG; those format the same grammar via snprintf). Never log
secrets/credentials/tokens - hosts, ports, and client/chat IDs are
fine. New log lines follow the same grammar.

Source: `lib/thesada-core/src/Log.h`, `Log.cpp`, `log_kv_policy.h`;
call sites in `CellularModule.cpp`, `Cellular.cpp`, `MQTTClient.cpp`,
`WiFiManager.cpp`, `OTAUpdate.cpp`, `LoRaModule.cpp`.

---

## Module activation

### A module is only compiled in if its library is listed in `lib_deps`

`ENABLE_*` decides whether a module's code survives the preprocessor, but only
after PlatformIO has decided to compile the file at all. It compiles a local
library under `lib/` only when that library is named in `platformio.ini`
`lib_deps`. A directory that is missing from that list never enters the
dependency graph, never produces an object file, and never links - so its
`MODULE_REGISTER` never runs and its `ENABLE_*` flag does nothing whatsoever.

The failure is silent in both directions a reader would check. The build
succeeds, because a library that is never built cannot fail to build, so the
whole env matrix stays green. And the module is simply absent at runtime rather
than disabled, so `module.status` does not list it either. Someone follows the
README, uncomments the flag, flashes, and gets no behaviour and nothing to
debug.

How enforced: `scripts/check-lib-deps.sh`, run by `make lint` and therefore by
CI. It fails when a `lib/<name>/` carrying a `library.json` is absent from
`lib_deps`, and when a local `lib_deps` entry has no directory behind it. The
check reads the wiring rather than trusting a green build, because a green
build is exactly what this defect produces.

Source: `platformio.ini` (`[env] lib_deps`), `scripts/check-lib-deps.sh`.

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
not an on/off switch: `Shell` itself always exists and always runs, and the
mode decides only which transports may reach it.

Source: `src/main.cpp` (`Shell::begin()` called unconditionally).

### `shell.mode` gates every transport into Shell, not just the two named ones

Three code paths reach `Shell::execute`: the serial console
(`Shell::pumpConsole`), the MQTT CLI (`cliInboundHandler` -> `runCli`), and
HTTP - `POST /api/cmd` and the `/ws/serial` terminal both enqueue on the same
ring. A mode that closed only the first two would leave the broadest remote
surface open on a device the operator believes is headless, so every narrowing
mode also closes the HTTP command surface; `full` is the only value that keeps
it, and `/ws/serial` is closed at connect so a narrowed mode leaves neither an
interactive session nor its log replay up. OTA is deliberately untouched in
all modes: the periodic `manifest_url` poll is the recovery path when a mode
change goes wrong, and it needs no subscription. (The `cmd/ota` push topic is
separately broken at boot for every mode - see the tracker - so it is not the
path to rely on.)

An absent key parses to `full`, so a config written before the key existed
behaves exactly as it did. A present value that is not a known name parses to
`off` and says so - `shell.mode_unrecognised` for a misspelt string,
`shell.mode_not_a_string` for a bool, number or object, which the config layer
would otherwise hand over as an absent key. A hardening request the firmware
cannot read exactly must not silently serve the full surface. The mode is resolved once on first use, not
per command - it changes on reboot, which is also when a config push lands.

How enforced: `shellModeResolve` / `shellModeParse` /
`shellModeSerialAllowed` / `shellModeMqttAllowed` / `shellModeHttpAllowed`
(`shell_mode_policy.h`,
host-tested in `test/test_shell_mode` under a 95% floor). The MQTT gate skips
the `cli/#` subscription at both registration sites AND guards
`cliInboundHandler`, so a retained message cannot slip through a reinit. A new
transport that reaches `Shell` must add its own gate here; the predicate list
is the checklist.

Source: `lib/thesada-core/src/shell_mode_policy.h`,
`lib/thesada-core/src/Shell.cpp` (`mode`, `pumpConsole`),
`lib/thesada-core/src/MQTTClient.cpp` (`cliInboundHandler`, `begin`,
`reinitSubscriptions`),
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`cmdHandler`, `_ws`).

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
