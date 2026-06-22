# Pre-launch checklist

Run before opening a new endpoint / module / MQTT cmd / board to traffic.

---

## Checklist

| # | Check | Why |
|---|-------|-----|
| 1 | [ ] Threat model updated? | New attack surface changes the threat model. Write down what you are exposing, who can reach it, and what the worst-case abuse looks like before anyone else can. |
| 2 | [ ] Failure modes documented? | Firmware failures are silent without it. List what happens when the feature partially fails, times out, or is fed malformed input. |
| 3 | [ ] Observability hooks in place (logs + metrics)? | If you cannot see it from a serial log or an MQTT `status/` topic at 3am, you cannot debug it at 3am. Every new subsystem emits at least one structured status line on state change. |
| 4 | [ ] Tests for the contract, not just the bug? | Regression tests catch the bug that was fixed. Contract tests catch the next bug. Cover the invariant, not the incident. |
| 5 | [ ] Adversarial pass done? | Spend 15 minutes pretending to be an attacker. Send malformed input. Fuzz lengths. Skip auth steps in sequence. File findings before merging. |

---

## Scope

This checklist applies to:

- A new MQTT command handler (`cli/<cmd>`, `cmd_topic`)
- A new HTTP endpoint
- A new Shell command exposed to remote callers
- A new board target going to production traffic
- A new module compiled in by default or enabled via `config.json`

It does NOT apply to internal refactors, doc-only changes, or test-only commits.

---

## Minimum bar per item

### 1. Threat model updated

Write a short block in `docs/invariants.md` (or a linked file) that answers:

- What is exposed? (topic, endpoint, command name)
- Who can reach it? (any device, paired device only, admin only, local serial only)
- What is the blast radius of a successful exploit?

If the answer is "any device on MQTT can send this command", that is load-bearing. Write it down.

### 2. Failure modes

For every new handler, answer:

- What happens on malformed input? (reject + log, or undefined?)
- What happens on partial write or timeout? (retry? corrupt state?)
- What happens if the downstream resource (FS, network, NVS) is unavailable?

If the answer is "it panics / crashes / wedges the event loop" - that is a bug, not a failure mode.

### 3. Observability

Every new subsystem must emit at minimum:

- One log line on successful init (`Log::info`)
- One log line on state change (esp. errors and retries)
- One MQTT `status/` publish where the feature is remotely observable

See `docs/invariants.md` - "State machines must emit structured transitions".

### 4. Contract tests

Unit tests that only verify the happy path are insufficient. Write at least one test per of:

- Boundary values (empty input, max-length input, `\0`)
- Rejected inputs (path traversal, out-of-range, wrong type)
- The post-condition that must hold after a successful call

### 5. Adversarial pass

Walk through the new surface manually. Minimum actions:

- Send an empty payload
- Send a payload 10x larger than expected
- Send a payload with path traversal (`../`, `//`, `\0`)
- Send commands out of order (skip setup step, double-invoke teardown)
- Pull power mid-operation and verify state on reboot

Document what you tried and what happened. "Did adversarial pass - no issues found" is an acceptable entry if you actually did it.

---

Related: [`invariants.md`](invariants.md), [`../CODE-GUIDELINES.md`](../CODE-GUIDELINES.md).
