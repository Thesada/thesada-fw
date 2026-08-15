# Contributing

Patches welcome. This is a one-person project running real hardware in a cold place, so the bar is "does it work on a board", not "does it look tidy".

**Before you start on something big**, open an issue and say what you have in mind. I would rather talk for ten minutes than have you spend a weekend on something I already tried.

**Claim the issue first.** Comment on it and wait for me to assign it to you. I assign whoever asks first, and I write the scope into the issue when I do. A PR against an issue that is already assigned to someone else gets closed however good the code is, and that has already happened once and wasted somebody's evening. Unclaimed bugs are fair game for a drive-by patch. Claimed ones are not.

**Branches.** `dev` is where work lands and it is the default branch, so branch off `dev` and target `dev`. `main` is release-only: it moves when a version ships. If you open against `main` I will just retarget it, no drama.

**Building.** `python3 scripts/check_deps.py`, then `pio run -e esp32-s3-debug` for a bare devkit or `esp32-owb` for the LILYGO board. The devkit target needs no cellular hardware and is the easiest way in.

**Tests.** `pio test -e native` runs the host-side unit tests, no board needed. `lua5.3 tests/lua/run_all.lua` covers the alert rules. Both run in CI and both need to pass.

**Your first PR will not build by itself.** GitHub holds workflow runs from a first-time contributor's fork at "action_required" until I approve them. If the checks look like they never ran, that is why. Say so on the PR and I will approve it.

**Hooks.** Run `./scripts/hooks/install.sh` once. The pre-commit hook will stop you if you touch a load-bearing source file without updating [docs/invariants.md](docs/invariants.md). The exact list lives in `scripts/check-invariant-ledger.sh` so it cannot drift away from what CI enforces. That gate is deliberate: those files carry rules the rest of the firmware assumes, and the ledger is how they stay written down. CI runs the same check on every PR, so skipping the hooks only moves the failure later. If your change genuinely establishes no new invariant, put `INVARIANT_OK: 1` as a trailer in the commit message. Same deal as the message lint below: explicit on purpose, and it leaves a trail.

**Commit messages** get linted for things that should not be in a public repo. Internal hostnames, private IPs, references to my internal tracker. If the lint fires on a genuine false positive, `MSG_OK=1 git commit` gets past it. That bypass is an environment variable and leaves nothing behind in the commit, so use it when you mean it.

**Log lines** are structured `module.event key=value`. Before inventing an event name, grep the file you are editing for a sibling. If the function next to yours logs `cellular.publish_skipped reason=at_bus_busy`, do not add `cellular.publish_drop` twenty lines away. These lines get grepped at 3am and consistency is the whole point of them.

**Good first issues** are tagged in the issue tracker. If none are open, the "Known limitations and ugly corners" list in the [README](README.md) is a standing menu, and the truncation and Telegram-delivery items are the most self-contained of them.

No CLA, no template to fill in. GPL-3.0-only, same as the rest.
