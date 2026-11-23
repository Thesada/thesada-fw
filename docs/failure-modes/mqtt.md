# Failure modes: MQTT client

`failure -> detection -> recovery` for the core MQTT client (`lib/thesada-core/src/MQTTClient.cpp`). Fill a row before redesigning the subsystem; a blank recovery cell is a gap.

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Broker connection loss (TCP drop) | SO_KEEPALIVE 30/10/3; `loop()` sees `!connected()` -> `Log::warn "Connection lost after %lus"` (`:892`) | Exponential backoff reconnect 2s -> 60s (`:899`) |
| Idle / half-open (keepalive missed) | Watchdog: no activity > 600s -> `Log::warn "...forcing reconnect"` (`:854`) | Disconnect + `stop()`, reset backoff (`:860`) |
| Broker unreachable / bad creds (persistent) | `connect()` fail -> `Log::warn "Failed (rc=%d)"` (`:685`), `_retryCount++` | After `mqtt.reboot_after_fails` (30): reboot up to `max_exhaust_reboots` (3), NVS `mqtt_reboots`; budget spent -> `_rebootHalted`, keeps retrying without reboot (`:705`) |
| TLS handshake OOM | `_retryCount>=3 && maxAllocHeap<40000` -> `Log::error "...rebooting to defrag"` (`:693`) | `ESP.restart()` |
| Heap slowly exhausted under TLS | Preventive: free heap < floor for hold-ms -> `Log::warn`/`Log::error "...rebooting"` (`:787`) | `esp_restart()`; disarm if heap recovers |
| `mqtt.broker` host longer than 95 chars | `mqtt.broker_refused reason=too_long len= cap=96 action=` (warn). `cmd/config` refuses it up front (`mqtt.cmd_config_refused reason=broker_too_long`) | Boot: no broker, connects fail, reboot budget rolls back to last-good. Reinit: keeps the previous broker and port, and that session does not commit the refused config as last-good (`mqtt.config_commit_skipped reason=broker_not_live`) |
| Bad `mqtt.*` config bricked connect | Boot-time `rollbackIfUncommitted()` vs last-good NVS -> `Log::warn "mqtt.config_rollback"` (`:523`) | Auto-restore last-good `mqtt.*`, reset reboot budget |
| mTLS cert/key invalid | `validateClientCertKey` fail in `connect()` -> `mqtt.mtls_cert_invalid fallback=password` | Auto-fallback to user/pass, evict stale cert |
| `cert.set` pushed a key that is not the cert's private half | `cert.apply` runs `checkStoredClientCert` -> `shell.cert_apply_refused reason=pair_invalid`, reply `ok:false`, `_storedCertBroken` set | No reboot. Push the matching half and apply again, or `cert.clear` on the same session. A pair that reaches `connect()` without `cert.apply` falls back as in the row above |
| `cert.apply` cannot allocate the check buffers | `checkStoredClientCert` -> NoHeap -> `shell.cert_apply_refused reason=low_heap`, reply `ok:false` | No reboot, broken verdict unchanged. Retry `cert.apply` once heap frees |
| Publish while disconnected | Ring full -> `Log::warn "Queue full - dropping oldest"` (`:1929`) | Drop oldest, enqueue new; `flushQueue()` on reconnect; route to cellular if fallback active |
| Topic does not fit its buffer (long prefix plus a sensor slug) | `formatTopic` -> `mqtt.topic_refused reason=too_long cap= fmt=` (warn), every publish cycle | That topic is skipped, nothing goes out clipped. Other topics and EventBus still run. Shorten `mqtt.topic_prefix` or the sensor name |
| Subscribe topic too long (Lua, `ota.cmd_topic`, `lua.reload_topic`) or empty (Lua) | `mqtt.sub_rejected reason=topic_length len= cap=160` (warn) | Not subscribed, never a clipped filter. Shorten the topic. An empty `ota.cmd_topic` subscribes nothing, an empty `lua.reload_topic` uses `<prefix>/cmd/lua/reload` |
| CLI output line larger than a page (`config.dump`, line-mode `fs.cat`) | none needed: the line goes out in pieces, each page ending mid-line marked `cont: true` | Consumer joins across `cont`. A reply that still cannot fit logs `mqtt.cli_response_refused reason=oversize` and is not sent, and the caller times out |
| CLI page rejected by PubSubClient | `publishNow` -> `mqtt.publish_failed payload_b= topic=` (warn) | Page lost, caller times out. Pages are sized to `buffer_in`, so this points at a broken session |
| Duplicate HA entity after an update from 26.11.1 (a long sensor or channel name, whose uid 26.11.1 clipped at 48 bytes) | `mqtt.ha_legacy_cleared count=` (info) on each connect that cleared one | `publishDiscovery` sends a zero-length retained publish to the old config topic after the pass and HA removes the old entity. Skipped when this session published that topic, and lost with the session if it drops mid-pass, so the next connect clears it |

## Gaps

- **TLS fails open when CA missing.** No `/ca.crt` + empty PROGMEM bundle -> `_wifiClient.setInsecure()` (`:356`), connects unverified with only a `Log::warn`. MITM exposure; contrast OTA/Telegram which fail closed. **No detection, no refusal.**
- **Auth-reject indistinguishable from broker-down.** Only generic `rc=%d` (`:685`); a credential/CN-ACL rejection loops the same backoff/reboot path as unreachable.
- **Subscribe SUBACK never checked.** `resubscribeAll` ignores `subscribe()` return (`:1578`); a broker-side subscribe drop is silent (no re-topic delivery, no error).
- **Embedded-NUL payload truncation.** WiFi `onMessage` preserves length, but CLI dispatch does `strlen(payload)` (`:405`, `:1554`); a binary `fs.write`/`cert.set` payload is cut at the first `0x00`. Silent.
- **The app does not join across `cont` yet.** thesada-app `awaitPagedCLIResponse` keeps every `output` string as its own line, so a split `config.dump` line is joined with a line break inside it and fails the snapshot JSON check. The reply now parses, but the content needs the app to join the last string of a `cont` page with the first of the next.
- **`fs.write` / `fs.append` to `/config.json` write in place.** The app config editor pushes the file as a chunked `fs.write` + `fs.append` chain (`:1398`). A chunk that fails mid-chain leaves partial JSON on disk, the next boot falls back to defaults (no WiFi, no MQTT). `cmd/config`, the shell config writers and the HTTP writers go through `Config::writeFile` (tmp + rename) instead. **No detection, no refusal.**
- **Alert/JSON payload > 256 B silently truncated** into `char payload[256]` (`:371`).

---

Related: [`../security-review-checklist.md`](../security-review-checklist.md), [`../invariants.md`](../invariants.md), [`ota.md`](ota.md), [`cellular.md`](cellular.md).
