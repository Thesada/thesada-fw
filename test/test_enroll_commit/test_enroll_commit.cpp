// Host-native unit tests for enroll_commit_policy.h (cert and broker config as one unit).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "enroll_commit_policy.h"
#include "enroll_policy.h"

void setUp(void) {}
void tearDown(void) {}

// Fake unit: NVS flags, cert halves and the mqtt config, each write one step.
// Generation 0 is the old state, any other number one enrollment reply.
struct Dev {
  bool ack = false;
  bool stg = false;
  int  stgGen = 0;
  int  cert = 0;
  int  key = 0;
  int  cfg = 0;
  int  reply = 1;    // reply this commit stores
  int  target = 0;   // what configApply writes
  int  budget = -1;  // writes left before the power cut, -1 none
  int  cfgWrites = 0;
  bool failAck = false, failCert = false, failKey = false, failCfg = false;
  bool failClear = false, failStageClear = false;

  bool write() {
    if (budget == 0) return false;
    if (budget > 0) budget--;
    return true;
  }
  bool ackSet(bool v) {
    if (failAck || !write()) return false;
    ack = v;
    return true;
  }
  bool ackPending() { return ack; }
  bool stagePut() {
    if (!write()) return false;
    stgGen = reply;
    if (!write()) return false;
    stg = true;
    return true;
  }
  bool stagePresent() { return stg; }
  bool stageClear() {
    if (failStageClear || !write()) return false;
    stg = false;
    return true;
  }
  bool certStore() {
    if (failCert || !write()) return false;
    cert = reply;
    if (failKey || !write()) return false;
    key = reply;
    return true;
  }
  bool certUsable() { return cert != 0 && cert == key; }
  bool certClear() {
    if (failClear || !write()) return false;
    cert = 0;
    if (!write()) return false;
    key = 0;
    return true;
  }
  bool configApply(bool& changed) {
    changed = cfg != target;
    if (!changed) return true;
    if (failCfg || !write()) return false;
    cfg = target;
    cfgWrites++;
    return true;
  }
  // Store this reply at runtime: config target is the reply.
  EnrollCommit commit() {
    target = reply;
    return enrollCommit(*this);
  }
  // Power back on, faults gone: recovery reads the staged fields.
  EnrollRecover boot() {
    budget = -1;
    failAck = failCert = failKey = failCfg = failClear = failStageClear = false;
    target = stgGen;
    EnrollRecover r = enrollRecover(*this);
    if (!(cert && key) && ack) ack = false;  // Enroll::begin drops an ack with no cert
    return r;
  }
  bool isOld() const { return cfg == 0 && !(cert != 0 && cert == key) && !stg && !ack; }
  bool isNew(int g) const { return cfg == g && cert == g && key == g && !stg && ack; }
  // The two states the issue is about: a broker without its cert, or the reverse.
  bool isMixed() const {
    bool usable = cert != 0 && cert == key;
    return (cfg != 0 && !usable) || (usable && cfg != cert);
  }
};

// --- runtime commit ---------------------------------------------------------

void test_happy_path_stores_all(void) {
  Dev d;
  TEST_ASSERT_EQUAL(EnrollCommit::Stored, d.commit());
  TEST_ASSERT_TRUE(d.isNew(1));
  TEST_ASSERT_EQUAL(EnrollStep::AwaitMtls, enrollStartStep(d.certUsable(), d.ack));
  TEST_ASSERT_EQUAL(EnrollRecover::Idle, d.boot());
  TEST_ASSERT_TRUE(d.isNew(1));
}

void test_ack_flag_failure_changes_nothing(void) {
  Dev d;
  d.failAck = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Refused, d.commit());
  TEST_ASSERT_TRUE(d.isOld());
  TEST_ASSERT_EQUAL(0, d.cfgWrites);
}

void test_cert_store_failure_leaves_old_config(void) {
  Dev d;
  d.failCert = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Refused, d.commit());
  TEST_ASSERT_TRUE(d.isOld());
  TEST_ASSERT_EQUAL(0, d.cfgWrites);
  Dev c;  // control: same reply, no fault
  TEST_ASSERT_EQUAL(EnrollCommit::Stored, c.commit());
  TEST_ASSERT_TRUE(c.isNew(1));
}

void test_key_store_failure_clears_the_cert_half(void) {
  Dev d;
  d.failKey = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Refused, d.commit());
  TEST_ASSERT_TRUE(d.isOld());
  TEST_ASSERT_EQUAL(0, d.cert);
}

void test_config_failure_clears_the_cert(void) {
  Dev d;
  d.failCfg = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Refused, d.commit());
  TEST_ASSERT_TRUE(d.isOld());
  TEST_ASSERT_EQUAL(EnrollStep::Announce, enrollStartStep(d.certUsable(), d.ack));
  Dev c;
  TEST_ASSERT_EQUAL(EnrollCommit::Stored, c.commit());
  TEST_ASSERT_TRUE(c.isNew(1));
}

// Cert cannot be undone: the stage stays so the next boot finishes the job.
void test_config_and_clear_failure_is_left_for_boot(void) {
  Dev d;
  d.failCfg   = true;
  d.failClear = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Left, d.commit());
  TEST_ASSERT_TRUE(d.stg);
  TEST_ASSERT_TRUE(d.ack);
  TEST_ASSERT_EQUAL(0, d.cfg);
  TEST_ASSERT_EQUAL(EnrollRecover::RolledForward, d.boot());
  TEST_ASSERT_TRUE(d.isNew(1));
}

void test_left_and_config_still_failing_rolls_back(void) {
  Dev d;
  d.failCfg   = true;
  d.failClear = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Left, d.commit());
  d.budget = -1;
  d.failClear = false;
  d.target = d.stgGen;
  d.failCfg = true;
  TEST_ASSERT_EQUAL(EnrollRecover::RolledBack, enrollRecover(d));
  TEST_ASSERT_TRUE(d.isOld());
}

void test_left_at_boot_when_nothing_can_be_undone(void) {
  Dev d;
  d.failCfg   = true;
  d.failClear = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Left, d.commit());
  d.target = d.stgGen;
  TEST_ASSERT_EQUAL(EnrollRecover::Left, enrollRecover(d));
  TEST_ASSERT_TRUE(d.stg);
  TEST_ASSERT_EQUAL(0, d.cfg);
}

void test_stage_clear_failure_still_stores(void) {
  Dev d;
  d.failStageClear = true;
  TEST_ASSERT_EQUAL(EnrollCommit::Stored, d.commit());
  TEST_ASSERT_TRUE(d.stg);
  TEST_ASSERT_EQUAL(1, d.cfgWrites);
  // Config already matches the stage: no second write, no reboot.
  TEST_ASSERT_EQUAL(EnrollRecover::Kept, d.boot());
  TEST_ASSERT_TRUE(d.isNew(1));
  TEST_ASSERT_EQUAL(1, d.cfgWrites);
}

void test_stage_put_failure_changes_nothing(void) {
  Dev d;
  d.budget = 2;  // ack, stage fields, then the marker is cut
  TEST_ASSERT_EQUAL(EnrollCommit::Refused, d.commit());
  TEST_ASSERT_FALSE(d.stg);
  TEST_ASSERT_EQUAL(0, d.cert);
  TEST_ASSERT_EQUAL(0, d.cfg);
}

// --- boot recovery ----------------------------------------------------------

void test_boot_without_stage_is_idle(void) {
  Dev d;
  d.cfg = 5;
  TEST_ASSERT_EQUAL(EnrollRecover::Idle, d.boot());
  TEST_ASSERT_EQUAL(5, d.cfg);
}

// An acked enrollment whose stage never cleared keeps its cert and config.
void test_sealed_stage_is_only_cleared(void) {
  Dev d;
  d.stg = true;
  d.stgGen = 1;
  d.cert = d.key = 1;
  d.cfg = 1;
  d.ack = false;
  TEST_ASSERT_EQUAL(EnrollRecover::Kept, d.boot());
  TEST_ASSERT_FALSE(d.stg);
  TEST_ASSERT_EQUAL(1, d.cert);
  TEST_ASSERT_EQUAL(0, d.cfgWrites);
}

// A stray key half from an older cert.set must not pass as the new pair.
void test_mismatched_half_pair_rolls_back(void) {
  Dev d;
  d.key = 7;
  d.budget = 4;  // ack, stage fields, marker, cert half; the key is cut
  d.commit();
  TEST_ASSERT_EQUAL(1, d.cert);
  TEST_ASSERT_EQUAL(7, d.key);
  TEST_ASSERT_TRUE(d.stg);
  TEST_ASSERT_EQUAL(EnrollRecover::RolledBack, d.boot());
  TEST_ASSERT_TRUE(d.isOld());
}

// --- power cut at every write -----------------------------------------------

static int happyWrites() {
  Dev d;
  d.budget = 1000;
  d.commit();
  return 1000 - d.budget;
}

void test_power_cut_at_every_write_recovers_old_or_new(void) {
  int n = happyWrites();
  TEST_ASSERT_GREATER_THAN(3, n);
  int sawOld = 0, sawNew = 0;
  for (int b = 0; b <= n; b++) {
    Dev d;
    d.budget = b;
    d.commit();
    d.boot();
    TEST_ASSERT_FALSE_MESSAGE(d.isMixed(), "mixed state after recovery");
    TEST_ASSERT_TRUE_MESSAGE(d.isOld() || d.isNew(1), "neither old nor new");
    if (d.isOld()) {
      sawOld++;
      d.reply = 2;  // the app re-signs on the next poll
      TEST_ASSERT_EQUAL(EnrollCommit::Stored, d.commit());
      TEST_ASSERT_TRUE(d.isNew(2));
    } else {
      sawNew++;
    }
  }
  // Both outcomes reached: the cut points span the commit point.
  TEST_ASSERT_GREATER_THAN(0, sawOld);
  TEST_ASSERT_GREATER_THAN(0, sawNew);
}

// A second cut during recovery itself still ends old or new on the boot after.
void test_power_cut_during_recovery_recovers_on_next_boot(void) {
  int n = happyWrites();
  for (int b = 0; b <= n; b++) {
    for (int r = 0; r <= 4; r++) {
      Dev d;
      d.budget = b;
      d.commit();
      d.budget = -1;
      d.target = d.stgGen;
      d.budget = r;
      enrollRecover(d);
      d.boot();
      TEST_ASSERT_FALSE(d.isMixed());
      TEST_ASSERT_TRUE(d.isOld() || d.isNew(1));
    }
  }
}

// Every fault combination at runtime, then one clean boot: never mixed.
void test_fault_matrix_never_mixed_after_boot(void) {
  for (int m = 0; m < 64; m++) {
    Dev d;
    d.failAck        = m & 1;
    d.failCert       = m & 2;
    d.failKey        = m & 4;
    d.failCfg        = m & 8;
    d.failClear      = m & 16;
    d.failStageClear = m & 32;
    d.commit();
    d.boot();
    TEST_ASSERT_FALSE(d.isMixed());
    TEST_ASSERT_TRUE(d.isOld() || d.isNew(1));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_happy_path_stores_all);
  RUN_TEST(test_ack_flag_failure_changes_nothing);
  RUN_TEST(test_cert_store_failure_leaves_old_config);
  RUN_TEST(test_key_store_failure_clears_the_cert_half);
  RUN_TEST(test_config_failure_clears_the_cert);
  RUN_TEST(test_config_and_clear_failure_is_left_for_boot);
  RUN_TEST(test_left_and_config_still_failing_rolls_back);
  RUN_TEST(test_left_at_boot_when_nothing_can_be_undone);
  RUN_TEST(test_stage_clear_failure_still_stores);
  RUN_TEST(test_stage_put_failure_changes_nothing);
  RUN_TEST(test_boot_without_stage_is_idle);
  RUN_TEST(test_sealed_stage_is_only_cleared);
  RUN_TEST(test_mismatched_half_pair_rolls_back);
  RUN_TEST(test_power_cut_at_every_write_recovers_old_or_new);
  RUN_TEST(test_power_cut_during_recovery_recovers_on_next_boot);
  RUN_TEST(test_fault_matrix_never_mixed_after_boot);
  return UNITY_END();
}
