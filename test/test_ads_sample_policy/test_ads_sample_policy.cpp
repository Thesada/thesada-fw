// Host-native unit tests for ads_sample_policy.h (ADS1115 scale, RMS, mean,
// ground clipping, wait).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <cmath>
#include <limits>
#include "ads_sample_policy.h"
#include "boot_wait_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const double PI_D = 3.14159265358979323846;

// Integer sine samples over whole cycles, as the ADC would return them.
static AdsRms sine(int amp, int bias, int n, int cycles) {
  AdsRms a;
  for (int i = 0; i < n; i++) {
    double v = bias + amp * std::sin(2.0 * PI_D * cycles * i / n);
    adsRmsAdd(a, (int16_t)std::lround(v));
  }
  return a;
}

// A single-ended input: whatever swings below 0 V reads 0.
static AdsRms clippedSine(int amp, int bias, int n, int cycles) {
  AdsRms a;
  for (int i = 0; i < n; i++) {
    double v = bias + amp * std::sin(2.0 * PI_D * cycles * i / n);
    adsRmsAdd(a, (int16_t)std::lround(std::fmax(0.0, v)));
  }
  return a;
}

void test_standard_gains_select_their_own_range(void) {
  for (int i = 0; i < ADS_PGA_RANGES; i++) {
    TEST_ASSERT_EQUAL_INT(i, adsPgaIndex(ADS_PGA_FS_V[i]));
    TEST_ASSERT_EQUAL_FLOAT(ADS_PGA_FS_V[i], adsPgaFullScaleV(ADS_PGA_FS_V[i]));
  }
}

void test_nonstandard_gain_rounds_up_to_the_next_range(void) {
  TEST_ASSERT_EQUAL_INT(0, adsPgaIndex(0.25f));
  TEST_ASSERT_EQUAL_INT(1, adsPgaIndex(0.3f));
  TEST_ASSERT_EQUAL_INT(1, adsPgaIndex(0.2561f));
  TEST_ASSERT_EQUAL_INT(3, adsPgaIndex(1.5f));
  TEST_ASSERT_EQUAL_INT(5, adsPgaIndex(5.0f));
  TEST_ASSERT_EQUAL_INT(5, adsPgaIndex(100.0f));
}

void test_degenerate_gain_stays_in_the_table(void) {
  TEST_ASSERT_EQUAL_INT(0, adsPgaIndex(0.0f));
  TEST_ASSERT_EQUAL_INT(0, adsPgaIndex(-1.0f));
  TEST_ASSERT_EQUAL_INT(5, adsPgaIndex(std::numeric_limits<float>::quiet_NaN()));
  TEST_ASSERT_EQUAL_INT(5, adsPgaIndex(std::numeric_limits<float>::infinity()));
  TEST_ASSERT_EQUAL_INT(0, adsPgaIndex(-std::numeric_limits<float>::infinity()));
}

// Issue #23: "gain":0.3 runs the chip at +-0.512 V. Half-scale counts are
// 0.256 V, not the 0.15 V a 0.3 / 32768 scale gives (41% low).
void test_counts_scale_by_the_range_the_chip_runs_at(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.512f, adsPgaFullScaleV(0.3f));
  AdsRms a;
  for (int i = 0; i < 10; i++) adsRmsAdd(a, (i & 1) ? 16384 : -16384);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.256f, adsRmsAcVolts(a, adsPgaFullScaleV(0.3f)));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.128f, adsRmsAcVolts(a, adsPgaFullScaleV(0.25f)));
}

// Issue #23: a CT biased to mid-rail at zero load reads 0 A, not its bias.
void test_dc_bias_alone_reads_zero(void) {
  AdsRms a;
  for (int i = 0; i < 30; i++) adsRmsAdd(a, 12000);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adsRmsAcVolts(a, 1.024f));
}

void test_bias_does_not_move_the_ac_rms(void) {
  const float fs = 1.024f;
  float plain  = adsRmsAcVolts(sine(10000, 0, 60, 2), fs);
  float biased = adsRmsAcVolts(sine(10000, 8000, 60, 2), fs);
  float expect = (float)(10000.0 / std::sqrt(2.0) * fs / 32768.0);
  TEST_ASSERT_FLOAT_WITHIN(expect * 0.002f, expect, plain);
  TEST_ASSERT_FLOAT_WITHIN(expect * 0.002f, plain, biased);
}

// The fleet wiring (bare CT, differential, mean ~0) reads as before.
void test_zero_mean_signal_reads_plain_rms(void) {
  AdsRms a;
  for (int i = 0; i < 30; i++) adsRmsAdd(a, (i & 1) ? 300 : -300);
  TEST_ASSERT_FLOAT_WITHIN(1e-7f, 300.0f * 0.256f / 32768.0f, adsRmsAcVolts(a, 0.256f));
}

void test_full_scale_extremes_do_not_overflow(void) {
  AdsRms flat;
  for (int i = 0; i < 1000; i++) adsRmsAdd(flat, -32768);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adsRmsAcVolts(flat, 6.144f));

  AdsRms swing;
  for (int i = 0; i < 1000; i++) adsRmsAdd(swing, (i & 1) ? 32767 : -32768);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 6.144f, adsRmsAcVolts(swing, 6.144f));
  TEST_ASSERT_EQUAL_UINT32(1000, swing.n);
}

void test_no_samples_reads_zero(void) {
  AdsRms a;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adsRmsAcVolts(a, 1.024f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, adsMeanVolts(a, 1.024f));
}

// A 0.5-4.5 V pressure transducer at 2.5 V on gain 4.096 ("dc": true). The
// AC RMS of the same samples is about 0, which is why DC needs the flag.
void test_dc_channel_reads_its_mean(void) {
  AdsRms a;
  for (int i = 0; i < 30; i++) adsRmsAdd(a, (int16_t)(20000 + (i % 7) - 3));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 2.5f, adsMeanVolts(a, 4.096f));
  TEST_ASSERT_TRUE(adsRmsAcVolts(a, 4.096f) < 1e-3f);
}

void test_dc_mean_keeps_its_sign(void) {
  AdsRms a;
  for (int i = 0; i < 10; i++) adsRmsAdd(a, -16384);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, -1.024f, adsMeanVolts(a, 2.048f));
}

void test_min_tracks_the_lowest_count(void) {
  AdsRms a;
  TEST_ASSERT_EQUAL_INT16(32767, a.min);
  adsRmsAdd(a, 500);
  adsRmsAdd(a, -7);
  adsRmsAdd(a, 300);
  TEST_ASSERT_EQUAL_INT16(-7, a.min);
}

// Issue #23: a bare CT on A0_GND reads 0.545x of true and is flagged clipped.
void test_bare_ct_on_single_ended_reads_clipped(void) {
  AdsRms a = clippedSine(10000, 0, 3000, 2);
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Clipped, (int)adsClipAtGround(a));
  float truth = (float)(10000.0 / std::sqrt(2.0) * 1.024 / 32768.0);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.5453f, adsRmsAcVolts(a, 1.024f) / truth);
}

// A mid-rail biased CT swings clear of ground and is not flagged.
void test_biased_ct_swings_clean(void) {
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Clean, (int)adsClipAtGround(sine(10000, 12000, 60, 2)));
}

// An overdriven biased CT whose trough reaches ground is clipped too.
void test_overdriven_biased_ct_reads_clipped(void) {
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Clipped, (int)adsClipAtGround(clippedSine(14000, 12000, 60, 2)));
}

// Idle inputs say nothing: noise at ground, a bias alone, no samples.
void test_idle_input_leaves_clipping_unknown(void) {
  AdsRms ground;
  for (int i = 0; i < 30; i++) adsRmsAdd(ground, (int16_t)((i % 7) - 3));
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Unknown, (int)adsClipAtGround(ground));
  AdsRms bias;
  for (int i = 0; i < 30; i++) adsRmsAdd(bias, 12000);
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Unknown, (int)adsClipAtGround(bias));
  AdsRms none;
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Unknown, (int)adsClipAtGround(none));
}

// The floors sit on their own edges: ground at 8 counts, AC above 16.
void test_clip_floors_hold_their_edges(void) {
  AdsRms edge;
  for (int i = 0; i < 30; i++) adsRmsAdd(edge, (i & 1) ? ADS_GROUND_COUNTS + 40 : ADS_GROUND_COUNTS);
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Clipped, (int)adsClipAtGround(edge));
  AdsRms above;
  for (int i = 0; i < 30; i++) adsRmsAdd(above, (i & 1) ? ADS_GROUND_COUNTS + 41 : ADS_GROUND_COUNTS + 1);
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Clean, (int)adsClipAtGround(above));
  AdsRms quiet;
  for (int i = 0; i < 30; i++) adsRmsAdd(quiet, (i & 1) ? 32 : 0);
  TEST_ASSERT_EQUAL_INT((int)AdsClip::Unknown, (int)adsClipAtGround(quiet));
}

// Fake bus: ready after readyAfter polls, or from readyAtUs on. The clock
// moves stepUs per read.
struct FakeBus {
  uint32_t now        = 0;
  uint32_t stepUs     = 1;
  uint32_t polls      = 0;
  uint32_t readyAfter = 0;  // 0 = never
  bool     byTime     = false;
  uint32_t readyAtUs  = 0;
};

static bool waitOn(FakeBus& b, uint32_t timeoutUs) {
  return adsWaitReady(
    [&b]() {
      b.polls++;
      if (b.polls >= 1000000) return true;  // stops an unbounded wait
      if (b.byTime) return (int32_t)(b.now - b.readyAtUs) >= 0;
      return b.readyAfter != 0 && b.polls >= b.readyAfter;
    },
    [&b]() { b.now += b.stepUs; return b.now; },
    timeoutUs);
}

void test_wait_returns_on_a_ready_conversion(void) {
  FakeBus b;
  b.readyAfter = 3;
  TEST_ASSERT_TRUE(waitOn(b, ADS_CONVERSION_TIMEOUT_US));
  TEST_ASSERT_EQUAL_UINT32(3, b.polls);
}

// Issue #20: a chip off the bus never sets the ready bit. The wait ends at
// the timeout instead of spinning until the task watchdog panics.
void test_wait_gives_up_on_a_chip_that_never_answers(void) {
  FakeBus b;
  b.stepUs = 300;
  TEST_ASSERT_FALSE(waitOn(b, ADS_CONVERSION_TIMEOUT_US));
  TEST_ASSERT_TRUE(b.polls <= ADS_CONVERSION_TIMEOUT_US / b.stepUs + 1);
}

void test_wait_survives_the_microsecond_clock_wrap(void) {
  FakeBus b;
  b.now = 0xFFFFFF00u;
  TEST_ASSERT_FALSE(waitOn(b, 1000));
  TEST_ASSERT_EQUAL_UINT32(1000, b.polls);

  FakeBus r;
  r.now        = 0xFFFFFFF0u;
  r.readyAfter = 500;
  TEST_ASSERT_TRUE(waitOn(r, 1000));
}

// The task is preempted for 50 ms right after the start, and the conversion
// finishes meanwhile. The poll after the deadline still sees it.
void test_wait_polls_once_more_after_a_preemption(void) {
  FakeBus b;
  b.stepUs    = 50000;
  b.byTime    = true;
  b.readyAtUs = b.stepUs + 1200;
  TEST_ASSERT_TRUE(waitOn(b, ADS_CONVERSION_TIMEOUT_US));
  TEST_ASSERT_EQUAL_UINT32(1, b.polls);
}

void test_wait_polls_at_least_once_with_zero_timeout(void) {
  FakeBus b;
  b.readyAfter = 1;
  TEST_ASSERT_TRUE(waitOn(b, 0));
  FakeBus n;
  TEST_ASSERT_FALSE(waitOn(n, 0));
  TEST_ASSERT_EQUAL_UINT32(1, n.polls);
}

// Even 30 timed-out samples stay far inside the task watchdog.
void test_timeout_covers_one_860sps_conversion_with_margin(void) {
  TEST_ASSERT_TRUE(ADS_CONVERSION_TIMEOUT_US >= 4u * 1000000u / 860u);
  TEST_ASSERT_TRUE(30u * ADS_CONVERSION_TIMEOUT_US / 1000u < TASK_WDT_TIMEOUT_MS / 10u);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_standard_gains_select_their_own_range);
  RUN_TEST(test_nonstandard_gain_rounds_up_to_the_next_range);
  RUN_TEST(test_degenerate_gain_stays_in_the_table);
  RUN_TEST(test_counts_scale_by_the_range_the_chip_runs_at);
  RUN_TEST(test_dc_bias_alone_reads_zero);
  RUN_TEST(test_bias_does_not_move_the_ac_rms);
  RUN_TEST(test_zero_mean_signal_reads_plain_rms);
  RUN_TEST(test_full_scale_extremes_do_not_overflow);
  RUN_TEST(test_no_samples_reads_zero);
  RUN_TEST(test_dc_channel_reads_its_mean);
  RUN_TEST(test_dc_mean_keeps_its_sign);
  RUN_TEST(test_min_tracks_the_lowest_count);
  RUN_TEST(test_bare_ct_on_single_ended_reads_clipped);
  RUN_TEST(test_biased_ct_swings_clean);
  RUN_TEST(test_overdriven_biased_ct_reads_clipped);
  RUN_TEST(test_idle_input_leaves_clipping_unknown);
  RUN_TEST(test_clip_floors_hold_their_edges);
  RUN_TEST(test_wait_returns_on_a_ready_conversion);
  RUN_TEST(test_wait_gives_up_on_a_chip_that_never_answers);
  RUN_TEST(test_wait_survives_the_microsecond_clock_wrap);
  RUN_TEST(test_wait_polls_once_more_after_a_preemption);
  RUN_TEST(test_wait_polls_at_least_once_with_zero_timeout);
  RUN_TEST(test_timeout_covers_one_860sps_conversion_with_margin);
  return UNITY_END();
}
