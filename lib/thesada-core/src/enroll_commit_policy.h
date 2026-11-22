// thesada-fw - enroll_commit_policy.h
// Stores an enrollment's client cert and mqtt settings as one unit, and
// finishes or undoes a half-done store at boot. Host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Ops is any type with these members, each one NVS or config write or read:
//   bool ackSet(bool)            the ack_pending flag
//   bool ackPending()
//   bool stagePut()              the reply's mqtt fields, then the stage marker
//   bool stagePresent()          the marker
//   bool stageClear()            the marker first, then the fields
//   bool certStore()             cert, then key
//   bool certUsable()            both halves stored and they form a valid pair
//   bool certClear()
//   bool configApply(bool& changed)  staged fields into the mqtt config,
//                                    changed=false when it already held them

enum class EnrollCommit : uint8_t {
  Stored,   // cert and config are the reply's
  Refused,  // cert and config are what they were before
  Left,     // a cert that could not be undone, the stage marker kept for boot
};

enum class EnrollRecover : uint8_t {
  Idle,           // no stage marker
  Kept,           // config already matched, marker cleared
  RolledForward,  // config now holds the staged fields, the caller reboots
  RolledBack,     // cert and marker cleared, config untouched
  Left,           // the cert would not clear, marker kept for the next boot
};

// Undo a store whose config did not land. Marker cleared only after the cert.
// in: ops. out: Refused, or Left when the cert stays.
template <class Ops>
EnrollCommit enrollCommitUndo(Ops& ops) {
  if (!ops.certClear()) return EnrollCommit::Left;
  ops.stageClear();
  ops.ackSet(false);
  return EnrollCommit::Refused;
}

// Order: ack flag, stage marker, cert, config, marker cleared. The config is
// the commit point, so it never names a broker the stored cert is not for.
// in: ops. out: outcome.
template <class Ops>
EnrollCommit enrollCommit(Ops& ops) {
  if (!ops.ackSet(true)) {
    ops.ackSet(false);
    return EnrollCommit::Refused;
  }
  if (!ops.stagePut()) {
    ops.stageClear();
    ops.ackSet(false);
    return EnrollCommit::Refused;
  }
  bool changed = false;
  if (!ops.certStore() || !ops.configApply(changed)) return enrollCommitUndo(ops);
  // A marker left here only makes the next boot confirm the config.
  ops.stageClear();
  return EnrollCommit::Stored;
}

// Boot path, before enrollment picks its step. A marker with a usable cert
// rolls forward, any other marker rolls back. A cleared ack means the app
// already sealed this cert, so the config is left alone.
// in: ops. out: outcome.
template <class Ops>
EnrollRecover enrollRecover(Ops& ops) {
  if (!ops.stagePresent()) return EnrollRecover::Idle;
  if (ops.certUsable()) {
    if (!ops.ackPending()) {
      ops.stageClear();
      return EnrollRecover::Kept;
    }
    bool changed = false;
    if (ops.configApply(changed)) {
      ops.stageClear();
      return changed ? EnrollRecover::RolledForward : EnrollRecover::Kept;
    }
  }
  if (!ops.certClear()) return EnrollRecover::Left;
  ops.stageClear();
  ops.ackSet(false);
  return EnrollRecover::RolledBack;
}
