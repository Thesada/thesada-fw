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
| Modem hard-reset mid-ACTIVE | `!isModemAlive()` -> `cellular.state_change from=active to=activating` | Re-enters ACTIVATING state machine |

## Gaps

- **SIM quota / data-plan exhaustion undetected.** `+CMEE=2` on but cause codes not parsed for billing/quota; surfaces only as a generic `cellular.bearer_failed` or `tls.dial_failed` that retries forever. No operator signal.
- **Blocked/PUK not distinguished from absent SIM** - same STANDBY path.
- **Steady-state network recovery blocks the main loop.** `Cellular::loop` retries `networkConnect` in a loop that pauses the AT bus and pumps the shell, but other modules do not tick until the network is back.
- **Weak-signal RSSI is telemetry only** (`CellularModule::emitRssi`) - no threshold alert/action.

---

Related: [`mqtt.md`](mqtt.md), [`../security-review-checklist.md`](../security-review-checklist.md).
