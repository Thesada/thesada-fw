# Failure modes: cellular (SIM7080)

`failure -> detection -> recovery` for the cellular module (`lib/thesada-mod-cellular/src/`). A blank recovery cell is a gap.

| Failure | Detection | Recovery |
|---------|-----------|----------|
| SIM PIN unlock fails | `Log::error "SIM unlock failed"` -> phase FAILED (`Cellular.cpp:1265`) | Back to STANDBY; re-armed after next 60s WiFi-down hold. No re-PIN retry |
| SIM not ready / blocked | `getSimStatus() != SIM_READY` -> `Log::error "SIM not ready"` (`:1272`) | STANDBY, re-arm next window |
| No signal / not registered (bring-up) | `REG_DENIED` -> `Log::error "Registration DENIED"` (`:582`); timeout (`:666`); re-walk (`:1316`) | Re-walk radio config; overall timeout -> `hardReset()` (`:1218`) |
| No signal (steady-state drop) | `!isRegistered() || !isGprsConnected()` -> `Log::warn "Network lost - re-registering"` (`:1408`) | Blocking re-register, releases AT bus each iter |
| LTE <-> GNSS radio contention | Known time-share; `gpsAcquireFix` holds AT guard across window (`:1489`) | After fix: `CFUN=1` re-wake, poll `+SMSTATE?` until state==1 before next publish (`:1547`) |
| Modem-native MQTT drop (SMPUB fail) | `Log::warn "SMPUB timeout/failed"`, `_mqttConnected=false` (`:1686`) | `loop()` reconnects w/ backoff (`:1432`); flag-drift re-sync |
| Modem wedged (SMSTATE no response) | `Log::warn "SMSTATE no response - modem wedged"` (`:706`) | `modemSoftReset()`, re-upload CA, re-register; hard escalation via DC3 power-cycle |
| Cellular mTLS cert upload fails | `Log::warn "Client cert upload failed - user/pass fallback"` (`:763`) | Auto-fallback to user/pass |
| WiFi -> cellular failover | WiFi down >=60s -> `Log::warn "...activating cellular"` (`CellularModule.cpp:224`) | Auto: fallback publishing on + republish retained set |
| Cellular -> WiFi failback | WiFi up >=60s -> `Log::info "...yielding to WiFi"` (`:309`) | Close publish gate; tear down PDP unless `link_mode:"standby"`; modem stays registered |
| Modem hard-reset mid-ACTIVE | `!isModemAlive()` -> `Log::warn "...re-walking activation"` (`:296`) | Re-enters ACTIVATING state machine |

## Gaps

- **SIM quota / data-plan exhaustion undetected.** `+CMEE=2` on but cause codes not parsed for billing/quota; surfaces only as generic `"Bearer activation failed"` / `"MQTT connect failed"` that retries forever (`:1326`, `:1360`). No operator signal.
- **Blocked/PUK not distinguished from absent SIM** (`:1272`) - same STANDBY path.
- **Inbound-MQTT NAT-timeout stall + lost `+SMSUB` URCs are mitigated, never detected.** `KEEPTIME=15` keeps the path warm (`:744`) and URCs are re-routed (`:867`), but a genuinely lost inbound message has no ack/sequence -> silent.
- **Weak-signal RSSI is telemetry only** (`CellularModule.cpp:365`) - no threshold alert/action.
- **`Cellular::publish` silently drops** when gate/session not ready - `return false` with no log (`:1649`).
- **Cellular MQTT TLS fails open** when CA missing - `Log::warn "MQTT TLS without CA verification"`, proceeds unverified (`:832`). Same as the WiFi MQTT gap.

---

Related: [`mqtt.md`](mqtt.md), [`../security-review-checklist.md`](../security-review-checklist.md).
