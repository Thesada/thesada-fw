# Operational runbook (firmware)

Recovery procedures for a device in the field. Numbered steps, real shell commands where confirmed; a step whose exact command is unverified says so rather than guess. Failure signals are in [`failure-modes/`](failure-modes/).

Access to a device: MQTT CLI (verified TLS, primary), the serial console (physical), or the HTTP dashboard if `web.enabled`. Commands below are firmware shell commands unless noted.

---

## Device certificate revoked

What the user sees: the device drops mTLS and the app has pushed it into **password-recovery mode** (the revoke flow deletes the dynsec client + pushes the device to recover). It reconnects with password auth if provisioned, otherwise it cannot reach the broker.

1. Confirm on the device: fw logs `mTLS: ... password fallback` (`MQTTClient.cpp:614`) or repeated connect failures.
2. Re-pair from the app: `/admin/devices/{id}/pair/issue` (signs a fresh cert, pushes cert+key+secrets, re-creates the dynsec client, marks paired).
3. On the device, confirm the new identity: `ota.status` / device `info`, and a clean MQTT connect in the logs.
4. If the device also needs a restart to pick up the new port: the pair flow sends `cli/restart`; if that publish was lost (`pair issue: cli/restart publish failed`), restart manually.

## OTA bricked the device (factory recovery)

1. First check it is actually bricked vs mid-recovery: on serial, read the boot line `Boot reset=<reason>` (`main.cpp:35`) and `ota.status` (`pending_verify` / `valid` / `invalid`, `Shell.cpp:1550`).
2. Bootloader rollback is compiled in (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`) but the app does not explicitly mark itself valid - health-gated rollback is a known enhancement, not yet implemented. If the device rolled back on its own to the prior image, it may already be recovering.
3. If it is boot-looping on a bad image: there is **no OTA-side recovery** - recover over **serial/USB reflash** with a known-good build. *(Exact flash command/board target: use the project's standard PlatformIO upload for that board; verify the port first.)*
4. After reflash, re-pair (cert was not affected unless you erased NVS).
5. Note: no dedicated boot-loop counter exists (only a monotonic brownout counter, `main.cpp:52`).

## /config.json corrupt

1. Symptom: device fails to apply config, or connects with wrong/empty settings.
2. Critical `mqtt.*` keys self-heal: boot-time `rollbackIfUncommitted()` compares live config against the last-good NVS snapshot and restores it (`Log::warn "mqtt.config_rollback"`, `MQTTClient.cpp:523`). Let it reboot once and check for that log.
3. Inspect on-device: `fs.cat /config.json` (or the mounted path); `fs.ls /` to see what is present.
4. If only non-critical keys are bad: fix with `config.set <key> <value>` and commit. *(Confirm the exact config command names on the running build.)*
5. If the file is unrecoverable: reformat LittleFS (`fs.format --yes` - LittleFS) and re-provision from the app pairing flow. Secrets in NVS (via `secret.set`) survive an FS reformat; a full NVS erase does not.

## mTLS provisioning failed mid-flow

1. Symptom (from the app): pair page shows `?error=...`; the device may hold a partial NVS write while the app shows it **unpaired** (the non-transactional pair gap; see the app-side pairing failure-mode doc).
2. Recovery is a clean **re-click of Issue** - the flow is idempotent (`secret.set` overwrites, dynsec "already exists" tolerated). It pushes cert/config/secret again then persists.
3. If Issue keeps failing at cert-sign: check the CA is initialized/unlocked on the app host (`CA not initialized` error); fix the CA passphrase, retry.
4. Verify: device `info` shows the cert; broker accepts the mTLS connect.

## Cellular: SIM blocked / quota exhausted

1. Symptom: registration may pass but PDP/MQTT connect fails; fw logs generic `Bearer activation failed` / `MQTT connect failed` - **there is no quota/billing detection** (see [`failure-modes/cellular.md`](failure-modes/cellular.md)).
2. Rule out signal vs SIM: `Registration DENIED` / `timeout` points to coverage/registration; a `SIM not ready` points to the SIM itself (note: blocked vs absent are not distinguished).
3. Check the SIM with the carrier (quota, block, activation). OneSimCard SIMs need handset-first activation before they work in the modem.
4. On the device, a wedged modem self-recovers: `SMSTATE no response - modem wedged` triggers a soft reset, and an activation timeout triggers a DC3 hard power-cycle (`Cellular.cpp:706`, `:1218`). Force one by rebooting the device if it is stuck.
5. Failover: if WiFi is available the device uses it after 60s; cellular is the fallback path.

## Heap-reboot loop

1. Symptom: device reboots repeatedly; boot line shows heap-floor or watchdog reboots.
2. The heap watchdog reboots when free heap sits below the floor (`MQTTClient.cpp:787`). This **masks a leak** rather than fixing it - the known follow-up is to persist the counter and surface it via `/info`.
3. Read the reboot/heap counters via device `info` / `/info` to see the trend. A steady decline between reboots = a leak, not a transient.
4. Mitigate in the field: disable non-essential modules (`config.set` the module `enabled` flags off) to cut heap pressure, reboot, observe.
5. If it persists, capture logs and file against the leaking subsystem; a firmware fix is required - the watchdog only buys time.

## Telegram bot token rotated

1. Symptom: Telegram alerts stop; fw logs `HTTP 401/404 for ...` (`TelegramModule.cpp:185`). Alerts still reach the app over MQTT (only the Telegram channel is down) - and this failure is **log-only**, not surfaced (see failure-modes/alerts.md).
2. Update the token on the device: `secret.set` (or `config.set`) the `bot_token`, then re-provision / restart. *(Confirm the exact key name on the running build.)*
3. Verify: send a test alert and confirm delivery; the `HTTP 401` log clears.

---

Related: [`failure-modes/`](failure-modes/), [`../SECURITY.md`](../SECURITY.md), and the app-side runbook.
