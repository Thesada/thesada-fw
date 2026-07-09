# Failure modes: alert delivery (firmware)

`failure -> detection -> recovery` for firmware alert paths: fw -> Telegram (`lib/thesada-mod-telegram/`) and fw -> app over MQTT. A blank recovery cell is a gap.

## fw -> Telegram

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Bot token rotated / invalid (401/404) | `Log::warn "HTTP %d for %s"` (`TelegramModule.cpp:185`), returns false | Alert still reaches app via EventBus -> MQTT (`:206`), so not lost; but Telegram delivery failure is log-only (gap) |
| Network down at send | `!WiFiManager::connected()` -> `Log::warn "WiFi down - skipping"` (`:135`) | No Telegram queue; alert still enqueues on MQTT/cellular path |
| Low heap (TLS would starve MQTT) | `freeHeap < floor` -> `Log::warn "...skipping (MQTT priority)"` (`:143`) | Intentional skip; alert still goes to MQTT. No Telegram retry |
| No bot_token / not ready | `Log::warn "No bot_token"` / `"not ready"` (`:154`) | Config fix required |
| Bot-API MITM / no CA | No CA -> `Log::error "No Telegram CA - refusing insecure"` (`:86`) | **Fails closed** (client left unconfigured) - this is the fix for the old `setInsecure()` token leak |

## fw -> app (MQTT publish path)

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Publish while MQTT down | `!connected()` in `publish` (`MQTTClient.cpp:909`) | Enqueue on ring for replay, or route to cellular forwarder if fallback active |
| Ring buffer overflow during outage | `Log::warn "Queue full - dropping oldest"` (`:1929`) | Drops oldest alert; newest kept (bounded queue 8) |

## Gaps

- **Telegram delivery failure is log-only.** 401 / token-rotation / webhook failure produce only `Log::warn` (`:185`, `:256`); no `status/` MQTT topic, no retry, no re-pair. The operator cannot see that Telegram stopped delivering (the app path masks it).
- **Webhook path uses `setInsecure()`** (`:92`) - unauthenticated TLS, MITM-exposed. Documented as intentional (may be self-signed/internal) but worth an explicit trust note.
- **Alert payload > 256 B silently truncated** into `char payload[256]` (`MQTTClient.cpp:371`).
- **No end-to-end delivery guarantee to the app.** QoS 0, publish return unchecked (`:874`, `:1957`); fire-and-forget, no app-level ack.

---

Related: [`mqtt.md`](mqtt.md), the app-side alerts failure-mode doc (ingest + notification), and [`../SECURITY.md`](../SECURITY.md).
