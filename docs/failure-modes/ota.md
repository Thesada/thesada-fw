# Failure modes: OTA update

`failure -> detection -> recovery` for OTA (`lib/thesada-core/src/OTAUpdate.cpp`). A blank recovery cell is a gap.

| Failure | Detection | Recovery |
|---------|-----------|----------|
| No transport at check time | `Log::warn "No transport - skipping"` + `status/ota {"state":"refused","reason":"no-transport"}` (`:487`) | Retries next interval; operator-visible on status topic |
| Manifest download failure | `Log::error "Manifest fetch failed (status=%d)"` (`:636`) -> `reason:"manifest-fetch-failed"` | Retries next interval |
| Manifest malformed / missing fields | `Log::error "Manifest JSON parse failed"` / `"...missing required fields"` (`:648`) | Retries next interval |
| Bad / partial image (short write) | `written < expectedSize` -> `Log::error "Download failed (...)"`, `Update.abort()` (`:764`) | Aborts flash, current fw untouched; `{"state":"failed"}`; retry next interval |
| Corrupt / tampered image | SHA256 stream vs manifest mismatch -> `Log::error "SHA256 mismatch!"`, `Update.abort()` (`:784`) | Aborts, no flash |
| Missing CA for OTA HTTPS | `Log::error "OTA disabled - no /ca.crt..."` -> `_enabled=false`; `reason:"no-ca"` (`:381`) | Fails closed unless `ota.allow_insecure=true` opt-in (logs warn) |
| Heap too low for 2nd TLS session | `maxAllocHeap < 40000` -> `Log::info "Skipping - heap too low"` -> `reason:"heap-low"` (`:538`) | Boot-time check runs on clean heap; `--force` disconnects MQTT to free TLS buffers |
| Cellular download stall | Range-chunk fail -> `Log::error "Range chunk failed at offset %u"` (`:292`) | 64KB Range chunks + periodic PDP bounce; `failed` status if unrecoverable |

## Gaps

- **No firmware signature verification.** SHA256 is integrity-only (`:784`); `CONFIG_SECURE_BOOT is not set`, no `SECURE_SIGNED_APPS`. SECURITY.md:47 confirms. Anyone controlling the (CA-verified) origin/manifest can push arbitrary firmware whose SHA256 matches their own binary. **No detection, no recovery.**
- **App never marks itself valid despite rollback enabled.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` but zero `esp_ota_mark_app_valid_cancel_rollback()` calls in `lib/src`. If the Arduino core does not auto-confirm, the first self-reboot after any OTA (MQTT-exhaust reboot, heap floor, cert.apply) rolls back to the previous image. **Needs verification.** No dedicated boot-loop counter (only a monotonic brownout counter, `main.cpp:52`).
- **CA / server cert expiry during OTA has no dedicated signal.** Surfaces only as a generic negative-status fetch failure (`:636`); the baked PROGMEM CA bundle can age out silently.

---

Related: [`../SECURITY.md`](../SECURITY.md), [`../security-review-checklist.md`](../security-review-checklist.md), [`mqtt.md`](mqtt.md).
