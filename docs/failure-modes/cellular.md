# Failure modes: cellular (SIM7080)

`failure -> detection -> recovery` for the cellular module (`lib/thesada-mod-cellular/src/`). A blank recovery cell is a gap. The modem carries plain TCP; TLS and MQTT run on the ESP32 (`TlsClient`, `MQTTClient`), so session failures are the same as on WiFi (see [`mqtt.md`](mqtt.md)).

| Failure | Detection | Recovery |
|---------|-----------|----------|
| SIM PIN unlock fails | `cellular.sim_unlock_failed` -> phase FAILED (`tickActivation` SIM) | Back to STANDBY; re-armed after next 60s WiFi-down hold. No re-PIN retry |
| SIM not ready / blocked | `cellular.sim_not_ready` (`tickActivation` SIM) | STANDBY, re-arm next window |
| No signal / not registered (bring-up) | `cellular.registration_denied`, `cellular.registration_failed` (`pollRegistration`, `tickActivation` REGISTER) | Re-walk radio config; overall timeout -> `hardReset()` |
| No signal (steady-state drop) | `cellular.link.state_change from=up to=down reason=network_lost` (`Cellular::loop`) | Sockets marked closed; blocking re-register, releases AT bus each iteration; MQTT reconnects after `from=down to=up` |
| LTE <-> GNSS radio contention | Known time-share; `gpsAcquireFix` holds AT guard across window | After fix: `CFUN=1` re-wake, poll `+CNACT?` until the bearer is active |
| MQTT session drop on the modem socket | `mqtt.state_change from=connected to=disconnected` | `MQTTClient::loop` reconnects with backoff, same as WiFi |
| Server cert or host name wrong | `tls.handshake_failed ... verify=0x...` (`TlsClient`) | Refused, retried with backoff; never falls back to unverified |
| Modem wedged | activation deadline, or `+CAOPEN` / `+CASEND` failing | `hardReset()` (DC3 power-cycle), sockets marked closed, activation re-walked |
| WiFi -> cellular failover | WiFi down >=60s -> `cellular.state_change from=standby to=activating` | Bearer up -> `MQTTClient::setCellularTransport(true)`, session dials the modem socket, retained set republished on connect |
| Cellular -> WiFi failback | WiFi up -> `mqtt.transport_switch from=cellular to=wifi`; after 60s `cellular.state_change from=active to=standby` | Session moves to WiFi at once; PDP torn down unless `link_mode:"standby"`; modem stays registered |
| Cellular NTP fails (`+CNTP: 62` right after activation, a LAN `ntp.server`, server down) | `cellular.ntp_failed step=result code=N`, then `cellular.ntp_sync_failed clock=unsynced attempt=N next_s=S` (`CellularModule::syncClock`) | Retried from ACTIVE at 30 s, then 60 s doubling to 30 min, over `server`, `server2`, `pool.ntp.org` with unreachable hosts skipped, one host per loop pass with a 15 s wait cap. The clock floor keeps `time()` plausible meanwhile |
| Modem hard-reset mid-ACTIVE | `!isModemAlive()` -> `cellular.state_change from=active to=activating` | Re-enters ACTIVATING state machine |
| `net.http` over cellular: slow body, large body, stalled body or silent server | `read stopped at the 25 s budget`, `cellular.https_get_cut reason=budget`, `shell.http_abandoned via=cellular wait_ms=30000` | The fetch runs on the `net.http` task. Its read loop stops 25 s after the connect began, checked on every read pass, so a stalled header or body stops too. A connect slower than the 30 s wait ends on the task. The shell ticks MQTT once a second (`shell.http_wait ... mqtt_ticks=`) and gives the loop back after 30 s whatever the modem does, and the task frees the job when the GET ends |
| Modem HTTPS socket (slot 1) already in use | `cellular.https_get_skipped reason=busy` | The second caller fails at once: `net.http` prints `status=0 ok=0` and an OTA check tries again at its next interval. A chunked OTA download already under way waits up to 90 s for the slot (`Cellular::httpsBusy`) before each try, so a console `net.http` in the chunk gap does not spend one |
| MQTT ping during a `net.http` meets a long AT-bus hold on the fetch task (slow `+CDNSGIP` or `+CAOPEN`) | `mqtt.state_change from=connected to=disconnected reason=connection_lost` | The slot-0 write fails after its 10 s bus guard, the `TlsClient` write timeout closes the session, and `MQTTClient::loop` reconnects with backoff. A ping that meets a shorter hold goes out late, inside the 45 s broker grace |
| Socket opens or sends from the `net.http` task reach the recycle or reset streak | `cellular.socket_recover_queued action=bearer_recycle` or `action=hard_reset` | The fetch fails at once. The loop runs the step at its next health probe, and every AT-bus wait meanwhile feeds the watchdog in 1 s slices, so neither task trips the 30 s TWDT |

## Gaps

- **SIM quota / data-plan exhaustion undetected.** `+CMEE=2` on but cause codes not parsed for billing/quota; surfaces only as a generic `cellular.bearer_failed` or `tls.dial_failed` that retries forever. No operator signal.
- **Blocked/PUK not distinguished from absent SIM** - same STANDBY path.
- **Weak-signal RSSI is telemetry only** (`CellularModule::emitRssi`) - no threshold alert/action.
- **OTA's `httpsGet` header phase has no idle bound.** OTA passes no read budget, so a server that completes TLS and never sends a status line holds slot 1 and the loop until the 5 min cap. `net.http` stops at its 25 s budget.

---

Related: [`mqtt.md`](mqtt.md), [`../security-review-checklist.md`](../security-review-checklist.md).
