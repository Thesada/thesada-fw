# thesada-fw invariants

The load-bearing rules this firmware relies on. Every PR that touches a
listed area must keep these true. Violations require this file to be
updated with a justification, not silent landing.

Dated 2026-11-10 (a POST body to `/api/config`, `/api/cmd` or `/api/file` with no `Content-Length` header is refused with 411 like a chunked one, so a request with neither header no longer reads as an empty body and truncates the `/api/file` target). Previously 2026-11-09 (the web terminal opens only on a single-use 128-bit token from `/api/ws/token`, bound to the caller's IP and spent at the `/ws/serial` handshake, so a refused upgrade gets no log line and two clients behind one NAT each get their own. A full grant table refuses, never evicts). Previously 2026-11-09 (an older `/config.json` is migrated at boot on a copy, the old file kept as `/config.json.bak`, and the live doc switches only after the write landed. A failed, newer or invalid schema runs unmigrated and alerts the app). Previously 2026-11-08 (the app alert goes out on every `Telegram.broadcast`, before any chat check, and the rules example publishes it itself without the Telegram module. `temperature.interval_s` is bounded to 5 s .. 1 day from its signed value. A cellular GET with an empty body (`Content-Length: 0`, 204, 304) completes at once. An `/ota` upload whose chunk 0 was accepted is not re-authenticated at completion). Previously 2026-11-08 (no Telegram or webhook TLS runs inside a Lua, shell, MQTT or timer chain: `Telegram.send` and `Telegram.broadcast` publish the app alert at once and queue the chat and webhook sends, four deep, and `TelegramModule::loop()` delivers one per pass. The rules example raises one alert per firing through `Telegram.broadcast(msg, code)` and publishes no alert JSON of its own. Shell `config.set` types a bare number as a 64-bit integer and keeps an out-of-range value as a string, and a numeric `telegram.chat_ids` entry is sent as stored. A body or `/ota` chunk 0 authenticates through the counted, lockout-first path, so a locked client buffers nothing, a guess dropped before completion still counts once, and the guess that starts a lockout answers 401 like on every other route. The alert relay scrubs control bytes, cuts the code on a UTF-8 boundary and keeps room for the heap breadcrumb. The SD event log counts only the bytes the file grew by. The route-auth gate matches identifiers, each allowed raw `_checkAuth` exactly once). Previously 2026-11-07 (`temperature.interval_s` is floored at 5 s for both the sampler and the HA expiry). Previously 2026-11-07 (loop-written state reaches async_tcp only as a copy under a portMUX: the sensor state, the log ring and the setup result, with atomic flags for `/api/cmd`, config writes and restart, and `gmtime_r` everywhere. `Shell::enqueue` moves the sink into its slot). Previously 2026-11-07 (the login lockout covers every authenticated route through `_authOrReply`, kept by a lint gate. `fs.write` and `fs.append` ack only bytes read back after close. The Telegram relay publishes the app alert JSON within the 256 B slot. The USB console waits up to 50 ms instead of dropping long lines). Previously 2026-11-05 (every call from C into Lua runs under one 10 s budget, not only the boot files: EventBus, MQTT and timer callbacks, `lua.exec` and `lua.load` go through `luaBudgetedCall`, and a nested call shares the outer budget, so no script holds the main loop past it. Time `Telegram.send`, `Telegram.broadcast` and `MQTT.publish` spend blocked is given back to the script, at most 60 s per outermost call. The invariant-ledger gate covers every file a `Source:` line cites, and CI hands it the range, so a range it cannot diff fails the job. A file every release or dependency bump changes (`src/thesada_config.h`, `platformio.ini`, `scripts/coverage-floors.txt`) is named in prose, never on a `Source:` line. `Config::set` is removed, the shell is the only dot-path writer. The rules example keys per-probe streaks by address, publishes its alert as the JSON the app stores, and publishes and starts the cooldown before the Telegram send. The Lua `string`, `table` and `math` globals are the library tables, not their names). Previously 2026-11-05 (every config writer except the generic `fs.write` / `fs.append` commands goes tmp + rename through `Config::writeFile`, which `Config::save` and the shell config writers share. It runs on the main loop only, because every writer shares `/config.json.tmp`, and renames only after the tmp, read back after close, holds the full measured size, since the lfs write happens inside an `fclose` whose result is dropped. HTTP `POST /api/config`, an `/api/file` write that resolves to the LittleFS `/config.json` and temperature discover and prune write the file only, leave the live config untouched and keep the last good file on any failure. The HTTP writers refuse a body that is not one JSON object. `/api/config` replies ok and restarts only once the new file is on disk. No HTTP handler walks the config doc: they read a main-loop copy refreshed on `Config::revision()`. A narrowed `shell.mode` closes `POST` and `DELETE /api/file` with the command surface, while `/api/config` and `/ota` stay open as the way back. A chunked POST body is refused with 411. `GET /api/file` streams any size, POST caps at 64 KB (16 KB for the config file), and every target is written through a tmp in the same directory, on SD with the old file parked and restored so no failure loses the last copy. The web snapshot refreshes from a `Config::onChange` hook, and a too-long credential locks only when it is the one in use, answers 403 with its reason and is not counted as a guess. `/api/state` reads a cached MQTT state instead of touching the client). Previously 2026-11-05 (a full login lockout table evicts no entry: an IP with no free slot counts on one shared overflow entry, and while that entry is locked every IP without a slot gets 429. Key-bearing heap copies are wiped before they return to the heap: the client cert and key buffers on every free, the MQTT payload copies, the CLI request envelope, the enroll cert reply and the shell `secret.set` value. The HTTP command path, the Telegram Bot API request and a secret kept in `config.json` are not covered. `cert.set` stores each half unchecked, and `cert.apply` reboots onto a stored pair only when it validates. A refused `cert.apply` records the broken verdict, so `cert.clear` opens on the same password session, and a check the heap could not run replies low heap and leaves the verdict alone). Previously 2026-11-05 (a publish or subscribe topic that does not fit its buffer is refused and logged, never clipped: every prefixed topic goes through `MQTTClient::formatTopic`, `publish`, `publishRetained` and `publishNow` refuse an empty topic, the subscription table refuses a topic it cannot hold, and the HA discovery uid holds a full slug. HA discovery clears the retained config a long slug left under its clipped 26.11.1 uid, so no old entity stays fed beside the new one. A `cli_response` line no page holds goes out in pieces, each page that ends inside a line carrying `cont: true`, and a page with its `cont` stays within the page limit, 160 bytes under the reply cap. A reply that still does not fit is refused, never clipped. Pages are sized to what one PubSubClient publish carries, and replies go out outside `mqtt.send_interval_s`. A chunked `fs.cat` sends only the bytes whose escaped form fits and reports them in `length`. An `mqtt.broker` host past the 96-byte host buffer is refused and logged, never dialed clipped: at boot the client has no broker and the reboot budget rolls back to last-good, a reinit keeps the previous broker and port, and `cmd/config` refuses it before saving. A connect snapshots last-good only when the configured host and port are the ones that session dialed). Previously 2026-11-05 (`net.http` runs its TLS on the 12 KB `net.http` task on both transports, so a cellular fetch no longer runs mbedTLS deep in the CLI chain on the 8 KB loop task. The shell's wait ticks MQTT once a second, so the keepalive runs through the fetch, and gives the loop back after 60 s on WiFi and 30 s on cellular. `MQTTClient::tick` does nothing inside another call into the client. A cellular GET's read loop stops 25 s after its connect began, checked on every pass, so a stalled header or body stops too, and a fetch still running at the wait ends on its task, which then frees the job. One `Cellular::httpsGet` at a time owns modem socket slot 1, and a chunked cellular OTA waits out a busy slot before each try instead of spending one. Every `ATGuard` wait feeds the task watchdog in slices of 1 s at most. A bearer recycle or modem hard reset that a socket failure earns on another task is queued for the loop's next health probe. `Shell::pumpConsole` runs only on the task that ran `Shell::begin`). Previously 2026-11-05 (an ADS1115 channel read probes the chip, waits on each conversion for at most 10 ms and probes again, so a chip off the bus is marked offline instead of holding `loop()` until the task watchdog, and it comes back on its next full read. A channel scales its counts by the PGA range the chip runs at, and its reading is AC RMS with the DC mean removed, unless `"dc": true` makes it the mean. No ADS1115 path calls the library's spinning `readADC_*`). Previously 2026-11-05 (OTA fetches a manifest or binary only from an `https://` URL. `http://` needs `ota.allow_insecure`, any other scheme never passes, and a refusal publishes `insecure-url` and fetches nothing). Previously 2026-11-05 (the planned restart into an installed update, which boots it pending, starts that image's crash streak at one instead of carrying the installer's, so a fix installed by a crash-looping image with no rollback target is not rolled back at its first boot. Once the update has set the boot slot the installer records the build as the fled release unless the record already names it, so a build that is aborted or rolled back before it confirms is skipped for 24 h instead of downloaded at every check, and a confirmed image drops a record that names its own build). Previously 2026-11-05 (a config string that lwIP keeps, or that is held across a console pump, is a copy, never a pointer into the config doc: the NTP server names live in two file-static slots that alternate per `configTime`, and the WiFi scan candidates hold their SSID by value. An NTP host that does not fit 255 bytes, which lwIP would refuse anyway, falls back to the default instead of being clipped). Previously 2026-11-05 (cellular NTP runs from ACTIVE, not in the activation step. The first try waits 30 s after activation, because the modem cannot resolve straight after `CNACT` (`+CNTP: 62`), and a failed attempt retries 60 s later, doubling to a 30 min cap, until NTP has synced this boot. Each attempt tries `ntp.server`, then `ntp.server2`, then `pool.ntp.org`, at most two, one per loop pass 2 s apart, and never a host a carrier cannot reach or the modem command cannot carry: a private, loopback, link-local or zero IPv4 literal, a `.local` name, or a name over 111 bytes (one trailing dot ignored, `cell_ntp_policy.h`). One scheduled `+CNTP` wait is capped at 15 s (`ntp.cell_timeout_s` above that is clamped, `net.ntp sync` still waits the full value), so a hold stays under the 20 s the cellular MQTT keepalive is sized for. The MQTT session no longer waits on the sync. Nothing on the cellular path needs the real time first: enrollment status checks wait for WiFi and `ntpSynced()`, and TLS never checks certificate dates). Previously 2026-11-04 (a lost cellular network or bearer in steady state clears `_started` and re-runs the non-blocking activation under its deadline, so no module stops ticking while the modem recovers. Cellular activation syncs the clock off the modem whenever NTP has not run this boot, not only below the sane-epoch floor, and that sync counts as `WiFiManager::ntpSynced()`. The long cellular waits pump the console. Failed modem sends count toward the same bearer-recycle and hard-reset streak as failed opens. A TLS write that times out closes the session. A cellular OTA chunk writes only its requested range, from a 206 or a whole-image 200. A modem dial failure does not count toward the low-heap reboot budget). Previously 2026-11-04 (one TLS stack on every transport: MQTT and HTTPS over cellular run `TlsClient`, mbedTLS on the ESP32, over a plain modem TCP socket, so the server chain and host name are checked exactly as on WiFi; the modem's own MQTT and SSL stacks are not used, and the modem sockets live for the whole program). Previously 2026-11-03 (cellular HTTPS binds a CA: OTA passes the WiFi CA to `Cellular::httpsGet`, which uploads, converts and binds it to the socket past TinyGSM's shadowed `certificates` array before connect and refuses on failure; `net.http` over cellular needs `/ca.crt` or `--insecure`). Previously 2026-11-03 (a POST body to `/api/config`, `/api/cmd` or `/api/file` is buffered per request in `req->_tempObject`, only after auth passes at chunk 0 and only up to the endpoint cap, and is acted on only when it arrived whole). Previously 2026-11-03 (an `ota.cmd_topic` publish only requests a check against `ota.manifest_url`; its payload never sets the manifest URL). Previously 2026-11-01 (HA discovery for a DS18B20 sets `expire_after` to three `temperature.interval_s`, so a stale probe, which skips its per-sensor topic, goes unavailable in HA instead of holding its last value). Previously 2026-11-01 (a shell error, an unknown command or a failed `config.set` / `config.del` / `config.save` / `secret.set` / `lua.exec` replies `ok:false` on `cli_response` and `/api/cmd`; a console line run while a command blocks parses into its own buffer and a deeper one is refused; `config.set`, `secret.set`, `fs.write`, `fs.append`, `cell.at`, `lora.send` and `lua.exec` take their value as the rest of the line, inner whitespace intact). Previously 2026-11-01 (the cellular `+SMPUB` command holds a full 159-byte topic, and a `+SMPUB`, `+CDNSGIP` or `+CNTP` command that does not fit its buffer is refused, never sent clipped). Previously 2026-11-01 (Lua `Node.uptime()` and `Node.setTimeout` deadlines count 64-bit `esp_timer` ms, so neither breaks at the 49.7-day `millis()` wrap; a NaN delay is refused, a negative one fires next pass, one past 30 days is capped). Previously 2026-11-01 (the shell refuses a line or a token it cannot hold instead of clipping it: the parser holds the 1023 bytes `runCli` accepts and 16 words, the WS terminal and serial console refuse a line past their 255-byte buffers, and shell `config.set` / `config.get` refuse a key past 127 bytes; `fs.mv` refuses a move between filesystems). Previously 2026-11-01 (a DS18B20 probe without a usable reading is published as `stale` with no `temp_c`/`temp` and no per-sensor value, never its last reading; a jump past `max_delta_c` is held and accepted when the next reading agrees with it, so one real step cannot lock a probe out). Previously 2026-11-01 (bearer-token and WS-grant expiry count 64-bit uptime, so an expired token or unused grant does not validate again 2^31 ms after it expired; the login lockout counts 64-bit uptime too, so no IP is locked out by the `millis()` wrap; after a lockout one wrong guess re-locks, an IP is forgotten on success or after 10 min with no failure, and a full table evicts the fewest-fails entry, else the soonest release, so a new IP is always counted). Previously 2026-10-26 (a Telegram or webhook POST resolves its host first, fed, skips on a failed resolve, and reads only the status line under a hard deadline after capped connect, handshake and write, so a stalled or slow-drip reply inside a boot script stays inside the task watchdog; a caught Lua budget error is re-raised by `pcall`/`xpcall` and fails the file, and the budget hook is inert after the file ends). Previously 2026-10-25 (MQTT dials the resolved broker address itself, so PubSubClient never repeats the lookup, and a DNS outage never spends the low-heap reboot budget; an SD directory named like a log holds its slot but is never resumed). Previously 2026-10-25 (no boot step blocks past the task watchdog without feeding it: the WiFi connect and NTP waits feed per poll step through `Watchdog::waitStep`, the OTA GET resolves and feeds before each redirect hop, and a boot Lua script is cut off after 10 s). Previously 2026-10-25 (the SD log to resume or open is picked from one root listing, so a card with hundreds of logs boots inside the 30 s watchdog). Previously 2026-10-25 (a failed healthy-uptime clear or USB mark is retried one hour after that failure, not on the next whole hour of uptime, so a first failure late in uptime does not retry on the next loop turn). Previously 2026-10-24 (a refused fled try puts back the age and try count its failed save overwrote in RAM, so a failed save spends no try; the age it holds is the 24 h cap, which adds no writes). Previously 2026-10-24 (a fled-release try whose save before the download fails does not download, so a lost write cannot let every boot install the build again). Previously 2026-10-24 (a fled release is refused for 24 h of skip age, then installed again, two tries before a new day starts, and the next switch starts a new record; the age is uptime carried over resets in RTC memory and saved at a boot's first refusal and at most every 10 min, and a synced clock adds at most an hour of time off per reset and is not read inside a boot, so a clock that jumps neither ends the skip more than an hour early nor makes it permanent). Previously 2026-10-24 (a slot switch from a confirmed image records its own `FIRMWARE_VERSION` as the fled release once the boot slot is set, and the update check refuses a manifest entry with that version as `fled-release`, so the image the switch booted does not pull the fled build straight back; another manifest version, `ota.check --force` and a firmware upload still install, and a power-on does not lift the refusal). Previously 2026-10-24 (the image a slot switch boots records the slot it came from by slot address and app ELF sha256, not by descriptor strings that match every build, and any boot of that slot removes the record, so a reinstalled build is a rollback target again and the old strings record blocks nothing; a switch target must verify as a whole image, and the rescue flag confirms only on a boot that no planned restart led to). Previously 2026-10-24 (a USB-flashed image that stays up an hour with a known crash streak is marked valid, so the first OTA after it can roll back to it; a switch target is still a valid slot only). Previously 2026-10-24 (the hourly healthy-uptime clear counts 64-bit uptime, so its retry still fires after the 49.7-day `millis()` wrap). Previously 2026-10-24 (a USB-flashed image, which otadata reads as undefined, counts as confirmed, so its operator restarts hold the crash streak; a failed healthy-uptime clear is tried again an hour later instead of settling). Previously 2026-10-23 (a CA, cert or key PEM handed to `WiFiClientSecure` is static, owned for the client's lifetime, or declared before the client in the same scope; `net.http` no longer drops its CA PEM before the handshake). Previously 2026-10-23 (an operator restart on a confirmed image holds the crash streak, and so does an MQTT budget reboot once that image has reached the broker since it was installed; a confirmed image that never reached the broker still counts its budget reboots and escapes to the other slot. A pending image counts every reset, and panic and watchdog resets always count). Previously 2026-10-22 (a valid image that stays up an hour clears its crash streak, so restarts weeks apart never add up to a slot switch; a loop that crashes within the hour still reaches the limit). Previously 2026-10-06 (a slot switch and a healthy confirm clear the crash streak, so the image that comes up next is not already at the limit). Previously 2026-10-06 (a pending image rolls back only when the other slot is a different valid image that is not the one just left; otherwise it is confirmed so the bootloader cannot boot that slot. A streak write that fails is treated as unread). Previously 2026-10-06 (a software reset counts toward the crash limit, so three restart loops abandon the image and one OTA reboot stays under it; power-on and deep sleep still clear the streak). Previously 2026-10-06 (three panic or watchdog resets abandon the running image: a pending image is marked invalid, a valid image boots the other valid slot and that slot is not the image just left, and a brownout holds the streak). Previously 2026-09-28 (a `cert.clear` that removed a cert reboots 3 s later, so the next boot starts enrollment; a unit that can re-enroll asks the app whether its cert serial is revoked and wipes only on an explicit, serial-matched "revoked"). Previously 2026-09-26 (a failed setup passphrase does not leave a station network, the claim link drops trailing slashes, and a timed-out enroll POST is reaped before the next one starts). Previously 2026-09-25 (the setup page exists only while the fallback AP is up and no station network is saved, unless a result from this boot is still on screen; the form is applied on the main loop and the claim link is text. A pending OTA image is marked valid after the first MQTT session or five minutes up, and a crash loop meets neither. Publish topic buffers are 160 bytes so a long tenant slug is not clipped. The enrollment claim code is an 8-digit serial-seeded
secret that never rotates and that password-session `secret.set` refuses;
enrollment only talks to an `https://` base. The cert arrives by HTTPS pull,
not over the AP. A stored enrollment cert is never polled for again, a reply
is stored only when it names this unit and its pair validates, and the ack
waits for the first mTLS session. Prior: 2026-09-23 `cmd/config` keeps a pushed `topic_prefix` at the boot
value until restart. A prefix that does not fit is refused, a shell edit
of another key keeps the boot value, and `config.save` still writes the
prefix already on disk. Reconnects broker changes without clearing
subscriptions. Prior: 2026-09-22 `cmd/config` accepts a JSON document on a
CA-verified TLS session only, including a password session, and refuses a
blob that drops `mqtt.broker`. It is not gated by `shell.mode`. Prior: 2026-09-17 OTA version compare rejects anything that is not N.N.N.
Prior: 2026-09-06 wildcard fs.rm needs --yes and never takes config.json
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

A command that takes two paths resolves both. `fs.mv` refuses with
`Cannot move between filesystems` when they resolve to different
filesystems: `rename` cannot cross mounts, and stripping both prefixes
for the source's FS turned `fs.mv /data.csv /sd/data.csv` into a rename
of `/data.csv` onto itself that replied `Renamed`.

How enforced: core does not depend on the SD module (one-way: SD
module depends on core). Modules call `Shell::registerFS` in their
own `begin()` after mount; core never imports SD-mod headers.

Source: `lib/thesada-core/src/Shell.cpp::resolveFS`, `stripPrefix`,
`registerFS`, `cmd_mv`; `lib/thesada-mod-sd/src/SDModule.cpp::begin` (caller);
`lib/thesada-core/src/MQTTClient.cpp::runCli` (consumer).

---

### `fs.write` and `fs.append` ack only the bytes that landed

littlefs commits in `close()`, whose result is dropped, so a write count says
nothing. `Shell::writeChecked` writes or appends, closes, reads the size back
and judges it with `fileWriteLanded`: the shell and the MQTT CLI answer
`ok:false` with the landed byte count when the file did not grow by exactly the
payload. The SD CSV log counts what `printf` wrote and warns once a minute on a
short row.

How enforced: `fileWriteLanded` in `config_write_policy.h`, host-tested in
`test/test_config_write`, benched on a full LittleFS on 2026-11-07.

Source: `lib/thesada-core/src/Shell.cpp` (`writeChecked`, `cmd_write`),
`lib/thesada-core/src/MQTTClient.cpp`, `lib/thesada-mod-sd/src/SDModule.cpp`.

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

### OTA fetches only `https://` URLs unless `ota.allow_insecure=true`

`check()` refuses a manifest URL, configured or an `ota.check <url>` override,
and the manifest's binary `url` unless it starts with `https://`. `http://`
passes only under `ota.allow_insecure`, and any other scheme never passes. A
refused manifest URL logs `ota.check_refused reason=insecure_url url=`, a
refused binary URL logs `ota.phase_change from=fetch_manifest to=idle
reason=insecure_url url=`, and both publish `refused` with `insecure-url` and
fetch nothing. The binary check runs after the up-to-date return and before
the fled-release gate, so it spends no fled try. Redirects keep the scheme
(`otaRedirectAllowed`), so an `https://` fetch reaches plain http on no hop.

Why: the manifest is not signed. Its only origin check is the TLS chain, and
the binary's only check is the manifest sha256. Over plain http an on-path
host sets `url` and `sha256` together, so the sha256 proves nothing.
`ota.allow_insecure` is already the opt-in for an OTA origin nothing verifies,
and plain http is no weaker than TLS without a CA. An http binary behind a
verified https manifest would still be bound by its sha256, but no shipped
manifest uses one, and one rule for both URLs is one predicate to review.

Over cellular every OTA fetch is TLS (`Cellular::httpsGet`), so an allowed
`http://` URL there opens TLS on port 80 and fails instead of running plain.

Fixed 2026-11-05: until then an `http://` URL went through a plain
`WiFiClient` with no opt-in at all.

How enforced: `otaUrlAllowed()` in `ota_verify_policy.h`, host-tested in
`test/test_ota_verify`. Code review keeps both calls in `check()` ahead of
`fetchManifest` and `applyUpdate`, which are private and have no other caller.

Source: `lib/thesada-core/src/OTAUpdate.cpp::check`,
`lib/thesada-core/src/ota_verify_policy.h` (`otaUrlAllowed`, `otaRedirectAllowed`).

### Every `OTAUpdate::check()` exit emits exactly one `<prefix>/status/ota` record

`refused` (with `reason` = `no-transport` | `no-manifest-url` | `no-ca` |
`insecure-url` | `heap-low` | `manifest-fetch-failed` | `fled-release`), `up-to-date`, `updating`, or
`failed`. Operators see the result of every check without needing serial.
Silent returns are a regression - any new bailout path in `check()` must
add a matching `publishOtaRefusal` (or other state) call.

`publishOtaRefusal` must NOT short-circuit on `!connected()`:
`MQTTClient::publish` queues on the ring while the broker is down, so a
refusal raised mid-outage is
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

### OTA version compare rejects anything that is not `N.N.N`

`otaIsNewer` parses both sides with a strict digit-only three-field parser
(`otaParseVersion`). Partial (`1.2`), signed (`-1.0.0`), trailing junk
(`1.2.3x`), empty, or overflow fields make the predicate return false.
A malformed local no longer makes every remote look newer. `force` still
bypasses via `otaShouldUpdate`.

How enforced: host tests in `test/test_ota_verify`. Coverage floor 95% on
`ota_verify_policy.h`.

Source: `lib/thesada-core/src/ota_verify_policy.h`.

### An `ota.cmd_topic` publish never picks the manifest URL

A message on `ota.cmd_topic` only requests a check against the configured
`ota.manifest_url`; its payload is ignored and any pending override is
cleared. The topic is not authz-gated, so a password, `allow_insecure` or
unverified cellular session that can publish there would otherwise point the
device at its own manifest, binary and sha256 under any publicly trusted
cert. A URL override is `ota.check <url>`, which `cliAuthzPasswordCmdAllowed`
keeps to mTLS, serial and HTTP.

How enforced: code review of `OTAUpdate::registerCommandTopic`; the
callback must not read `payload`.

Source: `lib/thesada-core/src/OTAUpdate.cpp::registerCommandTopic`,
`triggerCheck`.

### OTA-over-cellular shares the WiFi cert-verification gate

The cellular OTA path (manifest fetch + ranged binary fetch) verifies against
the same CA as WiFi OTA: `otaCellularCa()` hands `Cellular::httpsGet` the
`_otaCaCert` that `configureSecureClient` gives `WiFiClientSecure`
(`/ca.crt`, else the PROGMEM bundle). `httpsGet` runs `TlsClient` over modem
socket slot 1 with that bundle, so the check is the ESP32's own (next
invariant). A null CA means no verification and is passed only when the CA
is empty, which `begin()` / `check()` allow under `ota.allow_insecure` alone.
`net.http` over cellular follows its WiFi rule: `/ca.crt`, or `--insecure`.

Fixed 2026-11-03 (fw#50): until then the cellular client never set a CA,
so cellular OTA ran unverified TLS whatever the config said.

How enforced: `httpsGet` takes the CA as a required argument with no
default, so every caller states its choice and a new one does not compile
without it. Code review keeps a non-null CA on every path that is not an
explicit insecure opt-in. Bench check on owb-debug: `net.http` to a host
whose cert names another domain is refused over cellular.

Source: `lib/thesada-core/src/OTAUpdate.cpp` (`otaCellularCa`,
`otaHttpGetCellular`, `downloadBinaryChunkedCellular`),
`lib/thesada-mod-cellular/src/Cellular.cpp::httpsGet`,
`lib/thesada-core/src/Shell.cpp` (`httpFetchRunCellular`),
`lib/thesada-core/src/Net.h`.

### Every TLS session on the modem checks chain and host name on the ESP32

MQTT over cellular and `Cellular::httpsGet` run `TlsClient`: mbedTLS on the
ESP32 over a plain modem TCP socket (`Cellular::tcpClient`). The host name
is set on every session, sent as SNI and checked against the server
certificate, and `TlsClient` refuses to start a session with a CA but no
host name, or with neither a CA nor an explicit `setInsecure()`. The MQTT
session is one `TlsClient` that rides on the WiFi socket or the modem
socket, so WiFi and cellular apply the same chain, name and client-cert
rules.

The SIM7080's own TLS (`SMSSL` for MQTT, `CASSLCFG` sockets) checks the
chain against an uploaded CA but never the host name, and no AT setting
turns that on (SIM7080 AT manual V1.02). A valid chain to any root in the
bundle passed for any name. The modem TLS stacks are therefore not used for
anything that needs to know who answered.

The two modem sockets are static `ModemClient`s for the program's lifetime.
TinyGSM registers a client in `sockets[mux]` when it is constructed and
never removes it, so a freed client would leave a dangling pointer that a
later `+CASTATE` / `+CADATAIND` / `+CARECV` URC writes through. TinyGSM's
`maintain()` refreshes buffered data only through `sockets[0]`, so slot 0
must exist for any slot to read. Each socket call takes the AT bus guard
for that exchange only, never for a whole transfer.

How enforced: code review rejects a new modem TLS path (`SMSSL`,
`CASSLCFG`, `TinyGsmClientSecure`) and a heap-allocated TinyGSM client.
Bench check on owb-debug: MQTT and `net.http` to a name the broker cert
does not cover are refused over cellular with
`tls.handshake_failed ... verify=0x00000004` (CN mismatch).

Source: `lib/thesada-core/src/TlsClient.{h,cpp}`,
`lib/thesada-mod-cellular/src/Cellular.cpp` (`ModemSocket`, `ModemClient`,
`tcpClient`, `httpsGet`), `lib/thesada-core/src/MQTTClient.cpp::connect`.

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

### A PEM handed to `WiFiClientSecure` outlives every connect on that client

`setCACert`, `setCertificate` and `setPrivateKey` keep the `const char*`,
not a copy. The PEM is parsed on each `connect()`, including the reconnect
an `HTTPClient` redirect makes, so the buffer must be live for every
handshake the client can still start. That means one of three things: it
is static (`.rodata` or a file-scope buffer), it is owned for the client's
lifetime, or it is a local declared before the client in the same scope,
so it is destroyed after the client. A PEM `String` scoped inside the `if`
that read it is gone before `http.begin()`, and the handshake parses freed
heap.

| Site | PEM buffer | Why it lives long enough |
|---|---|---|
| `Shell.cpp` `httpFetchRun()` (`net.http`) | local `String pem` | declared first in the function, so it is destroyed after `tls` and `http` |
| `Enroll.cpp` `enrollTask()` | local `String ca`, else `OTA_CA_PROGMEM` | `ca` is declared before `tls` in the same block; the bundle is `.rodata` |
| `OTAUpdate.cpp` `configureSecureClient()` | file-static `_otaCaCert` | loaded once by `loadCaCert()`, never reassigned |
| `MQTTClient.cpp` `begin()` | `_caCert`, pointing at `_caHeap` or `OTA_CA_PROGMEM` | `_caHeap` is never freed; the bundle is `.rodata` |
| `MQTTClient.cpp` `connect()` | `_clientCert`, `_clientKey` | module buffers, wiped and freed only when a load or an allocation fails, and `connect()` then evicts the old pointer with `setCertificate(nullptr)` / `setPrivateKey(nullptr)` before any handshake |
| `TelegramModule.cpp` `begin()` | static `_caCert`, `_webhookCaCert` | statics beside the static clients, set once in `begin()` |

`TlsClient` keeps its PEM pointers the same way. MQTT passes the module
buffers above, cellular OTA the file-static `_otaCaCert`, and the cellular
`net.http` fetch (`Shell.cpp` `httpFetchRunCellular()`) a `String pem`
declared before the GET it outlives.

How enforced: code review. A new `setCACert` / `setCertificate` /
`setPrivateKey` call adds its row here, naming the buffer and why it
outlives the client.

Source: `lib/thesada-core/src/Shell.cpp`, `lib/thesada-core/src/Enroll.cpp`,
`lib/thesada-core/src/OTAUpdate.cpp`, `lib/thesada-core/src/MQTTClient.cpp`,
`lib/thesada-mod-telegram/src/TelegramModule.cpp`, at the functions above.

### A config string that outlives a doc rebuild is a copy

`Config::load` and `Config::replace` rebuild `_doc`, and the rebuild frees
every string in it. Shell `config.set` / `config.del` / `config.reload`, MQTT
`cmd/config` and the setup portal all do this at runtime. A `const char*`
read from `Config::get()` is therefore good only until the next rebuild. Two
kinds of holder must copy instead:

- a library that stores the pointer, not the bytes, and reads it later
- a caller that holds the pointer across `Watchdog::waitStep`, which runs
  `Shell::pumpConsole`, or across loop ticks

| Site | Holder | Copy |
|---|---|---|
| `WiFiManager.cpp` `scanAndConnect()` NTP block | lwIP SNTP keeps the `configTime` server names and re-resolves them on every poll | file-static `s_ntpHost[2][2][DNS_MAX_NAME_LENGTH]` (256). The slot flips on each `configTime`, so SNTP is moved onto the fresh slot before the old one is written again. A host that does not fit falls back to the default |
| `WiFiManager.cpp` `scanAndConnect()` candidates | `tryConnect` waits in `waitStep` between candidates | `Candidate::ssid[33]` by value, beside the password buffer |
| `MQTTClient.cpp` `begin()` and the `loop()` reinit | `PubSubClient::setServer` keeps the host pointer | `_brokerHost[96]`. A longer host is still clipped, not refused (fw #78) |

How enforced: code review. A new pointer handed to a storing library, or a
new wait that pumps the console while holding a config pointer, adds its
row here naming the copy.

Source: `lib/thesada-core/src/WiFiManager.cpp::scanAndConnect`,
`lib/thesada-core/src/MQTTClient.cpp::begin`.

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
`_mqttEnabled` (`boot.mqtt_disabled reason=node_name_not_unique`), so
`MQTTClient` never starts and no transport carries a session. `Shell` still
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
go over MQTT - a factory-fresh device has no broker credential at all. The
access point only collects WiFi credentials; the device then pulls its cert
from the app over HTTPS and never contacts the broker until it holds one.

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

### The claim code is seeded over serial at flash time and never rotates

The claim code is the bearer secret that lets an account claim an enrolled
unit. It is printed on the unit's sticker next to the fallback-AP join QR, so
it follows the same factory-sticker model as the AP passphrase.

| Rule | Why |
|---|---|
| exactly 8 digits, seeded by `flash-provision.sh` into NVS field `enroll.claim_code` | the sticker and the claim form carry it; the device only ever sends it, never shows it |
| never rotates, not on WiFi re-config, not on re-enroll | a rotated code no longer matches the sticker, and the unit has no display to show a new one |
| password-session `secret.set` refuses the field | a holder of the shared pairing credential could otherwise set a known code and claim the unit into their own tenant |
| never derived from the MAC or the device id | the device id is broadcast in the AP SSID, so a derived code is computable by anyone in range |
| sent to the app only over a CA-verified HTTPS base (`enrollUrlUsable`: `https://` only, no userinfo, query or fragment) | plain http would hand the code, and later the private key, to the network |

Eight digits is a small space on purpose: it has to be typeable. The app
carries the rest of the guess resistance with a per-device failed-claim cap;
the device cannot, because claiming happens between the account and the app.

Source: `lib/thesada-core/src/enroll_policy.h`, `test/test_enroll_policy/`,
`lib/thesada-core/src/cli_authz_policy.h::cliAuthzSecretFieldAllowed`,
`scripts/flash-provision.sh`.

### An enrolled cert is stored once, never re-polled, and acked after mTLS

The app re-issues the cert on every `/cert` poll until the device acks, and
each issue revokes the one before. The ack seals the enrollment row for good.

| Rule | Why |
|---|---|
| once a cert is stored, the device never polls `/cert` again; it only acks | a re-poll revokes the cert the device just stored |
| a reply is stored only when its `device_id` is this unit, host, port and prefix are usable (`enrollReplyUsable`), and the cert and key validate as a pair | a cert for another unit, or a broker the device cannot reach, strands it with nothing to retry |
| write order is the NVS `ack_pending` flag, then `mqtt.*` config, then the cert | a power cut before the cert lands leaves no cert, so the next boot re-enrolls from announce |
| the ack goes out only after a WiFi broker session that presented the client cert | sealing a cert that never connected leaves the app no way to re-issue |
| the enrollment POST verifies the server against `/ca.crt` or the baked roots, never `setInsecure` | the claim code goes out, and the private key comes back, on this link |
| HTTPS runs on its own 12 KB task; Config, Secret and NVS are touched only from the main loop | the handshake overflows the loop stack, and Config is single-task |
| a timed-out POST is kept until its task finishes, and no second POST starts while it is alive | dropping the job and starting another overlaps two handshakes and leaks the first |

Source: `lib/thesada-core/src/Enroll.cpp`, `lib/thesada-core/src/enroll_policy.h`,
`test/test_enroll_policy/`, `MQTTClient::clientCertPairValid`,
`MQTTClient::mtlsSessionUp`.

### The setup page is the fallback AP, and only until a station network exists

A unit with no display has to collect a station network and an app URL before it can enroll. The page is on the AP the sticker joins. It is not a general settings editor.

| Rule | Why |
|---|---|
| served only while the fallback AP is up and no station network is saved, or while a result from this boot is still on screen | after a reboot the saved network means the page is closed, so the passphrase form does not stay reachable on the LAN |
| the async handler only copies the form; Config and Secret are touched from the main loop | both are single-task, and the HTTP callbacks run on the AsyncTCP task |
| the success page is the claim link as text, not a QR, and it does not echo the form | the link is the only thing the operator needs, and the passphrase must not come back in the HTML |
| the passphrase is stored before the network is saved, and a failed save removes a passphrase this attempt just wrote | a saved network with no password closes the page on the next boot. An existing passphrase is replaced only after the save succeeds |
| the claim link drops trailing slashes on the app URL | a doubled slash is a different route |
| the app URL has to pass `enrollUrlUsable` | the next step sends the claim code over that URL |

Source: `lib/thesada-core/src/setup_portal_policy.h`, `lib/thesada-core/src/SetupPortal.cpp`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp`, `test/test_setup_portal/`.

### A pending OTA image is marked valid only after it has shown it can run

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` leaves a newly written image in `ESP_OTA_IMG_PENDING_VERIFY`. The bootloader rolls it back if the next boot never confirms it. USB-flashed images are not pending, so this rule does not touch them: `pio run -t upload` writes `boot_app0.bin`, which leaves the flashed slot `ESP_OTA_IMG_UNDEFINED`, and the bootloader never rolls that back.

| Rule | Why |
|---|---|
| `verifyRollbackLater` returns true unless the rescue flag was set, both the clear and the commit succeeded, and the RTC restart word holds neither planned marker, in which case it returns false. A set flag is cleared either way | the Arduino core confirms a pending image when this returns false. A failed clear must keep returning true so the health gate still runs. The OTA apply and the firmware upload restart through `plannedRestart` and a slot switch restarts without a marker, so a flag a failed switch left set cannot confirm the next written image |
| confirm on the first MQTT session this boot, WiFi or cellular, or after five minutes up, whichever comes first, unless the crash streak is already at the limit | a broker that is down, or a cellular-only unit, still has a way to keep a good image. Three crash resets refuse that confirm |
| at the crash limit, `setup` switches image before the serial wait and before MQTT can confirm. The other slot is a target only when otadata reads it `ESP_OTA_IMG_VALID`, `left_id` does not name it, and the image in it passes `esp_image_verify`. A pending image is marked invalid only when the other slot is a target; otherwise the pending image is confirmed so the bootloader cannot boot that slot. A valid image with a target zeroes the streak, sets the rescue flag to 2, sets that slot, records its release as fled and restarts; when setting the slot fails it clears the flag again. An undefined or unknown running state does not switch | one software reset stays under the limit, so the first boot of a new image can still confirm. A later crash loop is already valid, and the bootloader will not roll that back. `esp_ota_set_boot_partition` writes the chosen slot as new, so the next boot confirms it in `initArduino` only after the rescue flag is cleared. A failed clear leaves the image pending. An interrupted download leaves the other slot `VALID` in otadata with the new image's descriptor already readable, so state and descriptor alone pick a slot that cannot boot, and the flag was set before the switch failed. The image identity, not the streak, stops a later return. An undefined image is a USB flash: `boot_app0.bin` also leaves the other slot a seq-0 otadata entry, which reads undefined, so an undefined slot is never a target |
| an unread crash streak refuses confirm and does not switch slots | a missing counter must not look like a healthy zero |
| power-on and deep sleep clear the streak; a slot switch, a healthy confirm, and a valid image one hour up write it back to zero. Panic, watchdog, and unmarked software resets count. On a confirmed image, one that otadata reads valid or undefined, an operator restart holds, and an MQTT budget reboot holds only when the image is proven on the broker. On a pending image either one is the restart into an installed update and starts a new streak of one, and on an image whose state is unread both count. Brownout, external, SDIO, and unknown hold | the counter must survive a brownout. Three crash resets reach the limit when no boot between them stays up an hour. Config saves and other operator restarts on a confirmed image do not count, and neither do broker-outage reboots on an image that has connected before, or a long outage would boot the old image. A new image is confirmed at five minutes, long before its first budget reboot after `mqtt.reboot_after_fails` failed connects, so a confirmed image that never reached the broker must count those reboots: with the default `mqtt.max_exhaust_reboots` of 3 the third one reaches the limit, and a valid image switches slots under the rule above. An undefined USB image does not switch; after the healthy hour it is marked valid, below. A budget below 3 stops rebooting first, and that image stays. A pending image counts every other reset. Its install restart starts at one because the streak before it is the installer's: a crash-looping image with no rollback target that installs a fix would hand it a streak at the limit, and the guard would roll the fix back at its first boot. One OTA reboot still stays under the limit. An image of 26.10.2 or older restarts into an update without a marker, so its install still hands the streak over. A switch that left the streak at the limit would make the next update roll back at once. The pending path with no safe other slot still confirms if that write fails, because leaving the image pending boots the slot just refused |
| `confirmIfHealthy` runs on every main-loop turn, and `loop` calls it before the enabled and transport returns | `loop` itself is skipped when OTA is disabled or no transport is up, and that unit must still confirm or stay pending |
| `OTAUpdate::plannedRestart` and `OTAUpdate::brokerRestart` set an `RTC_NOINIT` word to their own magic value and restart; `recordBootStreak` reads and clears it on every boot. It holds only a software reset that carries a marker on an image whose state reads `ESP_OTA_IMG_VALID` or `ESP_OTA_IMG_UNDEFINED`, and for the broker marker only when that image is proven. On an image that reads `ESP_OTA_IMG_PENDING_VERIFY` a software reset with either marker starts the streak at one and logs `ota.crash_streak_restarted reason=install previous= streak=` | a power-on clears the streak whatever the word holds, and clears the word. Any other value reads as unplanned, and a marker read once cannot excuse a later unplanned reset. An unreadable image state counts. `plannedRestart`: shell `restart`, `fs.format`, `identity.reset`, the web restart, config save, and firmware upload, `Node.restart()`, the cert reboot, and the OTA apply. `brokerRestart`: both MQTT budget reboots. The heap watchdog and both slot switches restart without a marker and count; the switches clear the streak first |
| an image is proven on the broker when NVS `boot` key `proven_id` holds its id: the running slot address, then the app ELF sha256 from its app descriptor. `confirmIfHealthy` writes it on its first call after the boot's first MQTT session, WiFi or cellular, unless that boot already read a match. A boot tries the write at most once; after a failed write the next boot tries again | the record names one slot and one build, so a new build, or the same build in the other slot, starts unproven. A proven image writes nothing on later boots. An unreadable descriptor or an all-zero hash never matches, so that image counts its budget reboots. One record is kept, for the last image to connect; any other image reads unproven until it connects |
| the slot a switch came from is recorded in NVS `boot` key `left_id` by the image the switch booted: on a boot that finds the rescue flag on a pending image, `recordBootStreak` writes the other slot's address and app ELF sha256, the same id as `proven_id`. A flag of 2 always records; a flag of 1, from the older guard, records only when the two images' descriptor version, date and time differ. Every boot first removes a `left_id` that names its own slot. The other slot is not booted while its id matches the record. No `left_id`, one of any other length, or a candidate with an all-zero hash is no match. An unreadable candidate descriptor, an NVS that will not open, or a running image whose own id cannot be built refuses the switch. `left_ver`, `left_date` and `left_time` are never read, written or removed | the record stops the second switch of a ping-pong, where both images crash-loop and the left slot is not booted in between, and nothing else. A boot of the recorded slot means that slot was rewritten or the bootloader fell back to it, so the image there is not the one left even when it is the same build; a record that outlived a reinstall refused that build as a rollback target and kept a broken update. Writing it on arrival means a failed switch leaves no record. The descriptor's version, date and time come from the prebuilt IDF (`esp-idf: v4.4.7 38eeba213a`, `Mar  5 2024`, `12:12:53` on espressif32 6.13.0) and match every build on one platform, so matching on them refused every candidate, and a unit that holds only them has no record. The older guard refuses any image with its own strings, which bounds a same-platform pair, and recording its arrival there would also name an image that only installed this one after a failed switch of its own. Across platforms the strings differ and only this record bounds the pair. With every write landing, two images of this guard switch once and an old and a new image at most three times. A failed record write costs one more switch, after which the other image records this one. An image from 26.10.2 or older applies an update without a restart marker, so a flag its failed switch left still confirms the build it installs without the health gate, and across platforms that build records it as left |
| once `esp_ota_set_boot_partition` has set the switch target, a valid image writes NVS `boot` key `fled_age` as a fresh day, then `fled_ver` as its own `FIRMWARE_VERSION`, zeroes the RTC unsaved pair and restarts, whether or not the writes land. `check` passes a manifest whose version passes the version gate and equals `fled_ver` as N.N.N to `otaFledAction`. Under 24 h of skip age it logs `ota.phase_change from=fetch_manifest to=idle reason=fled_release version= age_s=` and publishes `refused` with `fled-release`. At 24 h it saves before the download, logs `ota.fled_release_retry version= try=` and installs; when that save fails it logs `ota.phase_change from=fetch_manifest to=idle reason=fled_try_unsaved version=`, publishes `refused` with `fled-release`, does not download, and puts back the age and tries that save overwrote in RAM, so the next check of that boot gets the same try: the first try leaves the day expired, the second (`OTA_FLED_TRIES`) restarts it at 0. `ota.check --force` installs at any age without spending a try and logs `ota.force_reflash version=fled`. The age is uptime: within a boot from `millis`, across a software reset or deep sleep from an `RTC_NOINIT` pair (unsaved seconds and their complement, taken once a boot and zeroed, capped at 20 min; power-on noise reads 0), and on the first save of a boot raised by the gap between two SNTP-synced clock readings by at most 1 h. It is saved as one u64 (age, tries, synced epoch or 0) at the first refusing check of a boot, then at most every 10 min (`OTA_FLED_SAVE_S`) from such checks and from `confirmIfHealthy`, only while the record names another build than the running one and the saved age is under 24 h; a failed save logs `ota.fled_age_unsaved reason=nvs_write` and waits for the next 10 min. A missing, unreadable or unparseable record refuses nothing, and a missing `fled_age` reads as a fresh day. Once `applyUpdate` has set the boot slot, `check` writes the manifest version the same way, as a fresh day, unless the record already names it as N.N.N or the version does not parse, logs `ota.install_recorded version=` or `ota.install_unrecorded reason=nvs_write`, and restarts. An image that otadata reads valid or undefined removes a `fled_ver` that names its own `FIRMWARE_VERSION` at the first `confirmIfHealthy` that reads its state, and a pending image right after its confirm, logging `ota.install_record_cleared version=` or `ota.install_record_kept reason=nvs_write`. Only the next switch or install replaces the record | the image a switch boots is usually the older one, so the manifest still offers the fled build as newer and the first check after the switch pulled it back. Each cycle cost a download of about 1.7 MB and a slot rewrite, then a confirm, a crash loop inside the hour and another switch: in the host model 111 downloads in 7 days, against 6 with the day. A skip with no end also held a unit on the older image after that image began to crash-loop on its own, since `left_id` names the fled slot and the skip refused the build; after the day the build installs again, in the model 24.1 h after the switch (24.4 h without NTP). An image of 26.11.1 or older inherits a crash-looping installer's streak, so its guard rolls the first try back at its first boot and the second one runs. A fled build that now crashes before it confirms keeps the record its try left, so it too costs at most two downloads a day. A try whose save was lost reads as unspent after the install reboots, so downloading it anyway would let every boot install the build again. `saveFled` moves RAM on even when the write fails, which would spend a try nothing installed: a failed first save left one install, a failed second one a fresh day with none. The age put back is the one this check computed, held at 24 h by `otaFledAdvance`, and `otaFledAges` does not age a record at 24 h, so `ageFledRelease` writes nothing after the refusal. `check` decides on the version alone and compares the sha256 only after a download, and a release publishes one binary per version, so the version names the release; a dev build that carries a release's version refuses that release too. A unit may have no synced clock (cellular-only, or SNTP blocked), so uptime is the base and the clock only adds time the unit was off. Reading it only across a reset means an NTP step or a set clock inside a boot moves nothing, a clock behind the last save adds nothing so the day still ends, and one ahead ends it at most 1 h early per reset. Without a synced clock a power-on loses the uptime since the last save, under 10 min, and the time off, and a boot that ends before its first 10 min save adds only the seconds before its first refusing check: in the host model, with that check 15 s after power-on, a unit power-cycled every 50 min ends the day 29.7 h after the switch and one power-cycled every 5 min 19.9 days after it. Saving more often would shorten the 5 min case only by spending more than one write per 10 min awake. Deep sleep counts awake time only, so at 30 s awake and 300 s asleep the day takes about 11 days. Saves stop once the day is spent and on the fled build itself, so a day of skip age costs at most one write a boot plus 144, one per 10 min awake, each a 32-byte NVS entry, and a check triggered over MQTT or the shell again and again writes nothing more. Written after the target is set, a failed switch leaves no record, and a lost write costs one more cycle before the next switch writes it. A power-on proves nothing about a build that crashed three times without one between, so it does not end the day. The image that runs the check holds the refusal, so an image without it (26.10.2 and older) that a switch boots still pulls the fled build until the manifest changes. A pending build that resets before it confirms is aborted by the bootloader, or rolled back by the guard, and the image that comes back is the one whose check installed it, so without the install record it pulled the build again at every check: in the host model 133 downloads in 200 steps. The record makes that one download, then at most two a day like a fled try. Written after the boot slot is set, a failed download leaves no record. A record that already names the build is kept so a fled try keeps its count. A record naming the running build refuses nothing while that build runs, but after a USB flash or upload of another build it would refuse it for a day, so the confirmed image drops it. The install record replaces the one before it, so a fled build that the manifest offers again after another install is not skipped. A power cut or restart before the confirm also aborts the build, and that costs a day. An image of 26.11.1 or older writes no install record, so a build it installs is pulled again after an abort |
| a failed IDF confirm is retried; a non-pending image with a streak tries the clear at one hour, again one hour after the uptime of the last failed write, and is settled once it lands; with no streak, or an unread one, it is settled at once. A USB-flashed image, one that otadata reads undefined, with a known streak is not settled at once: at the hour, after any clear, `esp_ota_mark_app_valid_cancel_rollback` marks it valid, retried on the same backoff from the last failed clear or mark, and skipped while the boot slot is not the running one. The hour counts `esp_timer_get_time` uptime, 64-bit | a failed confirm must not be treated as settled. A failed clear must not leave the streak for a later crash to add to; one try an hour keeps a failing NVS from writing every loop turn. A 32-bit `millis()` wraps at 49.7 days, and after that the hourly retry would never fire again. A valid USB image is a rollback target, so the first OTA after a USB flash can roll back to it once it was marked; an OTA that lands before the hour finds it undefined and has no target, as before. On an undefined running image the IDF call rewrites only the active otadata entry as valid and leaves the other entry as it was (IDF v4.4.7 `esp_ota_ops.c`: `esp_ota_current_ota_is_workable` lines 717-722, `rewrite_ota_seq` lines 363-377), so the other slot's seq-0 entry still reads undefined. With an update staged, the active entry is the staged image, and marking it would confirm a build that never booted. An unread streak is never written to zero and marks nothing: it may hide a crash loop. The no-other-image confirm and arming a slot switch also zero the in-memory streak, so the hour check does not write it again; a pending image whose rollback call failed keeps it and still refuses confirm |

This is not the MQTT config rollback. That one restores a broker config that never connected. This one confirms the firmware image.

Source: `lib/thesada-core/src/ota_health_policy.h`, `lib/thesada-core/src/OTAUpdate.cpp`,
the `OTAUpdate::plannedRestart` and `OTAUpdate::brokerRestart` callers, `test/test_ota_health/`;
the fled-release skip and its expiry are `otaFledAction` and the `otaFled*` helpers in `lib/thesada-core/src/ota_verify_policy.h`, `test/test_ota_verify/`.

### A publish topic buffer holds 160 bytes, and a topic that does not fit is refused

`Config::TOPIC_PREFIX_CAP` allows a prefix that, with `/sensor/battery/percent`, does not fit in a 64 or 96 byte topic. A clipped topic is a different topic. The queue slot, the subscription table, and the CLI topic buffers use the same 160 byte cap.

160 bytes does not hold every topic: a 127-char prefix plus `/sensor/temperature/` and a sensor slug of 13 chars or more needs 161, and `config.json` itself caps no prefix. So no topic is built with a bare `snprintf`:

| Path | Over the cap |
|---|---|
| any `<prefix>/...` topic in core and modules | `MQTTClient::formatTopic` logs `mqtt.topic_refused reason=too_long cap= fmt=`, leaves the buffer empty and returns false. The caller skips that publish only. EventBus events and the other topics still go out |
| the MQTT will (`<prefix>/status`) | connects without a will |
| `publish`, `publishRetained`, `publishNow` | refuse an empty topic (`mqtt.publish_refused reason=empty_topic`), which a broker answers by disconnecting |
| `subscribe` (Lua, `ota.cmd_topic`, a custom `lua.reload_topic`, core) | `mqtt.sub_rejected reason=topic_length len= cap=`, and the table refuses it too, so a reconnect never resubscribes a clipped filter. These topics are passed as configured, not formatted first |
| HA discovery uid | sized for the device id, a 31-char slug and `_current`, so `_current` and `_power` of one long slug never clip onto one config topic. A slug that clipped in 26.11.1 (`uid[48]`: with the 20-char device id a temp slug past 21 chars, power past 20, current past 18) gets a new uid. `publishDiscovery` then clears the 26.11.1 config topic with a zero-length retained publish after the pass, so HA drops the old entity, which carries the same `stat_t` and `avty_t` and would stay fed beside the new one. The clear is skipped when this session published that topic itself, a clipped uid that equals a real new one. The clear is not recorded in the manifest, since nothing stays retained to sweep, and it repeats on every connect. The discovery payload, core and SHT31, is serialized whole to the heap, not clipped at 640 or 512 bytes by the long `stat_t` and `avty_t` a long prefix makes |

The cellular `+CDNSGIP` and `+CNTP` commands refuse a host that does not fit their buffer (`cellular.resolve_skipped`, `cellular.ntp_skipped`), never sending it clipped.

Source: `MQTTClient.h` `MQTT_TOPIC_CAP`, `cli_topics.h` `CLI_TOPIC_CAP`,
`mqtt_topic_policy.h` (`mqttTopicFmt`, `mqttTopicFits`, `haLegacyUid`, `test/test_mqtt_topic`),
`MQTTClient.cpp` (`formatTopic`, `subscribe`, `publishDiscovery`),
`mqtt_sub_table.h` (`test/test_mqtt_sub_table`), `lib/thesada-mod-cellular/src/Cellular.cpp`
(`resolveHost`, `ntpSync`).

### A broker host that does not fit is refused, never dialed clipped

`_brokerHost` holds 95 chars plus the NUL (`MQTT_BROKER_HOST_CAP`). A host cut at 95 is a different name. TLS checks the clipped name, so the connect fails, and nothing in the log said why.

| Path | A host of 96 bytes or more |
|---|---|
| `begin()` at boot | `mqtt.broker_refused reason=too_long len= cap=96 action=no_broker`. The rest of `begin()` runs, the host stays empty, every connect fails, and the reboot budget records the config and rolls it back to last-good on the next boot |
| the deferred reinit in `loop()` (shell `config.reload`, `cmd/config`) | `mqtt.broker_refused ... action=keep_previous`. Host and port stay the pair last handed to `setServer`, so the unit stays on the broker it had |
| `cmd/config` | `mqtt.cmd_config_refused reason=broker_too_long`, nothing saved |
| enrollment reply | `enrollHostUsable` refuses it. A `static_assert` in `Enroll.cpp` keeps `ENROLL_HOST_CAP` within the buffer |

A connect commits last-good (`snapshotGoodConfig`) only when the configured host and port are the ones the session dialed (`mqttSnapshotProven`), else it logs `mqtt.config_commit_skipped reason=broker_not_live`. Without that, a session kept on the previous broker would commit the refused host as last-good, and after the next reboot no rollback would be left. The same check covers an `mqtt.port` written but not applied yet.

Source: `mqtt_topic_policy.h` (`mqttHostCopy`, `mqttHostFits`, `test/test_mqtt_topic`),
`mqtt_rollback_policy.h` (`mqttSnapshotProven`, `test/test_rollback`),
`MQTTClient.cpp` (`begin`, `loop`, `snapshotGoodConfig`, `mqttApplyCmdConfig`),
`Enroll.cpp`.

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
frozen at dispatch, before the Shell deferred ring. There is one session,
on whichever transport is live, so one answer: `_mtlsActive`, recomputed
from scratch on every `connect()` attempt ahead of the early returns. A move
between WiFi and cellular is a new `connect()`, so the verdict never carries
over from the other transport. Password is the default in every ambiguous
case.

Not device authentication. A leaked per-device cert still gets the full
surface - the CLI carries no signature and no replay protection.

`secret.set` stays on the password row because pairing pushes the whole
scalar secret set (`mqtt.password`, `telegram.bot_token`, `web.password`,
`wifi.ap_password`) plus per-SSID wifi passwords through it before mTLS is
live. The gate holds the field to exactly that provisioning set
(`cliAuthzSecretFieldAllowed`, backed by `secret_keymap.h`, minus
`enroll.claim_code`, which is serial-only) - a containment
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
`web_auth_policy`. Coverage floor in `scripts/coverage-floors.txt`.

Source: `lib/thesada-core/src/cli_authz_policy.h`, `test/test_cli_authz/`.

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
deferred reinit reconnect), only together with the host it belongs to, so a
refused host leaves both as they were. It is never read out of `Config` at some other moment,
so a reconnect on a changed port re-evaluates the verdict along with
everything else `connect()` recomputes. On a non-matching port the client
material is not attached either, so the session really is password auth and
sends user/pass. The same `connect()` serves WiFi and cellular.

The check is positive and fail-closed: `== MQTT_MTLS_PORT`, never
`!= password_port`. Some deployments run a plaintext listener, and "not the
password port" would read a plaintext password session as mTLS.

What it guarantees: a password session cannot use the one key it may write to
promote itself to an mTLS session. What it does not: it is no proof that a
handshake actually presented the cert. It is the port the device dialled plus
a cert that parses, not a broker-confirmed identity.

`MQTT_MTLS_PORT` defaults to 8884 in `src/thesada_config.h` and is
`#ifndef`-guarded so a build flag can override it per env. The broker
listener and whatever pairs devices against it pin the same number; all of
them move together or paired devices stop being judged as paired.

Source: `lib/thesada-core/src/MQTTClient.cpp::connect`.

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

"Broken" is exactly the state that logs `mqtt.mtls_cert_invalid`,
`mqtt.mtls_load_failed` or `shell.cert_apply_refused reason=pair_invalid`,
so a refused `cert.apply` opens `cert.clear` on the same session. A failed
buffer allocation logs `mqtt.mtls_load_oom` or
`shell.cert_apply_refused reason=low_heap` and leaves the last verdict
standing - heap pressure says nothing about the cert, and must not open
`cert.clear` to a password session while a good cert sits in NVS. Absent is deliberately not broken: there is nothing
to recover and nothing to clear, so the permission would buy the operator
nothing while widening what a shared-credential holder reaches. A half-stored
pair is the same call, and `cert.set` re-pushes the missing half from the
password row anyway. The flag drops back to false inside `clearClientCert()`
and on any successful `storeClientCert()`, so the permission ends with the cert
that granted it rather than outliving a repair until the next connect.

The fact is an explicit parameter of `cliAuthzAllowed`, not a global read from
inside the policy header, and it is a snapshot from the last `connect()`
attempt or `cert.apply` check rather than re-derived per command -
re-parsing a PEM pair on every inbound CLI message is not worth the heap. Two consequences: a device whose
WiFi `connect()` has not run carries `false` and is denied on the cellular
session, which is the fail-closed direction; and a cert the broker revoked
still parses locally, so it is not broken here and `cert.clear` stays
mTLS-only for it.

Source: `lib/thesada-core/src/cli_authz_policy.h::cliAuthzPasswordCmdAllowed`,
`lib/thesada-core/src/MQTTClient.cpp` (`_storedCertBroken`, `connect`,
`checkStoredClientCert`), `lib/thesada-core/src/cert_policy.h::storedCertBrokenAfter`,
`test/test_cli_authz/`, `test/test_cert_policy/`.

### `cert.clear` that removed a cert reboots

Clearing a stored cert schedules the same deferred reboot `cert.apply` uses,
3 s out, and keeps the session until then so the command's reply is
published. Enrollment is decided once, in
`Enroll::begin` at boot, so without the reboot a unit whose owner revoked it
sits with no cert and no enrollment until something else restarts it. After
the reboot `enrollStartStep(hasCert=false)` starts at Announce when
enrollment is configured, and a unit without enrollment comes up on
password auth at whatever `mqtt.port` is stored, which is where the admin
recovery flow's `config.set mqtt.port` has already put it. That flow also
sends `restart` after `cert.clear`; whichever lands first reboots the unit,
and the outcome is the same. A `cert.clear` with no cert
stored clears nothing and does not reboot.

The latch is serviced from the main loop on every tick, outside
`MQTTClient::loop`, so it fires with the session down and on a build or
config with MQTT disabled, where `cert.clear` arrives over the serial or
HTTP shell.

Source: `lib/thesada-core/src/Shell.cpp::cmd_cert_clear`,
`lib/thesada-core/src/MQTTClient.cpp` (`scheduleCertReboot`,
`serviceCertReboot`), `src/main.cpp::loop`.

### A stored cert is wiped only on the app's explicit "revoked"

A unit that missed `cert.clear` gets no refusal it can see: PubSubClient
does not read SUBACK codes, so the session looks healthy after a revoke. It
learns of the revoke by asking: `POST <enroll.url>/api/v1/devices/enroll/status` over the
same CA-verified client as enrollment, five minutes after boot and every six
hours after an answer, with backoff when unanswered.

The request carries device id, pubkey, the cert serial and unix seconds,
signed with the identity key over
`thesada-enroll-status\n<device_id>\n<serial>\n<ts>`. The serial is canonical
(lowercase hex, no leading zeros) because the DER bytes carry padding the app
does not store; the reply's `serial` must come back in that same form. No
check goes out until NTP has synced this boot: the boot clock floor keeps
`time()` plausible but stale, and a stale timestamp is refused by the app.

| Answer | Action |
|---|---|
| 200, `status` `revoked`, `serial` equal to ours | clear cert, reboot into enrollment |
| 200, anything else | keep the cert, check again in six hours |
| non-200, transport failure, unparseable | keep the cert, back off |

Keeping the cert on everything but an explicit revoke is the load-bearing
choice: an app outage or a restored database must never wipe a fleet. Only a
unit that can enroll again runs the check (identity, a usable `enroll.url`
and a claim code): a wipe with no way back strands it, which is worse than
holding a cert that no longer works. A unit paired over `cert.apply` without a claim
code learns of a revoke only from `cert.clear`. WiFi and the claim code live
outside the cert namespace and survive the wipe. A wipe that leaves either
half in NVS reports failure (`mqtt.cert_clear_failed`), does not reboot, and
is retried on the backoff. The reply's status is logged as a fixed
word, never echoed.

Source: `lib/thesada-core/src/enroll_policy.h` (`enrollStatus*`,
`enrollSerial*`), `lib/thesada-core/src/Enroll.cpp` (`startCheck`,
`finishCheck`), `lib/thesada-core/src/WiFiManager.cpp::ntpSynced`,
`test/test_enroll_policy/`.

### Private key material in heap is zeroed before `free()`

Use `mbedtls_platform_zeroize(buf, len)` (not `memset` - the compiler
is allowed to elide a memset on a buffer about to be freed; the
mbedtls helper is explicitly volatile-pointer to defeat that). Every
heap copy that can hold a private key or a secret is wiped before it
goes back to the heap:

| Copy | How |
|---|---|
| `_clientCert`, `_clientKey` | `secretAlloc` / `secretFree`. Held across the session because `TlsClient` keeps the raw pointer, and freed on the OOM and will-not-load branches of `connect()` |
| `onMessage` payload copy | `secretAlloc` / `secretFree`. PubSubClient's receive buffer is zeroed once copied, it reads no more of it |
| CLI payload held for the deferred ring | `SecretString`, wiped when the slot drops it, run or not |
| `runCli` request envelope, enroll cert reply | `JsonDocument` on `secretJsonAllocator()`. ArduinoJson grows a string by realloc, so clearing the final string alone leaves the earlier blocks |
| `cert.set` and `secret.set` value buffers, `checkStoredClientCert` buffers | zeroize or `secretFree` before free. Shell `secret.set` strips its quotes in place, so no unwiped temporary holds the value |

`secret_heap.h` blocks carry their capacity in a header, so a free wipes
the whole block without asking the heap. A grow copies and wipes the old
block. A shrink stays in place and wipes the tail, so it never fails.

What it does not cover:

| Copy | Detail |
|---|---|
| framework copies outside this code | the `HTTPClient::getString` buffer behind the enroll reply, mbedTLS record buffers, NVS page buffers |
| a Lua subscription that receives the payload | the Lua VM owns its strings |
| the HTTP command surface | the `/api/cmd` `HttpBody`, its `JsonDocument`, `cmdStr` and the deferred-lambda copy, and the `/ws/serial` `String cmd` are plain heap, so a `secret.set` sent over HTTP leaves them unwiped |
| a secret kept in `config.json` | `config.set` and the config loader hold the whole file in plain heap copies |
| the Telegram Bot API request | `boundedPost` builds the request line in a plain `String`, with the `telegram_token` secret in the path, and every send frees it unwiped |

How enforced: `test/test_secret_heap` under a 95% floor, with a recording
`secretWipe`. Any new code that holds a key on the heap uses these helpers
or zero+free with `#include <mbedtls/platform_util.h>`.

Source: `lib/thesada-core/src/secret_heap.h`,
`lib/thesada-core/src/SecretJson.cpp`,
`lib/thesada-core/src/MQTTClient.cpp` (`connect`, `onMessage`,
`cliInboundHandler`, `runCli`, cert.set handler, `checkStoredClientCert`),
`lib/thesada-core/src/Shell.cpp::cmd_secret_set`,
`lib/thesada-core/src/Enroll.cpp::onCert`.

### Cert+key pair are validated before they are used, not at cert.set

`validateClientCertKey` parses both, then runs
`mbedtls_pk_check_pair(&crt.pk, &pk, ...)` against the cert's public
key before returning ok - the `crt` context is kept live through the
check for exactly that.

`cert.set` stores each half on its own, unchecked: the halves arrive in
separate messages, and the first half of a rotation never pairs with the
old other half still in NVS. The pair is judged where it is used:

| Path | Check | On a mismatch |
|---|---|---|
| enrollment reply (`Enroll.cpp::onCert`) | `clientCertPairValid` before anything is stored | reply refused, `enroll.reply_refused reason=cert_pair` |
| `cert.apply` | `checkStoredClientCert`, then `certApplyVerdict` | `ok:false`, no reboot, `_storedCertBroken` set, `shell.cert_apply_refused reason=pair_invalid` |
| `connect()`, WiFi and cellular alike | `validateClientCertKey` before the pair reaches `TlsClient` | no client material attached, password session, `_storedCertBroken` set, `mqtt.mtls_cert_invalid` |

The pairing flow in the app sends `restart`, not `cert.apply`, so for it
`connect()` is the gate. A mismatched pair is never handed to the TLS
stack on either transport.

A `cert.apply` whose check buffers will not allocate replies `ok:false`
with `shell.cert_apply_refused reason=low_heap`, does not reboot and leaves
`_storedCertBroken` as it was, the same rule `connect()` follows on
`mqtt.mtls_load_oom`.

How enforced: every path that installs or applies client mTLS material
calls `validateClientCertKey` first. `test/test_cert_policy` pins
`certApplyVerdict`: a stored pair that does not validate maps to Mismatch,
a check the heap could not run to LowHeap, a missing half to Missing. It
pins `storedCertBrokenAfter` too: Invalid sets the broken verdict, Valid
clears it, NoHeap keeps it. The call site in `Shell.cpp::cmd_cert_apply` and
the mbedtls pair check run on the target only and are checked by review.
The check is version-guarded -
`mbedtls_pk_check_pair` and `mbedtls_pk_parse_key` take RNG callback
args on mbedtls 3.x (pioarduino / IDF 5.x), the shorter forms on 2.x.

Source: `lib/thesada-core/src/MQTTClient.cpp::validateClientCertKey`,
`checkStoredClientCert`, `lib/thesada-core/src/Shell.cpp::cmd_cert_apply`,
`lib/thesada-core/src/cert_policy.h::certApplyVerdict`, `storedCertBrokenAfter`.
`validateClientCertKey` is fronted by `certKeyInputsUsable()` in
`lib/thesada-core/src/cert_policy.h`, a structural PEM pre-flight that is
strictly more rejecting than the previous non-null/non-empty guard. **The
pair check itself is unchanged and remains untested** - `cert_policy.h`
covers structure, CN extraction, the `cert.apply` verdict and the broken
verdict it records, not `mbedtls_pk_check_pair`.

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

### Loop-written state reaches async_tcp only as a copy taken under a portMUX

async_tcp runs at priority 10, unpinned, so it can run on the other core while
the main loop writes. Nothing it serves walks loop-owned state directly.

| State | Rule |
|---|---|
| `_stateDoc` | the loop serialises it after each update into a fresh heap buffer and swaps the `_stateJson` pointer under `_stateMux`, freeing the old one outside the lock (up to 16 KB). `/api/state` sizes, then copies under the same mux and parses its own document |
| Log ring | `Log::write` formats outside the lock and copies into the ring under `_ringMux`, the WS replay takes `Log::ringSnapshot` under the same mux. No caller holds a pointer into the ring |
| Setup result | `applyForm` builds into loop-only buffers, the loop publishes link and error under `_resMux`, the page reads them through `SetupPortal::result`. Flags are `std::atomic<bool>` |
| `/api/cmd` and config-write flags | `done`, `cmdOk` and `_restartPending` are `std::atomic<bool>`, so the reply buffer written before `done` is visible on the other core |
| Time strings | `gmtime_r` everywhere, `gmtime` shares one static buffer across tasks |

Critical sections only copy fixed buffers or swap pointers, never allocate or free.

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp` (`updateState`, `/api/state`, WS connect replay, `_restartPending`),
`lib/thesada-core/src/Log.cpp` (`write`, `ringSnapshot`), `lib/thesada-core/src/SetupPortal.cpp`.

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
because two GETs have one. `GET /api/ws/token` mints a single-use
WS grant and `/ws/serial` reaches `Shell::enqueue`, so a cross-site
call there is worse than any POST. A cross-origin page cannot read the
token from the reply, but Basic credentials are still refused there. `GET
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

### Auth-state deadlines count 64-bit uptime, never `millis()`

Bearer-token expiry (1 h), the WS grant (30 s) and the login
lockout (30 s) are persistent auth state. They are stored and compared
as 64-bit uptime ms (`esp_timer_get_time() / 1000`), which does not wrap,
so a plain `now < expiry` is exact.

`millis()` is not safe here, signed subtraction included. Nothing clears
a token or grant slot when it expires, and `(int32_t)(expiry - now) > 0`
turns true again from 2^31 to 2^32 ms after expiry: an expired token, or
an unused WS grant, validated for another 24.8 days unless a newer
entry reused the slot. Signed subtraction is only correct while every
compared value is within 2^31 ms of now, and a stale slot is not.

Expiry `0` is the empty / consumed sentinel. Every expiry is uptime plus
a nonzero TTL, so a live entry never lands on it. Token eviction compares
raw expiry values directly, which is correct once nothing wraps.

Transient sub-minute `while (millis() < deadline)` poll loops (cellular
AT, OTA pacing, shell drains) and the `_warnDue` log throttle are exempt:
worst case is one early timeout or one dropped warn at the wrap,
self-healing on retry.

How enforced: `ttlActive` (`ttl_policy.h`) takes 64-bit values and is
host-tested in `test/test_ttl_policy` past 2^31 and 2^32 ms after expiry;
the lockout lives in `login_rate_policy.h` (entry below). All three use
the one `_uptimeMs()` clock in `HttpServer.cpp`. New auth state stores
`uint64_t` deadlines from that clock; reviewers reject a `millis()` expiry.

Source: `lib/thesada-core/src/ttl_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_uptimeMs`,
`_createToken`, `_validateToken`, `_issueWsGrant`, `_consumeWsGrant`).

### `/ws/serial` opens only on a single-use token, checked at the handshake

`GET /api/ws/token` (authenticated) returns a random 128-bit token
(`esp_fill_random`, hex) valid for 30 s and bound to the caller's IP. The
`/ws/serial?token=` handshake spends it, compared in constant time over
every slot, so a token works once, from that IP, and two clients behind one
NAT each get their own. Four slots, and a full table answers 503 instead of
evicting a live grant. The check runs in `handleHandshake`, before the
library creates a WebSocket client, so a refused upgrade (401) never joins
`textAll` and gets no log line. The reasons are `no_token`, `bad_token` and
`shell_mode`. Before 2026-11-09 a token call pre-authorized the IP for its
next upgrade, and a refused socket briefly received broadcast lines.

How enforced: `ws_grant_policy.h`, host-tested in `test/test_ws_grant`
under a 95% floor (single use, wrong token or IP, expiry, two grants on one
IP, full table refusing without eviction, bad input). The dashboard checks
a saved login with `/api/auth/check`, never by minting a grant.

Source: `lib/thesada-core/src/ws_grant_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_issueWsGrant`,
`_consumeWsGrant`, `/ws/serial` handshake), `dashboard.html.h`.

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

### A POST body is buffered per request, only after auth, and capped

`/api/config`, `/api/cmd` and `/api/file` keep their body in
`req->_tempObject`, one `malloc`'d `HttpBody` per request that the request
destructor frees. `_bodyChunk` allocates it at chunk 0 only when
`_checkAuth` passes and the declared total fits the endpoint cap (config
16 KB, cmd 2 KB, file 64 KB). A chunk out of order or past the total
poisons it. So another client's chunk 0 can neither reset nor write into an
operator's upload, an unauthenticated `Content-Length` pins no heap, and the
handler acts only on a body that arrived whole. Chunk 0 authenticates through
`_authCounted`, lockout first, so a locked client allocates nothing. A wrong
credential there counts once and leaves a denied marker (`httpBodyDenied()`),
so a client that drops mid-body still spends the guess and completion does not
count it again. The marker records whether the client was already locked, so
the guess that starts a lockout answers 401 and the next one 429, as on every
other route. The `/ota` marker holds the chunk-0 `RouteAuth`, and only
`ROUTE_AUTH_OK` lets a chunk reach `Update`.

`_bodyOrReply` decides through `httpBodyVerdict`: 401 unauthenticated, 411
for a `Transfer-Encoding` (chunked) request or one with no `Content-Length`
header, then an empty body for `Content-Length: 0`, 413 past the cap, 400
when the body did not arrive whole. The 411 comes before the empty case:
a chunked request has Content-Length 0 and onBody total 0, so nothing is
ever buffered for it, and read as an empty body it used to truncate the
`/api/file` target. ESPAsyncWebServer 3.12.1 reads no body at all for a
request with neither header (`WebRequest.cpp`, end of headers goes straight
to `PARSE_REQ_END`), so that request read as `Content-Length: 0` the same
way. Only a declared `Content-Length: 0` writes an empty file.

`GET /api/file` streams the file from the filesystem whatever its size
(an `AsyncFileResponse` that owns and closes the `File`), so nothing is
clipped and no full copy sits in RAM. `HTTP_FILE_MAX` (64 KB) is the write
cap and the dashboard editor's load cap: the editor refuses to load a
larger file for editing and downloads it instead, so it never saves back a
partial read, and a POST over the cap gets 413 before anything is
buffered. The body costs no internal heap: every shipped env is an
ESP32-S3 with PSRAM (`BOARD_HAS_PSRAM`, `qio_opi`), and the arduino-esp32
3.3.12 sdkconfig sets `CONFIG_SPIRAM_USE_MALLOC` with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096`, so a body past 4 KB comes from
the 8 MB PSRAM. A unit whose PSRAM failed falls back to internal heap,
where a 64 KB malloc can fail, and that reads as 400 with nothing written.
A POST to the LittleFS `/config.json` keeps the 16 KB config cap that
`/api/config` has, checked in the shared `_configWriteOrReply`, so the two
routes accept the same documents.

The dashboard sends every file body as `application/octet-stream`.
ESPAsyncWebServer 3.12.1 treats a `text/plain` body whose first token runs
into `=` as a form post and never calls `onBody`, so a Lua script such as
`local t = 30` came back 400. The editor reads the source once per load,
so a source switch while a load is in flight cannot relabel what arrives,
and an upload over the loaded file clears the editor so Save cannot undo
it.

`/api/file` writes through `_fileWriteAtomic`. The body goes to
`.upload.tmp` in the target's own directory (`uploadTmpPath`), the tmp is
reopened and its size checked (`fileWriteLanded`, the same read-back rule
as `Config::writeFile`), then it replaces the target. LittleFS renames over
the target atomically. FAT cannot rename onto an existing name, so
`fatCommit` parks the old file as `.upload.bak`, renames the tmp in, and
removes the bak. If the rename fails the bak is renamed back, and if that
fails too both copies stay where they are and `web.file_restore_failed`
names them. No step deletes the last copy of either version, including
when an SD log append recreates the target between the steps. A target
name that FAT would match to a staging name (case-blind, trailing dots and
spaces dropped, so `.UPLOAD.TMP` or `.upload.tmp.`) is refused, because
staging over it would destroy it. The declared length is written, so a NUL
inside the body no longer cuts the file short. Same
chunk-before-completion ordering as the `/ota` rule above.

All live bodies share one budget, `HTTP_BODY_BUDGET` (one 64 KB file write
beside one 16 KB config push). Chunk 0 reserves its declared length after
auth, the request's disconnect gives it back, and a body that does not fit
is not buffered: the handler answers 503 and `web.body_refused
reason=budget` is logged. Before, each authed POST malloc'd its own cap with
no total.

How enforced: host tests in `test/test_http_body` over
`http_body_policy.h` (including the budget) and in `test/test_config_write` over
`config_write_policy.h`, coverage floor 95%. No body-taking handler keeps a
file-scope buffer.

Source: `lib/thesada-core/src/http_body_policy.h`,
`lib/thesada-core/src/config_write_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_bodyChunk`,
`_bodyOrReply`, `_fileWriteAtomic`, the three handlers),
`lib/thesada-mod-httpserver/src/dashboard.html.h` (`loadFile`, `saveFile`,
`copyFileToOther`).

### Bearer token comparison is constant-time

`_validateToken` compares via `_constTimeEq` (no early return) and
walks every token slot regardless of an early match, so neither the
per-byte compare nor the slot loop leaks token correctness via
response timing.

Source: `lib/thesada-mod-httpserver/src/HttpServer.cpp::_validateToken`.

### HTTP rate limiter: 5 fails -> 30 s lockout, 16 IP table plus one overflow entry, auto-reset on success

Five counted guesses (`webAuthCountsAsGuess`) from one source IP lock
every authenticated route for 30 s, with a 429. After that, one more
wrong guess re-locks. A success, or 10 min with no failure, forgets the IP.
Once all 16 slots hold live IPs, every other IP shares one overflow entry
with the same rules.

| Rule | Why |
|---|---|
| Time is 64-bit uptime (`esp_timer_get_time`), not `millis()` | a `millis()` deadline goes stale: `until = 0` read as locked for 24.8 d once uptime passed 2^31 ms, and a passed lockout came back 2^31 ms later |
| Fewer than 5 fails is always allowed | only a full count locks |
| A passed lockout keeps the entry at 4 fails | the next wrong guess re-locks at once: a single-IP brute force gets 1 guess per 30 s, not 5 |
| Success (`reset`) or `LOGIN_RATE_QUIET_MS` (10 min) with no failure clears the entry | an operator's old typos do not follow them; a quiet entry is a free slot |
| A guess while locked changes nothing | cannot extend the lockout or overflow the count |
| Full table: a new IP takes a free or quiet slot, else it counts on the overflow entry. No live entry is ever evicted | evicting reset the evicted IP's count, so 17 source addresses guessed without limit: 16 locked IPs gave up a lockout each, and two spare IPs alternating over one slot never locked at all. A released lockout is not free, it sits at 4 fails |
| While the overflow entry is locked, an IP with no slot gets 429 | the cost of a bounded table: an attacker holding 16 slots and the overflow keeps a new IP out, at 16 guesses per 10 min plus 1 per 30 s. An IP that already holds a slot keeps its own count. A slot frees on success or after 10 min quiet |
| A success never clears the overflow entry | it is shared, so one login must not hand every other unlisted IP fresh guesses |

How enforced: `LoginRateTable` (`login_rate_policy.h`), host-tested in
`test/test_login_rate` under a 95% floor, including both wrap cases, the
re-lock after a lockout, the quiet period, a full table of locked IPs, two
spare IPs thrashing one slot, and the overflow entry's lock, release and quiet
period. `HttpServer.cpp` only supplies the IP and the clock. Every route
handler authenticates through `_authOrReply` or `_bodyOrReply` (one count per
request), chunk 0 of a body or `/ota` upload counts through `_authCounted`, and
`scripts/check-route-auth.sh` in `make lint` matches identifiers outside `//`
comments in the HTTP server module, where every route lives, and fails on any
raw `_checkAuth` beyond its allowlist, each allowed line exactly once.

Source: `lib/thesada-core/src/login_rate_policy.h`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_rlAllow`, `_rlFail`,
`_rlReset`, `_uptimeMs`, `_authCounted`, `_authOrReply`), `scripts/check-route-auth.sh`.

---

## Alerts

### The Telegram relay publishes the app's alert JSON, inside the 256 B slot

`TelegramModule::send` publishes `{"severity":"warn","code":..,"message":..}`
on `EventBus("alert")`, `code` from the caller (`Telegram.broadcast(msg, code)`)
or `telegram`, cut to 32 bytes on a UTF-8 boundary (`alertUtf8Cut`). The app
drops any alert without a valid severity, so the old `{"value":..}` shape never
arrived. `alertScrubControls` turns control bytes into spaces, since ArduinoJson
writes unnamed ones raw, and `alertMessageFit` counts characters as ArduinoJson
writes them and cuts on a UTF-8 boundary, leaving room for the heap breadcrumb,
so the JSON fits the MQTT client's 256 B alert buffer and stays valid. A
`telegram.chat_ids` entry may be a string or an integer, since a group id is a
negative number, and an entry that is neither logs `reason=bad_chat_id`.
The app alert is published on every `send`, before any chat check, and
`Telegram.broadcast` returns true once it is out, so a caller never repeats an
app alert over a chat problem. The chat and webhook sends queue (four deep,
`reason=queue_full` past that, a message past 511 bytes cut on a UTF-8 boundary
and logged) and `TelegramModule::loop()` delivers one per pass, so no TLS
handshake runs inside the Lua, shell, MQTT or timer chain that asked for it.
Chat delivery is best effort, every skip logs `telegram.send_skipped`. The rules
example publishes the alert JSON itself when the `Telegram` global is nil.

How enforced: `alert_payload_policy.h`, host-tested in `test/test_alert_payload`
under a 95% floor.

Source: `lib/thesada-core/src/alert_payload_policy.h`,
`lib/thesada-mod-telegram/src/TelegramModule.cpp`.

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

`Shell::enqueue` takes the sink by value and moves it into the slot, so
the copy (which may allocate) happens on the caller's stack, outside the
lock.

How enforced: any new SlotMode or new payload type must follow the
move-then-exit pattern.

Source: `lib/thesada-core/src/Shell.cpp::loop`, `Shell::enqueue`.

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
instead of the main one - acceptable for debug operations. The pump runs
only on the task that ran `Shell::begin` (`shellPumpAllowed`). A call from
any other task returns at once, so a worker that reaches a pumping wait
never runs a line. None reaches one today: the modem hard reset, the
pumping wait a socket failure can start, is queued for the loop task.

How enforced: every new blocking loop with a per-iteration delay
>= 100 ms in core / cellular modules adds the pump call. Reviewers
check this on PRs that introduce new wait loops. Async/state-machine
refactors of these modules are out of scope - the pump call is the
contract.

Source: `lib/thesada-core/src/Shell.cpp::pumpConsole` and call sites
in `Watchdog.cpp` (every `waitStep`), `OTAUpdate.cpp`, `Cellular.cpp`,
`main.cpp`.

### The SD log pick is one root listing, never a 999-name probe

`SDModule::begin()` runs inside `setup()` after the 30 s task watchdog is
armed and before the first `loop()` feed. The active log is picked from
one listing of the card root (`_scanLogs`), parsed by `sd_log_policy.h`:
resume the highest `/logNNN.csv` under `sd.max_file_kb`, else claim the
lowest free slot. A directory named like a log takes its slot but is
never resumed (the listing's `d_type`). No wrap past log999. `exists()` still guards the
`FILE_WRITE` so an entry the listing missed is never truncated. A root
that will not open is `sd.log_scan_failed`, not a blind create.

Why: each `exists()` re-walks the FAT directory. Probing log999 down to
log001 took ~29 s on a 694-file card and boot-looped owb-debug on the
watchdog every ~58 s. Size-triggered rotation in `logEvent()` uses the
same one listing.

How enforced: `test/test_sd_log_policy` (694 files picks log694, gaps,
garbage names, a full set, an empty card); the coverage floor on
`sd_log_policy.h`.

Source: `lib/thesada-mod-sd/src/SDModule.cpp::_scanLogs`, `_openNextLog`,
`_resumeOrOpen`; `lib/thesada-core/src/sd_log_policy.h`.

### No boot step blocks past the task watchdog without feeding it

`setup()` arms the 30 s task watchdog (`TASK_WDT_TIMEOUT_MS`) before any
network or script work, and `loop()` is the only global feed. A step that
can outlast the period feeds inside its own bounded loop, once per
iteration, and only while its config-bounded exit is still ahead. Nothing
disables or widens the watchdog to make a step fit: an unbounded wait must
still reboot.

| Step | Unfed span | Total bound |
|---|---|---|
| WiFi connect (`tryConnect`) | one 250 ms poll | `wifi.retries` (1-5) x SSIDs in range (max 8) x `wifi.timeout_per_ssid_s` (1-60 s) |
| WiFi scan | one `scanNetworks()`, fed after | IDF scan |
| NTP wait after connect | one 200 ms poll | 15 s |
| MQTT connect (`MQTTClient::connect`) | dial: TCP 10 s + TLS 10 s, after a fed DNS resolve; then CONNECT write 10 s + CONNACK 5 s | one attempt per retry interval |
| OTA GET hop (`otaHttpGetWiFiHop`) | TCP 10 s + TLS 10 s + headers 5 s idle per line (a trickling origin can outlast it; the TWDT then reboots), after a fed DNS resolve | 5 same-scheme redirects; body loop fed per read, 5 min cap |
| Telegram and webhook POST (`TelegramModule::sendTo`, `broadcast`) | TCP 5 s + TLS 10 s + write 5 s + status line 5 s (a hard deadline, not an inactivity timer), after a fed DNS resolve; a failed resolve skips the send | one POST per chat and one webhook, fed after each |
| Any call from C into Lua (`luaBudgetedCall`): `main.lua` / `rules.lua`, EventBus, MQTT and `Node.setTimeout` callbacks, `lua.exec`, `lua.load` | 1000 VM instructions | 10 s of script time per outermost call, shared by the calls nested in it, then `script.budget_exceeded` aborts it. Time `Telegram.send`, `Telegram.broadcast` and `MQTT.publish` spend blocked is given back, at most 60 s per outermost call, so wall time can pass 10 s by those fed sends |

The poll steps go through one helper, `Watchdog::waitStep(ms)` (pump the
console, sleep at most 1 s, feed). Every call from C into Lua goes through
`luaBudgetedCall` (`lua_budget_hooks.h`), at boot and in steady state, so a
callback that never returns ends after 10 s instead of looping the task
watchdog on every event that fires it. The Lua count hook feeds until the
budget is spent and raises after. `pcall` and `xpcall` are wrapped: once the
budget is spent they re-raise after the protected call, so a caught budget
error climbs to `luaBudgetedCall`, which also fails a call whose budget ran
out even when the script returned normally. A binding that blocks in a send
gives that time back through `ScriptEngine::creditNative`, capped at
`LUA_NATIVE_CREDIT_MS` (60 s) per outermost call, so a slow Telegram send
does not cut off the rest of an alert callback and a script looping on sends
still ends. The budget counts script time, not the binding's own capped
POSTs. A nested call (a binding publishes an event a script subscribes to)
shares the outer budget, so re-entering Lua cannot restart the clock. The
hook is set once on the main thread before any coroutine exists, so every
coroutine inherits it, and it is idle between calls: a coroutine saved by one call runs under the budget of the call that
resumes it. `coroutine.resume` still returns the error to its caller, and
that caller's next instructions raise it again. OTA follows redirects by hand
(`HTTPC_DISABLE_FOLLOW_REDIRECTS`): HTTPClient's own redirect loop chains
hops with no feed, and core 3 DNS is uncapped (about 7 s per server).

Why: with two configured SSIDs out of reach the defaults alone held setup
for 40 s, and the NTP wait stacks 15 s on a connect. Each tripped the
watchdog before the first `loop()`. A Telegram broadcast with the internet
down and WiFi up takes 5 s per chat, so without the credit the first hook
tick after it aborted the callback before its MQTT alert.

How enforced: `test/test_boot_wait_policy` (every poll step under half the
period, clamps on every config key, the 40 s default case, rollover-safe
budget), `test/test_ota_verify` (redirect codes, scheme kept, relative and
junk Locations refused), and the coverage floor on `boot_wait_policy.h`. The
Lua budget: `test/test_lua_budget` (a nested call cannot restart the clock,
a caught overrun fails the call, the next call re-arms, rollover-safe, a
credit is capped per outermost call and never revives a spent budget), the
coverage floor on `lua_budget_policy.h`, `test/test_lua_budget_hooks` on the
host-built Lua fork (a runaway loop on a fake clock raises in a plain call,
in a coroutine created after setup, in a saved coroutine a later call
resumes and under `pcall`, credited native time is not script time, a loop
of slow sends ends), and `scripts/check-lua-entry.sh` in `make lint`, which
fails on a `lua_pcall`, `luaL_dostring`, `lua_resume` or `lua_sethook`
outside `lua_budget_hooks.h`, and when `createState` does not call
`luaBudgetInstall` ahead of `luaOpenLibs` or `callAllRegistrars` does not
wrap `pcall`. On
hardware (espressif32 8.0 bench, 2026-10-26): owb-debug and node boot
inside the watchdog with the SD pick and the bounded MQTT connect. Not yet
run on hardware: the WiFi and NTP outage paths, the Lua budget, OTA
redirects, and the Telegram cap.

Not covered: other C bindings that block inside a boot script carry only
their own timeouts; module `begin()` calls not listed above carry their own
caps (Cellular feeds, the SD pick is one listing).

Source: `lib/thesada-core/src/Watchdog.cpp`, `boot_wait_policy.h`,
`WiFiManager.cpp::scanAndConnect`, `tryConnect`;
`OTAUpdate.cpp::otaHttpGetWiFi`; `MQTTClient.cpp::connect`;
`TelegramModule.cpp::sendTo`, `boundedPost`; `http_url_policy.h`;
`ScriptEngine.cpp::createState`, `creditNative`, `lua_mqtt_publish`,
`lua_budget_hooks.h`, `lua_budget_policy.h`, `scripts/check-lua-entry.sh`,
`TelegramModule.cpp::begin` (the `Telegram` bindings).

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

### Config and EventBus are walked on the main loop only, other tasks read copies

`Config::_doc` (JsonDocument) and `EventBus::_subscribers` (an
`EventBusTable`, std::map inside) have no locking. Every call into either runs on the main-loop task:
Shell handlers, MQTT callbacks, ModuleRegistry init, module `loop()`s.
Cellular AT-bus work is on its own task but does not touch Config or
EventBus, and neither does the `net.http` fetch task.

The async_tcp task (priority 10, not pinned, so it can run on the other
core) serves every HTTP handler. It never calls `Config`. What the handlers
need from config (web user, the `web.password` fallback behind the NVS
secret, the device name) lives in a `WebSnap` copy that `HttpServer` fills
on the main loop. `Config::onChange` runs the refill at the end of every
`load`, `replace` and `set`, so a `config.set` typed into a console pumped
from inside an OTA download, a modem wait or `Watchdog::waitStep` applies
before that wait returns. `HttpServer::loop` compares `Config::revision()`
as a backstop. The copy is taken and read under a `portMUX` spinlock, so a
doc rebuild only ever leaves the handlers on the previous copy. The NVS
secret is still resolved per request on async_tcp, which NVS allows.

A credential is never clipped into the snapshot, since a clipped one would
accept a different credential. `webCredState` decides: a `web.user` past
63 bytes, or a config `web.password` past 159 bytes that is actually in use
(no NVS secret), is `WEB_CRED_TOO_LONG`. A long fallback behind a set NVS
secret is unused and locks nothing. Both lock states veto every
credential, Bearer included, answer 403 on `/api/auth/check` and
`/api/login` with their own reason, log `web.admin_refused
reason=credential_too_long` or `reason=default_password`, and are not
counted as guesses by the login lockout, because the right credential
gets the same refusal.

`/api/state` reads `MQTTClient::connectedCached()`, the main loop's last
`connected()` answer. `connected()` itself can `stop()` the TLS session
the main loop is using. HTTP writers that need `Config` run on the main
loop through the Shell deferred ring.

When a new module runs on a dedicated FreeRTOS task (likely BLE), it reads
a copy the same way or takes a recursive mutex modelled on `ATGuard` in
`Cellular.cpp`. Both headers (`Config.h`, `EventBus.h`) carry the
constraint above the class, and reviewers reject any other-task code that
calls into these singletons.

How enforced: `webSnapFill` and `webCredState` (`web_snapshot_policy.h`,
host-tested in `test/test_web_snapshot`) own the copy and lock rules. `HttpServer.cpp` has no
`Config::` call outside `_webSnapRefresh` and the deferred-ring lambda.

Source: `lib/thesada-core/src/Config.cpp` (`revision`, `onChange`),
`lib/thesada-core/src/MQTTClient.cpp` (`connectedCached`),
`lib/thesada-core/src/EventBus.cpp`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`_webSnapRefresh`,
`_webSnapRead`, `_webCred`, `_checkAuth`, `_sendCredLocked`, `/api/info`),
`lib/thesada-mod-cellular/src/Cellular.cpp::ATGuard`.

### Cellular AT bus is protected by a recursive mutex with RAII guard

`ATGuard` is a recursive mutex wrapper around the SIM7080 AT
serial line. Every AT exchange takes the guard for the duration of
its `sendAT` / `waitResponse` pair so URC parsing and socket reads
cannot interleave with command dispatch. A socket takes it per call, and
`available()` only tries it without waiting, so a transfer on one socket
never locks out the health probe or the other socket. The guard
has a `pause()` method that releases-sleeps-reacquires for long
backoffs so other tasks (Shell over serial, network timeout polls)
can preempt without starving.

Two tasks take the bus, the loop and `net.http`, so a guard can wait on
a hold longer than the 30 s task watchdog. Every wait is fed:

| Rule | Why |
|---|---|
| The constructor and `pause()` take the mutex in slices of 1 s at most (`waitSliceMs`) and feed the watchdog between them when the calling task is watched (`esp_task_wdt_status`). A zero timeout is still one try | the probe's 30 s default wait sat in one unfed take while `net.http` held the bus through a 40 s bearer recycle, and the TWDT panics at 30 s |
| Socket-failure recovery runs on the task that drives activation and the probe (`cellRecoverHere`). A streak on another task counts and queues its step (`cellular.socket_recover_queued`), and the next `Cellular::loop` probe runs it under its guard. A hard reset or a fresh bearer clears the queue | the bearer recycle (up to 40 s) and the hard reset (a 120 s guard, up to 60 s for AT) held the bus from `net.http` and changed `_started` under the loop's state machine |
| The open and send failure streaks are kept per socket slot (`CellFailStreaks`). A success clears only its own slot, and the power cycle one slot earns clears every slot | one shared open streak let an HTTPS success on slot 1 wipe the count of an MQTT slot 0 that kept failing, so it never reached the recycle |

How enforced: `test/test_boot_wait_policy` (slices walk any budget, fed)
and `test/test_cell_recover` (step per streak, reset over recycle, driver
task, one slot's success keeps the other slot's streak) under the 95% floors on `boot_wait_policy.h` and
`cell_recover_policy.h`. The fed take itself runs only on hardware.

Source: `lib/thesada-mod-cellular/src/Cellular.cpp` (`atTake`, `ATGuard`,
`socketRecover`, `Cellular::loop`), `lib/thesada-core/src/boot_wait_policy.h`,
`lib/thesada-core/src/cell_recover_policy.h`.

### A `millis()` deadline or span holds across the 49.7-day wrap

A wait is timed as a span from its start, `msWithin(millis(), t0, span)`,
which is `millis() - t0 < span`. A deadline kept in a variable is checked
with `msReached(now, at)`, the signed difference. No wait compares
`millis() < end`: an end set in the last minutes before the wrap lands past
zero, and the loop then ended at once, which aborted a cellular OTA, a
modem reset wait or a download near day 49.7. Spans run in the modem waits
(`waitChunked`, `modemSoftReset`, the AT exchange, the GPS fix and bearer
settle, the `httpsGet` 5 min cap), the OTA download cap, the chunk link
wait and pacing, and the heartbeat pulse. The awake window before deep
sleep is a deadline. A GNSS interval runs from the end of the previous read,
so a cold fix that timed out does not start the next at once.

How enforced: `test/test_elapsed` covers a deadline and a 5 min span that
start just before the wrap, under the 95% floor on `elapsed_policy.h`.

Source: `lib/thesada-core/src/elapsed_policy.h`,
`lib/thesada-core/src/OTAUpdate.cpp` (`otaHttpGetWiFiHop`,
`downloadBinaryChunkedCellular`), `lib/thesada-core/src/SleepManager.cpp`,
`lib/thesada-core/src/HeartbeatLED.cpp`,
`lib/thesada-mod-gnss/src/GNSSModule.cpp`.

### `net.http` runs TLS on its own task and keeps MQTT serviced

Both `net.http` transports run the fetch on the 12 KB `net.http` task:
WiFi as `HTTPClient` + `WiFiClientSecure`, cellular as the provider's
`httpsGet`, which is `TlsClient` (mbedTLS on the ESP32) over modem socket
slot 1. `cmd_http` waits, feeding the loop-task watchdog, pumping the
console and calling `MQTTClient::tick` once a second (`httpWaitTickDue`),
and gives the loop back at the transport's ceiling: 60 s on WiFi, 30 s on
cellular. A cellular GET's read loop stops 25 s after its connect began
(`httpsGet` `budgetMs`, checked on every pass of the read loop, so a
stalled header or body stops too) and prints `read stopped at the 25 s
budget`. The budget is not checked before the request is sent: DNS,
`CAOPEN` and the handshake keep their own bounds, so a slow connect can
still pass the wait. A fetch still running at the ceiling is handed to its
task (`httpJobAbandon`), which frees the job once its own bounds end the GET
(`httpJobFinish`). Nothing leaks and nothing is freed twice, whichever side
gets there first.

The cellular fetch subscribes its task to the task watchdog for the GET.
The modem socket and `TlsClient` feed it, and IDF 5 logs `task not found`
at ERROR, inside a critical section, for every feed from a task it does
not watch. The WiFi fetch does not feed and stays unsubscribed.

The tick keeps the broker session alive through the wait. On cellular a
keepalive ping falls due 30 s after the device's last packet, goes out by
the next tick, and its slot-0 write waits at most 10 s for the AT bus
(`ModemClient::write`), so it reaches the broker inside the 45 s grace.
The PINGRESP is read within a second of its arrival, so an outstanding
ping never meets the next keepalive check. `MQTTClient::tick` does nothing while another
call into the client is under way (`ClientCall` in `loop`, `publish`,
`publishRetained`, `dropSession` and `tick`): a modem hard reset inside a
slot-0 write pumps the console, and a `net.http` typed there must not
re-enter PubSubClient or its TLS session.

With the fetch off the loop task, three more rules hold:

| Rule | Why |
|---|---|
| One `Cellular::httpsGet` at a time owns slot 1 (`HttpSlotClaim`). A second caller, from any task, gets false and status 0 with `cellular.https_get_skipped reason=busy`. A chunked OTA download waits out a busy slot (`Cellular::httpsBusy`) for up to 90 s before each try | OTA on the loop task, a console `ota.check` or a second `net.http` would otherwise run two TLS sessions over one modem socket. A console `net.http` typed in the chunk gap would otherwise spend all three chunk tries within a second |
| `Shell::pumpConsole` runs only on the task that ran `Shell::begin` (`shellPumpAllowed`) | the console line buffer and the shell nesting depth belong to one task, and a worker that reaches a pumping wait must not run lines on them |
| The fetch's socket failures queue their recovery for the loop (see the AT bus section above) | the recycle and the hard reset are long holds that change the state machine the loop drives |

Why: the cellular branch ran `Cellular::httpsGet` inline on the loop task,
on the belief that the modem did TLS. Since 2026-11-04 it is `TlsClient` on
the ESP32, so the handshake ran at the depth the WiFi branch was moved off
the loop task for (loop -> `Shell::loop` -> `runCli` -> `execute` ->
`cmd_http` -> `TlsClient`). The loop also stopped for the whole GET, up to
the 5 min `httpsGet` cap, and the broker drops a cellular session after
45 s (1.5 x the 30 s keepalive).

Limits: a slot-0 write that waits past its 10 s bus guard fails, and the
TLS write timeout then closes the MQTT session. The fetch task holds the
bus that long only through a slow DNS lookup (up to 18 s) or socket open
(`CAOPEN` 15 s between two 3 s `CACLOSE`), so a ping whose write meets
more than 10 s of such a hold drops the session and the loop reconnects.
A tick that writes holds the wait for its bus wait, so the loop can come
back that much past the ceiling. The tick runs the PubSubClient read and
its callbacks below `cmd_http` on the 8 KB loop task. OTA passes no read budget, so its
`httpsGet` header phase still has no idle bound: a server that accepts TLS
and never answers holds an OTA GET, and the loop, until the 5 min cap.

How enforced: `test/test_http_fetch` (preview cap, read budget, a 1 s MQTT
tick across the wait and the `millis()` wrap, a ticked ping inside the
broker grace where an unticked wait is not, job hand-off in either order,
slot claim) under the 95% floor on `http_fetch_policy.h`, and
`test/test_shell_line` (`shellPumpAllowed`). Not covered: the task split,
the watchdog subscription, the `ClientCall` guard and both stack margins
run only on hardware. Bench check on owb-debug with WiFi down: `net.http`
from the MQTT CLI against a large body answers within about 30 s, the MQTT
session stays up (no `mqtt.state_change ... to=disconnected`),
`shell.http_wait ... mqtt_ticks=` counts about one per second,
`loop_stack_free=` shows the loop-task margin with the tick, and
`shell.http_done ... stack_free=` the fetch task's.

Source: `lib/thesada-core/src/http_fetch_policy.h`,
`lib/thesada-core/src/Shell.cpp` (`httpFetchTask`, `httpFetchRunCellular`,
`cmd_http`, `pumpConsole`, `begin`),
`lib/thesada-core/src/MQTTClient.cpp` (`tick`, `ClientCall`),
`lib/thesada-mod-cellular/src/Cellular.cpp` (`httpsGet`, `httpsBusy`),
`lib/thesada-core/src/OTAUpdate.cpp::downloadBinaryChunkedCellular`.

---

### Cellular activation is incremental - one phase per loop tick

Cellular bring-up must not run as a single blocking call. `Cellular::
tickActivation()` advances at most one phase (`POWER_ON`, `SIM`,
`RADIO_CFG`, `REGISTER`, `BEARER`) per call and returns
`PENDING`; `CellularModule` polls it each loop while ACTIVATING so
every other module loop keeps ticking between phases. Registration is
polled ~1 Hz, never as one 30-180 s blocking wait.

The whole cycle is bounded by `cellular.activation_timeout_ms`
(default 30 min): on expiry the modem is hardware-reset and
`tickActivation()` returns `FAILED`, dropping the module to STANDBY -
a wedged modem can never pin the device in ACTIVATING. The broker is
MQTTClient's business: an unreachable one retries with backoff on the
cellular socket like it does on WiFi.

Why: the old blocking `Cellular::begin()` froze MQTT keepalive,
sensors, Telegram and the shell for 30-120 s during a WiFi-to-cellular
failover, exactly when remote intervention matters most.

How enforced: `CellularModule` ACTIVATING calls only
`tickActivation()` - never a blocking bring-up. Steady-state recovery
uses the same path: when the `Cellular::loop()` health probe sees the
network or bearer gone it clears `_started`, and ACTIVE drops to
ACTIVATING on the next tick. No blocking bring-up exists.

Source: `lib/thesada-mod-cellular/src/Cellular.cpp::tickActivation`,
`lib/thesada-mod-cellular/src/CellularModule.cpp::loop`.

---

## Lua sandbox

### No `io`, `os`, `debug`, `package` or code loaders

`createState` sets `io`, `os`, `debug`, `package`, `require`, `dofile`,
`loadfile`, `load` and `loadstring` to nil once the libraries are open, so
`lua.exec` from the MQTT CLI reaches no file, process or new chunk. Every
call from C into Lua is protected (`luaBudgetedCall`), so a script error never
crashes the host firmware.

Source: `lib/thesada-mod-scriptengine/src/ScriptEngine.cpp::createState`.

### `string`, `table` and `math` are the library tables

The Lua fork's `luaopen_string`, `luaopen_table` and `luaopen_math` set
their global and then return the name argument, so `luaL_requiref`, and the
fork's own `luaL_openlibs`, store the strings `"string"`, `"table"` and
`"math"` as the globals. `createState` opens the libraries through
`luaOpenLibs`, which runs each of the three openers again and stores the
table in the global and in the registry's loaded table. With the name
strings, `table.concat` and `math.floor` fail with `attempt to call a nil
value`, and `string.format` works only because a string indexes the string
library. A script meant to run on older firmware too uses string methods
and no `table` or `math` calls, as the rules example does.

How enforced: `test/test_lua_stdlibs` builds the fork on the host
(`[env:native-lua]`, run by `make test-native`). It checks that the fork's
own `luaL_openlibs` still leaves the names, and that after `luaOpenLibs` the
globals are tables, `table.concat`, `table.insert`, `math.floor` and
`string.format` work, method syntax still works and the loaded table holds
the global. `tests/test_firmware.py` runs the same check on a device.
`tests/lua/test_rules_example.lua` runs the rules example with the three
globals set to their names.

Source: `lib/thesada-mod-scriptengine/src/lua_stdlibs.h`,
`ScriptEngine.cpp::createState`.

### Lua time does not wrap at 49.7 days

`Node.uptime()` and `Node.setTimeout` deadlines both count
`esp_timer_get_time()` in ms, 64-bit, so a script's `now - stamp` never goes
negative after the `millis()` wrap and a timer fires on a plain
`now >= fireAt`; the cooldown and sustain math in `rules.lua.example` relies
on that. The Lua build is `LUA_32BITS`, so uptime reaches a script as a float:
monotonic, but its step grows with uptime (512 ms past 49.7 d, 2 s past
199 d), fine for second-scale cooldowns. The delay is a Lua float and is
never cast raw: NaN is refused (`setTimeout` returns false,
`script.timer_refused`), a negative delay fires next pass, and anything past
30 days (`math.huge`, above `INT_MAX`) is capped there. A 32-bit deadline set
across the wrap fired on every loop pass, and a raw `(int)` cast of a huge
delay was undefined behaviour.

How enforced: `test/test_lua_timer` covers a 1 h timer set 30 min before the
32-bit wrap, uptime past the wrap, NaN, negative, infinite and above-`INT_MAX`
delays.

Source: `lib/thesada-core/src/lua_timer_policy.h`,
`lib/thesada-mod-scriptengine/src/ScriptEngine.cpp`
(`lua_node_uptime`, `lua_node_setTimeout`, `ScriptEngine::loop`).

---

## MQTT

### The subscription table's `active` flags and its slot count clear together

`MqttSubTable::reset()` is the only way to empty the table, and it clears both.
`removeOwner()` frees one owner's slots in place and gives back the count of
trailing free slots, and `add()` takes the first free slot before it grows the
count, so a freed slot in the middle is never stranded.
Clearing `active` alone leaves the count high: the freed slots stay skipped by
dispatch, and the next `subscribe()` writes past them. That is what happened:
`MQTTClient::begin()` cleared the flags only, so the `ota.cmd_topic` subscription
that `OTAUpdate::begin()` had registered in slot 0 was stranded inactive for the
whole boot, and publishing to it did nothing until a `mqtt.topic_prefix` change
forced a `reinitSubscriptions()`.

Registration order is fixed by heap, not preference: `main.cpp` runs
`OTAUpdate::begin()` before `MQTTClient::begin()` so the boot manifest fetch gets
a contiguous heap. MQTT therefore inherits a non-empty table, resets it, and
calls `OTAUpdate::registerCommandTopic()` to put the topic back - before the CLI
block, which can return early on a truncated prefix. That function re-reads
`ota.enabled` rather than the `_enabled` flag, so the second call site cannot
hand a remote OTA trigger to a device whose config has OTA switched off.

Every slot carries an owner. `MQTTClient::begin()` and `reinitSubscriptions()`
free the core slots and register them again under the current prefix, and
keep the Lua slots, which hold whole topics the script chose. A Lua reload
(`ScriptEngine::reload`) frees the Lua slots, unsubscribes each topic no other
slot still holds and lets the new scripts subscribe again, so reloads never
fill the table. Slots are marked free, never moved, so a reload that runs
inside `dispatch()` (the MQTT reload topic) leaves the walk on a stable
table. EventBus entries carry an owner too (`event_bus_table.h`): a reload
drops the Lua callbacks, and a drop inside `publish()` skips the dropped
entries and sweeps them once the outermost publish returns. Before, a reinit
killed every Lua subscription until reboot, and each reload left its MQTT
slots and EventBus callbacks behind.

How enforced: the table is a type with no public way to clear one field
(`mqtt_sub_table.h`, host-tested in `test/test_mqtt_sub_table` under a 95%
floor, including ten reloads, a core reinit beside Lua slots and slot reuse).
`test/test_event_bus_table` covers ten reloads, a drop and a subscribe from
inside a publish, under a 95% floor on `event_bus_table.h`. Anything that
empties the table calls `reset()`. `mqtt.diag` prints
`subs: n/max` and the active topics, which is where a stranded subscription
shows.

Source: `lib/thesada-core/src/mqtt_sub_table.h`,
`lib/thesada-core/src/event_bus_table.h`,
`lib/thesada-core/src/MQTTClient.cpp` `begin()` / `reinitSubscriptions()` /
`unsubscribeOwner()`, `lib/thesada-core/src/OTAUpdate.cpp::registerCommandTopic`,
`lib/thesada-mod-scriptengine/src/ScriptEngine.cpp::reload`.

### One MQTT session, one subscription table, on every transport

`MQTTClient::subscribe` writes to a single subscription table and every
session, WiFi or cellular, resubscribes all of it on connect. There is no
per-transport cap: the modem only carries TCP bytes.

Source: `lib/thesada-core/src/MQTTClient.cpp::subscribe`, `resubscribeAll`.

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
overflow the page it ships the current page with `"more": true` and
starts a fresh one. The final page carries `"more": false`. Every page
carries a 0-indexed `"page"` field. Single-page output - the common
case - is `page: 0, more: false`, the same shape a consumer that
ignores both fields already sees.

| Rule | Why |
|---|---|
| a line no page holds goes out in pieces, one page each. A page whose last `output` string continues on the next page carries `"cont": true`, and a consumer joins those two strings with no separator. A piece never ends inside an escape or a UTF-8 sequence | `config.dump` prints the whole config as one line. Past a page it was serialized into the fixed buffer and clipped to invalid JSON, so the app could not parse the reply at all. A piece cut mid-character decodes to U+FFFD |
| a page is sized to `cliReplyCap`: `mqtt.buffer_out`, or what PubSubClient's one buffer (`mqtt.buffer_in`) holds beside the 7-byte header and a 159-byte topic, whichever is smaller. The page limit (`cliPageLimit`) is 160 bytes less, and `cliNextPiece` keeps a page with its strings and `cont` within it: 3 bytes per string for quotes and comma, 12 for `cont` when the piece splits. The 160 bytes hold `more`, a last page's `ok: false` and the NUL | PubSubClient publishes from the `buffer_in` buffer, so a page sized to a larger `buffer_out` failed to publish and was lost |
| `publishCliResponse` measures the reply first and refuses one that does not fit (`mqtt.cli_response_refused reason=oversize`), never clips it. A line that cannot start on an empty page (a `req_id` that fills it) is dropped with `mqtt.cli_line_dropped` and the last page says `ok: false` | clipped JSON reaches nobody intact, and a log line says why the reply is missing |
| replies go out through `MQTTClient::publishNow`, outside `mqtt.send_interval_s`, and are queued only with no session up | `publish` sent anything inside the interval to the offline queue, which refuses 256 bytes or more, so every page after the first was dropped |
| chunked `fs.cat` measures the reply around empty `data`, sends only the bytes whose escaped form fits and puts that count in `length`. `data` is set by length, so a NUL byte is sent as `\u0000`. A chunk that cannot fit one byte, or a failed read, replies `ok: false` | 2048 quote bytes escape to 4096 and clipped past the buffer. Both consumers resume at `offset + length`, so a short chunk is safe, and a zero `length` would make them ask for the same chunk forever |

The JSON sizes come from `cli_page_policy.h`, which models ArduinoJson
7.4 `TextFormatter`: `"`, `\`, `\b`, `\f`, `\n`, `\r`, `\t` take two bytes,
NUL six, every other byte one. Other control bytes go out raw, as
ArduinoJson writes them.

How enforced: the general path publishes only through the paginating
sink in `runCli`, which takes every piece size from `cliNextPiece`, and
every reply only through `publishCliResponse`. `test/test_cli_page` runs
a model of that sink loop over pages serialized in `resp`'s key order:
every page with `cont` is within `cliPageLimit`, every page with `more`
and `ok: false` is under `cliReplyCap`, and the pieces join back to the
lines. It covers a 6 KB quote-heavy `config.dump` line at 4096 and 1024
byte buffers, every line length around a page boundary, UTF-8 text and a
`req_id` that leaves no room. It fails with the `cont` reserve at 0, the
margin at 0 or the string overhead at 2. 95% floor.
Not covered: the sink loop itself and `fs.cat` run only on hardware.

Source: `lib/thesada-core/src/MQTTClient.cpp::runCli`,
`lib/thesada-core/src/cli_page_policy.h` (`cliNextPiece`, `cliPageLimit`).

### The shell command line in `runCli` is never silently truncated

The general path builds the shell input as
`snprintf(line, sizeof(line), "%s %s", cmd, payload)` into a fixed
`line[SHELL_LINE_MAX]` (1024). The `snprintf` return is captured in `lineN`; if it is
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
guard runs before the line reaches `Shell::execute`. The shell parser
buffer is the same `SHELL_LINE_MAX`, so a line this guard passes is
never cut there (next entry).

Source: `lib/thesada-core/src/MQTTClient.cpp::runCli`.

### The shell refuses a line or a token it cannot hold, never clips it

| Entry | Holds | Over the limit |
|---|---|---|
| `Shell::execute` (MQTT cli, `/api/cmd`, and every path below) | `_parseBuf[slot][SHELL_LINE_MAX]`: 1023 bytes, 16 words (`SHELL_ARGS_MAX`); a rest-of-line value is one word | replies `Line too long (max 1023 bytes) - not run` or `Too many arguments (max 16 words) - not run`, runs nothing |
| `Shell::enqueue` (WS terminal) | ring slot `DEFERRED_LINE_LEN`: 255 bytes | returns false; the WS handler checks first and replies `[line too long - not run]` |
| `Shell::pumpConsole` (serial) | 255 bytes | discards to the newline, replies `Line too long (max 255 bytes) - not run` plus the done marker |
| shell `config.set`, `config.get`, `config.del <a.b>` | key `key[128]`: 127 bytes | `Key too long (max 127 bytes)` |

The parser copied into a 256-byte buffer and kept 16 tokens, silently.
An MQTT `config.set wifi.networks [...]` of about 300 bytes lost the end
of its JSON, the literal parse failed, the stub was stored as a string
and the reply said saved; a 17th word was dropped and the command ran on
the rest. The parser buffer matches the 1024 `runCli` already accepts,
since the app's config editor pushes JSON values through `cli/config.set`;
that costs 768 bytes of `.bss`. The WS and serial buffers stay at 256:
their input is typed, and they now refuse rather than grow.

How enforced: `shell_line_policy.h` (`shellSplitLine`, `shellFits`) is the shell
command-line tokenizer, host-tested in `test/test_shell_line` under a 95%
coverage floor. Any new fixed-buffer copy of a shell argument checks
`shellFits` first.

Source: `lib/thesada-core/src/shell_line_policy.h`,
`lib/thesada-core/src/Shell.cpp` (`execute`, `enqueue`, `pumpConsole`,
`cmd_config_set`, `cmd_config_get`),
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`/ws/serial`).

### A shell command reports failure, nests one level, and keeps its value as typed

`Shell::execute` returns a `ShellResult`. A refused line, an unknown
command, or a handler that called `Shell::fail` is `Failed`, and
`runCli` and `/api/cmd` reply `ok:false` with the output. `runCli`
sets it on the last page (`more:false`); a page shipped mid-command
says `ok:true`. Handlers that fail this way: `config.set`, `config.del`,
`config.save` (and their shared write), `secret.set`, `lua.exec`.
Other handlers still reply `ok:true` on their own error text.

Each nesting level parses into its own buffer (`SHELL_NEST_MAX` = 2): the
outer command, and one console line run while it blocks (OTA, `net.http`
and cellular waits call `pumpConsole`). A third level is refused with
`Shell busy (nested command) - not run`. `pumpConsole` resets its line
before it runs one, so a line typed during the run starts clean. The second
buffer costs 1024 bytes of `.bss`.

A command registered with `restFrom` gets `argv[restFrom]` as the rest of
the line, inner spaces and tabs kept, trailing ones cut: `config.set`,
`secret.set`, `fs.write` and `fs.append` from argv[2], `cell.at`,
`lora.send` and `lua.exec` from argv[1]. A value that must end in
whitespace is quoted (`config.set` and `secret.set` strip one pair).

Why: every error went out as `ok:true`, so the app's config editor showed
"saved" for a rejected `config.set`. One static parse buffer let a nested
console command overwrite the outer argv. The value was rebuilt from argv
with single spaces, so a WiFi passphrase with two spaces or a tab was
stored altered while the reply said saved.

How enforced: `test/test_shell_line` (`shellDispatchResult`,
`shellParseSlot`, `shellSplitLine` with `restFrom`) under the 95% floor
on `shell_line_policy.h`. Not covered: the transports' mapping and the
nested console path run only on hardware.

Source: `lib/thesada-core/src/shell_line_policy.h`,
`lib/thesada-core/src/Shell.cpp` (`execute`, `fail`, `pumpConsole`),
`lib/thesada-core/src/MQTTClient.cpp::runCli`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`/api/cmd`).

### The offline publish queue rejects oversized messages, never truncates

`enqueue()` refuses a topic or payload that does not fit the
`MQTTMessage` buffers (topic 160, payload 256 - sized to cover the alert
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

### A config key the shell cannot hold is refused, never truncated

Shell `config.set` (and a dotted `config.del`, which runs through it) and
`config.get` copy the dot-path into a fixed `key[128]`. A key past 127
bytes is refused with "Key too long" before the copy instead of being
truncated: a silent truncation would resolve to a *different* key than the
operator named and write the value there, corrupting config from a remote
command. Config-write counterpart to the runCli line-length and pagination
guards above. There is no other dot-path writer: `Config::set` had no
caller and was removed 2026-11-05.

How enforced: the `shellFits(argv[1], sizeof(key))` check runs before the
`strncpy` into `key`. Any new fixed-buffer copy of an operator-supplied key
adds the same length check. The MQTT binary handlers apply the same rule:
`fs.write` rejects a path that would not fit `path[64]` and `fs.cat`
rejects args longer than `pbuf[256]`, publishing an error rather than
acting on a clipped path/range. `secret.set` and `cert.set` likewise
reject an over-long `<field>` / `<type>` before the `\n` rather than
clipping it - a clipped length makes the value/PEM length too large and
reads past the payload end.

Source: `lib/thesada-core/src/Shell.cpp::cmd_config_set`, `cmd_config_get`,
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

### An older `/config.json` is migrated at boot, all or nothing

`config.json` carries `"schema"`, absent reads as 0. After a clean parse,
`Config::load` runs the ordered migrations in `config_schema_policy.h` on a
copy of the doc. Each one is a pure, idempotent function from schema N to
N+1 and logs `config.schema_step`. Only when every step ran does it copy the
old file to `/config.json.bak` (tmp, rename, size read back) and write the
migrated doc through `Config::writeFile`. The live doc switches to the
migrated copy only after that write landed.

| Case | Result |
|---|---|
| schema is current | nothing written |
| older, backup and write land | migrated doc on disk and live, `.bak` holds the old file, `config.schema_migrated` |
| backup or write fails | live doc and file stay unmigrated, `config.schema_migrate_failed`, app alert `config_schema` |
| schema newer than the firmware knows | untouched, `config.schema_newer`, app alert, runs with the keys it understands |
| `schema` not a whole number | untouched, `config.schema_invalid`, app alert |

The alert goes out through `EventBus("alert")` after `ModuleRegistry::beginAll`,
so MQTT queues it until the first session. A migration never runs on a
malformed file (that case is the entry above).

| Schema | Migration |
|---|---|
| 0 to 1 | `drop_colon_keys`: a key saved with a trailing colon (`"password:"`, `"user:"` on the 2026-04 OWB config) is dropped when the plain key exists, else renamed to it, in every object and array |

A new migration bumps `CONFIG_SCHEMA_CURRENT`, appends to
`CONFIG_MIGRATIONS` and gets a host test. How enforced:
`test/test_config_schema` under a 95% floor.

Source: `lib/thesada-core/src/config_schema_policy.h`,
`lib/thesada-core/src/Config.cpp::load`, `src/main.cpp`.

### `Config::replace` and the shell config writers never report success on a failed persist

`Config::save()` returns `false` when `/config.json` cannot be opened
or the serialize comes up short of `measureJson` (LittleFS full,
truncated write). `replace()` then logs `config.replace_failed
reason=persist` instead of "replaced" and reloads the on-disk file, so
the in-memory `_doc` is back at the last persisted state, never a value
that was never written. The shell writers (`config.set`, `config.del`,
`config.save`) reply `ok:false` on a failed `shellConfigWrite` and
reload nothing, so the live doc stays as it was before the command. A config write that
did not hit flash must never surface to the operator as applied -
otherwise a full filesystem silently discards every change while the
device reports success.

How enforced: every caller that persists config checks the write's
return before logging or returning success; a failed persist in a
mutate-then-save path reloads the persisted state.

Source: `lib/thesada-core/src/Config.cpp::save`, `replace`,
`lib/thesada-core/src/Shell.cpp::shellConfigWrite`.

### `/config.json` is never truncated in place by a config writer - tmp + rename (fs.write / fs.append excepted, see Gap)

Opening `/config.json` with `"w"` truncates the last good config before
the new bytes are known to fit. A short write (LittleFS full, flash
error) then leaves partial JSON on disk, and the `load()` rollback path
parses that corpse and wipes the live in-memory config too - persistent
loss of WiFi/MQTT/prefix from one full filesystem. Every config writer
therefore serializes to `/config.json.tmp`, closes it, reopens it and
checks the size that landed, then `LittleFS.rename()`s over the original
(lfs rename atomically replaces the destination). On any failure the tmp
is removed and the original is untouched.

The read-back is the check that counts. Arduino `File` writes go through
stdio, fully buffered at 4096 bytes (`vfs_api.cpp` `setvbuf`), so the
serializer's count is met before a byte reaches lfs. The lfs write and sync
run inside `fclose`, and `VFSFileImpl::close` drops its result. A full
filesystem there leaves an empty or partial tmp that the count alone would
rename over the last good config. `configWriteLanded(written, onDisk,
expected)` (`config_write_policy.h`, host-tested in `test/test_config_write`)
renames only when all three agree on a non-empty file, and `writeFile`
reports the on-disk size.

How enforced: `Config::writeFile(doc, pretty)` is the one tmp + rename
writer. It writes only the document it is handed and never touches the
live doc. Its callers:

| Caller | Writes | Live doc |
|---|---|---|
| `Config::save()` (portal, enrollment, MQTT rollback, `Config::replace` for MQTT `cmd/config`) | the live doc, held boot prefix swapped out | is the source |
| `Shell.cpp::shellConfigWrite` | config.set / config.del: an edited copy of the file. config.save: a copy of the live doc | config.set / config.del reload it |
| HTTP `POST /api/config` | the request body | untouched, restart on success |
| HTTP `POST /api/file`, `source=littlefs`, path resolves to `/config.json` (403 in a narrowed `shell.mode`, use `/api/config`) | the request body | untouched until restart or `config.reload` |
| temperature discover / prune | an edited copy of the file | untouched until restart or `config.reload` |

Any new code path that persists config routes through `writeFile`, never
a direct `LittleFS.open("/config.json", "w")`. `configPathIsLive`
(`config_write_policy.h`, host-tested in `test/test_config_write`) decides
which `/api/file` paths are the config file. It counts `"."` segments
(`/./config.json`) because littlefs skips them, and does not count a path
that goes on past the file (`/config.json/`), because littlefs 2.10 and
later answers NOTDIR there. The owb build links esp_littlefs 1.22.3
(framework-arduinoespressif32-esp32s3-libs 3.3.12), whose
`lfs_dir_find` is the 2.10 path walk.

Every writer shares `/config.json.tmp`, so `writeFile` runs only on the
main loop. Two writers on two tasks would interleave their bytes in the
one tmp and rename a mix over `/config.json`. The HTTP writers parse-check
on the AsyncTCP task and run `writeFile` from the Shell deferred ring,
waiting up to 5 s like `/api/cmd`.

Before any write, 401, 411, 413 and 400 "body not received" come from the
body rule ("A POST body is buffered per request, only after auth, and
capped"). `/api/file` also answers 403 in a narrowed `shell.mode`, 400 for
a missing or invalid path, and 413 for a config body past 16 KB. The write
step replies:

| Reply | Meaning |
|---|---|
| 400 | body is not one JSON object, nothing written |
| 503 | Shell deferred ring full, nothing written, safe to retry |
| 504 | main loop did not drain in 5 s, the write may still land (and `/api/config` then restarts) |
| 500 | the write failed, the old file is intact |
| 200 | the new file is on disk. `/api/config` restarts, `/api/file` reports the bytes on disk |

Temperature discover edits the file on disk, not the live doc, because
`fs.write` can leave the file ahead of the live doc until a restart. A
file that exists but does not parse as an object is never overwritten.

Gap: `fs.write` / `fs.append` (Shell `cmd_write` and the MQTT CLI binary
handler in `MQTTClient::runCli`) still open any path that passes
`pathSafe` with `"w"` / `"a"`, `/config.json` included, and write in
place. The app's config editor pushes `/config.json` that way as an
`fs.write` plus `fs.append` chain of 3900-byte chunks, so a chunk that
fails mid-chain leaves a truncated file and the next boot runs defaults.
`fs.mv` onto `/config.json` is an lfs rename and replaces it atomically.

Source: `lib/thesada-core/src/Config.cpp::writeFile`, `::save`,
`lib/thesada-core/src/config_write_policy.h::configWriteLanded`,
`lib/thesada-core/src/Shell.cpp::shellConfigWrite`,
`lib/thesada-mod-httpserver/src/HttpServer.cpp::_configWriteOrReply`,
`lib/thesada-mod-temperature/src/TemperatureModule.cpp::commitConfig`.

### Dashboard / shell HTML output is escaped via the browser's serializer

Dashboard XSS path uses `_escEl.textContent = s; return _escEl.innerHTML`
to escape any user-influenced string before it lands in HTML. Avoids
hand-rolling escape tables.

Source: `lib/thesada-mod-httpserver/src/dashboard.html.h` `esc` helper.

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
fires only on `MQTT_CONNECT_FAILED` (TCP/TLS never came up) after the
broker name resolved; an rc >= 1 means the broker answered, the heap was
sufficient, and a defrag fixes nothing. A failed lookup never reached TLS,
so a DNS outage never spends the budget.

Both reboots go through `OTAUpdate::brokerRestart`. On a confirmed image
that has reached the broker since it was installed they do not count
toward the OTA crash streak, so a long outage cannot switch slots. On a
pending image, or one that never connected, they count, so a new image
that cannot reach the broker still escapes.

How enforced: BOTH reboot branches in `connect()` are gated by
`!_rebootHalted` and the `mqttRebootCount()` budget check. The fast-reboot
gate is `mqttDefragRebootDue()` in `mqtt_rollback_policy.h`, host-tested in
`test/test_rollback` (the DNS-failure case included). Any new
reboot trigger in this file must justify why it cannot loop.

Source: `lib/thesada-core/src/MQTTClient.cpp::connect`.

### A critical mqtt config that never connects rolls back to the last-good snapshot

The connection-critical keys (`broker`, `port`, `user`, `password`)
snapshot to NVS (`thesada-boot`, `mqtt_lg`) on every successful connect
whose dialed host and port are the configured ones (`mqttSnapshotProven`).
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
`snapshotGoodConfig()` checks `mqttSnapshotProven()` first, same test file.

Source: `lib/thesada-core/src/mqtt_rollback_policy.h::mqttRollbackShould`, `mqttSnapshotProven`,
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

### `net.*` shell commands and MQTT reach cellular only through the `Net` provider hook

`net.ip` / `net.ping` / `net.ntp` / `net.http` and `MQTTClient` live in `thesada-core`,
which must not depend on the optional cellular module. The cellular
module registers a `Net::CellularProvider` (function-pointer table) in
its `begin()`; the `net.*` commands consult `Net::cellular()` and fall
back to the modem path when WiFi is down. When no cellular module is
compiled in, `Net::cellular()` returns `nullptr` and the commands stay
WiFi-only with no dead code. `MQTTClient` takes its modem socket from
`provider.tcp` the same way.

Same one-way-dependency rule as the `Shell::FSMount` registry: core
declares the hook, the module fills it, core never imports module
headers. Any new transport-aware shell command routes through the
provider rather than `#include`-ing `Cellular.h`.

Source: `lib/thesada-core/src/Net.h`, `Net.cpp`;
`lib/thesada-core/src/Shell.cpp` (`cmd_ifconfig`, `cmd_ping`, `cmd_ntp`,
`cmd_mqtt`, `cmd_http`); `lib/thesada-core/src/MQTTClient.cpp`
(`transportWanted`, `transportClient`);
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

Source: `scripts/check-lib-deps.sh`.

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
it, and the `/ws/serial` handshake is refused so a narrowed mode leaves neither an
interactive session nor its log replay up. OTA is deliberately untouched in
all modes: the periodic `manifest_url` poll is the recovery path when a mode
change goes wrong, and it needs no subscription and no broker. The `ota.cmd_topic`
push works from boot again now that the subscription-table reset is fixed, but
it needs both, so it is a convenience and not the path to rely on.

An absent key parses to `full`, so a config written before the key existed
behaves exactly as it did. A present value that is not a known name parses to
`off` and says so - `shell.mode_unrecognised` for a misspelt string,
`shell.mode_not_a_string` for a bool, number, object or an explicit `null`,
which the config layer would otherwise hand over as an absent key. An empty
string is present too and closes the same way (`shell.mode_unrecognised
value=""`), so a blank UI field cannot open the HTTP command, file and
terminal routes. Only a missing `mode` key is `full`. `shell.ready` logs the
mode applied, not the raw value. A hardening request the firmware
cannot read exactly must not silently serve the full surface. The mode is resolved once on first use, not
per command - it changes on reboot, which is also when a config push lands.

The HTTP file manager is the same capability as `fs.write` and `fs.rm`, so
`POST` and `DELETE /api/file` close with the command surface in every
narrowing mode (403). Two HTTP writers stay open in every mode on purpose:
`POST /api/config` and `POST /ota`. They are how a narrowed device is
changed back when MQTT is down or unverified, the same reason `cmd/config`
is outside the mode. That also means `shell.mode` is no boundary against
someone who holds the web credential: a config push can set `shell.mode`
back to `full` and restart, and an OTA upload can replace the firmware. The
mode narrows what an operator exposes by accident, the web password is what
keeps an attacker out.

How enforced: `shellModeResolve` / `shellModeParse` /
`shellModeSerialAllowed` / `shellModeMqttAllowed` / `shellModeHttpAllowed` /
`shellModeHttpSurfaceAllowed` (`shell_mode_policy.h`,
host-tested in `test/test_shell_mode` under a 95% floor). The MQTT gate skips
the `cli/#` subscription at both registration sites AND guards
`cliInboundHandler`, so a retained message cannot slip through a reinit. A new
transport that reaches `Shell` must add its own gate here; the predicate list
is the checklist.

Source: `lib/thesada-core/src/shell_mode_policy.h`,
`lib/thesada-core/src/Shell.cpp` (`mode`, `pumpConsole`),
`lib/thesada-core/src/MQTTClient.cpp` (`cliInboundHandler`, `begin`,
`reinitSubscriptions`),
`lib/thesada-mod-httpserver/src/HttpServer.cpp` (`cmdHandler`, `_ws`,
`fileHandler`, the `/api/file` DELETE route).

### `cmd/config` is outside `shell.mode` and requires a verified broker

`<prefix>/cmd/config` is not a shell transport. It stays subscribed when
`shell.mode` is `off`, the same way the OTA command topic does, so a headless
device with the web module off can still be given a new `config.json`. The
payload is applied only when the broker session verified the server
certificate. A password on that session is enough. `mqtt.allow_insecure`
encrypts without that check and is refused. The body must be a JSON object
that still contains a non-empty `mqtt.broker` that fits the 96-byte host
buffer. A change to the
connection-critical mqtt keys reconnects without clearing the subscription
table, so callbacks registered after boot stay. The last-good rollback
still covers a broker the device cannot reach. `topic_prefix` is saved and
stays at the boot value until restart. Anything else is written and left
for the next restart.

How enforced: `cmdConfigVerdict` (`cmd_config_policy.h`, host-tested in
`test/test_cmd_config`) refuses an unverified session, a non-object, and a
document with no non-empty `mqtt.broker`. The session bit is set only when
a CA was configured and the live `TlsClient` session verified against it,
on either transport. `Config::replace`
reloads the on-disk file when the write fails and puts a held boot prefix
back. The apply does not reconnect unless that write succeeded, and it
refuses a prefix that cannot form `/cmd/config`, and a prefix that is
not a string. An omitted
`topic_prefix` stays omitted on the next save. The reconnect leaves the
subscription table in place. `Config::save` and shell `config.save` write
the prefix already on disk. Shell `config.set` and `config.del` put the
boot prefix back unless the key is `mqtt` or `mqtt.topic_prefix`.
A prefix that does not fit the CLI topic still subscribes `cmd/config`.

Source: `lib/thesada-core/src/cmd_config_policy.h`,
`lib/thesada-core/src/Config.cpp` (`replace`, `holdTopicPrefix`, `save`,
`set`, `load`, `copyDiskDoc`),
`lib/thesada-core/src/MQTTClient.cpp` (`mqttApplyCmdConfig`,
`mqttSubscribeCmdConfig`, `begin`, `reconnectWithCurrentConfig`,
`reinitSubscriptions`, `onMessage`),
`lib/thesada-core/src/Shell.cpp` (`config.set`, `config.save`, `config.del`).

---

## Sensors

### A temperature probe never publishes a value it did not just read

Every probe on the roster is listed in the `temperature` event and the
combined `<prefix>/sensor/temperature` payload every cycle. A probe whose
read failed, or whose reading the plausibility guard held back, is
`{ name, address, stale: true }` with no `temp_c` or `temp`, and its
per-sensor topic is not published that cycle; its HA discovery carries
`expire_after` of three `temperature.interval_s` (`tempExpireAfterS`), so
HA marks the entity unavailable instead of showing the last value. The
sampler and the expiry both read the interval through `tempIntervalS`, which
takes the signed value and bounds it to 5 s .. 1 day, so `interval_s: 0` or a negative
value can neither spin the loop nor drop the expiry, and a huge one cannot wrap the ms interval. A live
entry carries no `stale` key. Lua rules and every other consumer must handle the stale
shape: a rule that compares or concatenates `temp_c`/`temp` unguarded
throws, and the throw ends that callback's loop over the remaining probes.
Two probes can share a name (an operator's choice, or an auto-named
`temp_N` after a prune), so a rule keys per-probe state by `address`: in
the example a stale twin clearing `freeze:<name>` held the live twin's
freeze streak at one forever. The example publishes its alert to
`<prefix>/alert` as the JSON the app stores, `{"severity","code",
"message"}` with every string escaped. The app drops a plain-text or
severity-less payload.

The plausibility guard (`temperature.max_delta_c`, 0 = off) holds a
reading more than `max_delta_c` from the last accepted one. The next
reading within `max_delta_c` of the held one is accepted
(`temp.read_rebaselined`); a failed read in between keeps the held one.
The first reading after boot is accepted unchecked, except the DS18B20
power-on value: exactly 85.0 C is held back (`temp.read_rejected
reason=power_on`) unless the last accepted reading is within 2 C of it or
the next read repeats it, so a probe reset never publishes 85.00 while a
boiler really at 85 C still reads.

Why: the old fallback republished the last good value with no flag, so a
House Supply lead that broke at 80 C would keep reading 80 C while the
water cooled, and the fire-dying and freeze rules could never fire. A
rejected reading never moved
the baseline, so one real jump past `max_delta_c` (a probe returning
mid fire-up) locked that probe out until reboot.

How enforced: `test/test_temp_guard_policy` (the 5 C to 70 C return,
a held reading across a failed read, lone and disagreeing outliers,
library fault codes and NaN, guard off, the expiry of three intervals
and its saturation, the 85.0 C power-on value first, mid-run and repeated); the coverage floor on
`temp_guard_policy.h`; `tests/lua/test_rules_example.lua` (the example
rules skip a stale probe, alert on it once sustained, and still judge
the probes after it). The same file checks that a same-name stale twin
leaves the live twin's freeze alert to fire, that the alert decodes as
that JSON with quotes, backslashes and control bytes in the name, and runs
the example with the `string` and `table` globals clobbered as on the
device. Not covered: the module's payload assembly runs only on hardware.

Source: `lib/thesada-core/src/temp_guard_policy.h`,
`lib/thesada-mod-temperature/src/TemperatureModule.cpp` (`readSensorC`,
`readAndPublish`), `lib/thesada-core/src/MQTTClient.cpp`
(`publishDiscovery`), `examples/scripts/rules.lua.example`.

### An ADS1115 read never waits on a chip that does not answer

Every channel read probes the chip's address, waits on each conversion for
at most `ADS_CONVERSION_TIMEOUT_US` (10 ms, where one 860 SPS conversion
takes about 1.2 ms) and probes again before the reading counts. A NACK or
a timeout marks the chip offline
(`ads.device.state_change addr= from=online to=offline reason=nack|timeout`),
drops that reading and skips the chip's other channels for the cycle. A
timed-out conversion is re-probed, so a chip that left mid-sample reports
`nack` and only a chip that still ACKs reports `timeout`. An offline chip,
including one missing at boot, is probed once per cycle and is back on its
first full read (`to=online`). No path calls the library's `readADC_*`,
which spin on a ready bit that a NACKed register read never sets. The
deadline is checked before the last poll, so a task preempted past it
still sees a finished conversion.

A channel scales its counts by the PGA range the chip runs at. `gain`
rounds up to the next of 0.256, 0.512, 1.024, 2.048, 4.096 and 6.144 V
(`ads.gain_rounded` warns when it moved), and that range is the scale. The
reading is AC RMS with the DC mean removed, so a biased CT reads 0 A at
zero load. A single-ended mux therefore needs a CT biased to mid-rail. A
bare CT on `A0_GND` is half-wave clipped and reads about 0.55x of true
(0.71x before the mean came out). A bare CT belongs on a differential pair.
A single-ended AC channel whose swing (AC RMS above 16 counts) reaches 8
counts or less logs `ads.clipped name= reason=single_ended_no_bias` once,
and `ads.clip_cleared` once a swing clears ground. An idle input changes
neither. A DC sensor, such as the well-pump pressure transducer, needs
`"dc": true`, which reads the mean of the samples. Without it the mean is
removed and the channel reads about 0 V at any level.

Why: a loose connector next to a vibrating pump hung `loop()` inside
`readADC_*` until the 30 s task watchdog panicked, and every panic counted
toward the OTA crash streak. `"gain": 0.3` ran the chip at 0.512 V but
scaled by 0.3 V, so every reading came out 41% low.

How enforced: `test/test_ads_sample_policy` (standard and non-standard
gains, the 0.3 V case, a DC bias alone, a biased sine, a DC mean at 2.5 V,
a bare CT clipped at ground, a biased CT that swings clear, idle inputs,
full-scale extremes, a chip that never answers, a 50 ms preemption past the
deadline, the microsecond wrap), the coverage floor on
`ads_sample_policy.h`, and `scripts/static-check.sh` (`make lint`), which
fails on any `readADC_` call under `lib/thesada-mod-ads1115/src` and on a
grep that cannot read the tree. Not
covered: no test builds `ADS1115Module.cpp`, so `loadChannels` taking its
scale from `adsPgaFullScaleV`, `sampleRms` routing `"dc"` and single-ended
channels, the probe, the re-probe after a timeout and the offline marking
run only on hardware. No bench run has pulled the connector yet.

Source: `lib/thesada-core/src/ads_sample_policy.h`,
`lib/thesada-mod-ads1115/src/ADS1115Module.cpp` (`readChannelRms`,
`sampleRms`, `markOnline`, `markClipped`, `loadChannels`),
`scripts/static-check.sh`.

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
