# Security review checklist

Run at PR time when a change adds or widens **attack surface**: a new MQTT command, a new Shell command, a new HTTP endpoint, or a new module that handles remote input. Paste the table into the PR description and answer every row. "N/A" is a valid answer if you can say why.

This is the security-specific pass. The broader readiness gate is [`pre-launch-checklist.md`](pre-launch-checklist.md); the two overlap on "threat model" and "adversarial pass" by design.

---

## Checklist

| # | Check | What good looks like |
|---|-------|----------------------|
| 1 | [ ] Authenticated? How? | Names the mechanism: Basic Auth + bearer token (HTTP), mTLS client cert (MQTT), or local-serial-only. "Anyone on the LAN" is an answer - write it down. |
| 2 | [ ] Scoped to the right caller? | Paired device only / admin only / local serial only. A new `cli/` command any device can publish is load-bearing - say so. |
| 3 | [ ] Input validated? | Length caps, type checks, range checks before use. Rejects malformed input with a log line, does not wedge the event loop. |
| 4 | [ ] Path traversal possible? | Every filesystem path routed through `Shell::pathSafe` before `LittleFS.open` / `fs->open`. No raw caller-supplied path reaches the FS. |
| 5 | [ ] Injection vectors? | No caller string interpolated into a shell dispatch, topic, or format string unescaped. |
| 6 | [ ] Rate limited? Should it be? | Auth surfaces have a lockout (see the HTTP 5-fail / 30 s). New brute-forceable surface gets one or a reason it does not need one. |
| 7 | [ ] Logged? Level + context? | One structured line on state change, errors and retries included. Enough to debug from a serial log or `status/` topic at 3am. |
| 8 | [ ] Audit trail for privileged actions? | Config writes, `secret.set`, OTA triggers, factory reset leave a durable trace. |
| 9 | [ ] CSRF protection on state-changing HTTP? | Dashboard state-changing endpoints are not vulnerable to a forged cross-site request. |
| 10 | [ ] Error messages leak info? | No secrets, no full config, no internal paths in responses returned pre-auth. |
| 11 | [ ] Failure mode designed? | Documented what happens on malformed input, timeout, or unavailable FS/NVS/network - not "it crashes". |

---

## Scope

Applies to: a new MQTT command handler, a new Shell command reachable remotely, a new HTTP endpoint, a new module compiled in by default or enabled via `config.json`.

Does NOT apply to: internal refactors, doc-only changes, test-only commits.

---

Related: [`pre-launch-checklist.md`](pre-launch-checklist.md), [`invariants.md`](invariants.md), [`../SECURITY.md`](../SECURITY.md).
