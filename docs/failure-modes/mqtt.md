# Failure modes: MQTT client

`failure -> detection -> recovery` for the core MQTT client (`lib/thesada-core/src/MQTTClient.cpp`). Fill a row before redesigning the subsystem; a blank recovery cell is a gap.

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Broker connection loss (TCP drop) | SO_KEEPALIVE 30/10/3; `loop()` sees `!connected()` -> `Log::warn "Connection lost after %lus"` (`:892`) | Exponential backoff reconnect 2s -> 60s (`:899`) |
| Idle / half-open (keepalive missed) | Watchdog: no activity > 600s -> `Log::warn "...forcing reconnect"` (`:854`) | Disconnect + `stop()`, reset backoff (`:860`) |
| Broker unreachable / bad creds (persistent) | `connect()` fail -> `Log::warn "Failed (rc=%d)"` (`:685`), `_retryCount++` | After `mqtt.reboot_after_fails` (30): reboot up to `max_exhaust_reboots` (3), NVS `mqtt_reboots`; budget spent -> `_rebootHalted`, keeps retrying without reboot (`:705`) |
| TLS handshake OOM | `_retryCount>=3 && maxAllocHeap<40000` -> `Log::error "...rebooting to defrag"` (`:693`) | `ESP.restart()` |
| Heap slowly exhausted under TLS | Preventive: free heap < floor for hold-ms -> `Log::warn`/`Log::error "...rebooting"` (`:787`) | `esp_restart()`; disarm if heap recovers |
| Bad `mqtt.*` config bricked connect | Boot-time `rollbackIfUncommitted()` vs last-good NVS -> `Log::warn "mqtt.config_rollback"` (`:523`) | Auto-restore last-good `mqtt.*`, reset reboot budget |
| mTLS cert/key invalid | `validateClientCertKey` fail -> `Log::warn "...password fallback"` (`:614`) | Auto-fallback to user/pass; evict stale cert |
| Publish while disconnected | Ring full -> `Log::warn "Queue full - dropping oldest"` (`:1929`) | Drop oldest, enqueue new; `flushQueue()` on reconnect; route to cellular if fallback active |

## Gaps

- **TLS fails open when CA missing.** No `/ca.crt` + empty PROGMEM bundle -> `_wifiClient.setInsecure()` (`:356`), connects unverified with only a `Log::warn`. MITM exposure; contrast OTA/Telegram which fail closed. **No detection, no refusal.**
- **Auth-reject indistinguishable from broker-down.** Only generic `rc=%d` (`:685`); a credential/CN-ACL rejection loops the same backoff/reboot path as unreachable.
- **Subscribe SUBACK never checked.** `resubscribeAll` ignores `subscribe()` return (`:1578`); a broker-side subscribe drop is silent (no re-topic delivery, no error).
- **Embedded-NUL payload truncation.** WiFi `onMessage` preserves length, but CLI dispatch does `strlen(payload)` (`:405`, `:1554`); a binary `fs.write`/`cert.set` payload is cut at the first `0x00`. Silent.
- **Alert/JSON payload > 256 B silently truncated** into `char payload[256]` (`:371`).

---

Related: [`../security-review-checklist.md`](../security-review-checklist.md), [`../invariants.md`](../invariants.md), [`ota.md`](ota.md), [`cellular.md`](cellular.md).
