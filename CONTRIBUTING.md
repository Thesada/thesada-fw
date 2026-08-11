# Contributing

Patches welcome. This is a one-person project running real hardware in a cold place, so the bar is "does it work on a board", not "does it look tidy".

**Before you start on something big**, open an issue and say what you have in mind. I would rather talk for ten minutes than have you spend a weekend on something I already tried.

**Building.** `python3 scripts/check_deps.py`, then `pio run -e esp32-s3-debug` for a bare devkit or `esp32-owb` for the LILYGO board. The devkit target needs no cellular hardware and is the easiest way in.

**Tests.** `pio test -e native` runs the host-side unit tests, no board needed. `lua5.3 tests/lua/run_all.lua` covers the alert rules. Both run in CI and both need to pass.

**Hooks.** Run `./scripts/hooks/install.sh` once. The pre-commit hook will stop you if you touch a load-bearing source file without updating [docs/invariants.md](docs/invariants.md). The exact list lives in `scripts/check-invariant-ledger.sh` so it cannot drift away from what CI enforces. That gate is deliberate: those files carry rules the rest of the firmware assumes, and the ledger is how they stay written down.

**Commit messages** get linted for things that should not be in a public repo. Internal hostnames, private IPs, references to my internal tracker. If the lint fires on a genuine false positive, `MSG_OK=1 git commit` gets past it. That bypass is an environment variable and leaves nothing behind in the commit, so use it when you mean it.

**Good first issues** are tagged in the issue tracker. If none are open, the "Known limitations and ugly corners" list in the [README](README.md) is a standing menu, and the truncation and Telegram-delivery items are the most self-contained of them.

No CLA, no template to fill in. GPL-3.0-only, same as the rest.
