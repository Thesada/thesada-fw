## Summary

<!-- What changed and why, in 1-3 bullets. -->

## Test plan

<!-- Boards exercised, manual flow checked, log line that confirms success. -->

- [ ] Built clean on the relevant board envs (`pio run -e ...`)
- [ ] Manual flow exercised on hardware where applicable

## Invariant ledger

- [ ] Touches load-bearing source (OTA, MQTT, Shell, Config, HttpServer, Cellular)
- [ ] `docs/invariants.md` updated (and `Dated` line bumped) in this PR
- [ ] OR: bypass justified - explain why no invariant is established or relied on

The `invariant-ledger` CI job enforces this for the listed paths.
Bypass with `INVARIANT_OK: 1` as a commit trailer if the touch genuinely
does not affect any invariant.
