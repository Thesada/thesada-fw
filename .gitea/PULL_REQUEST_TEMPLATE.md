## Summary

<!-- 1-3 bullets on what changed and why -->

## Test plan

- [ ] Builds clean for all supported board envs (`pio run`)
- [ ] Bench-flashed on at least one target
- [ ] Soak run >= 10 min where applicable

## Load-bearing surface

Did this PR touch any of: OTA, MQTT client, Shell, Config, HTTP server,
Cellular, or other load-bearing source paths named in
[docs/invariants.md](docs/invariants.md)?

- [ ] **No.** No ledger change needed.
- [ ] **Yes**, and `docs/invariants.md` is updated in this PR
      (`Dated` line bumped). New / modified invariants:
  - <!-- one line per invariant -->
- [ ] **Yes**, but no invariant change. Reason:
  - <!-- one-line N/A justification - e.g., "pure rename, no semantic change" -->

## Privacy and secrets

- [ ] No credentials, tokens, or keys in diff or commit message
- [ ] No personal data or deployment-specific identifiers
