// thesada-fw - ads_sample_policy.h
// ADS1115 sample math: the PGA range a configured gain actually selects, AC
// RMS with the DC mean taken out, the DC mean, clipping at ground and a
// bounded wait for one conversion. Pure, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <stdint.h>
#include <limits>

// PGA full-scale ranges in volts, smallest first (datasheet table 3).
constexpr int   ADS_PGA_RANGES = 6;
constexpr float ADS_PGA_FS_V[ADS_PGA_RANGES] = {0.256f, 0.512f, 1.024f, 2.048f, 4.096f, 6.144f};

// One conversion at 860 SPS takes about 1.2 ms. Past this the chip is gone.
constexpr uint32_t ADS_CONVERSION_TIMEOUT_US = 10000;

// PGA range for a configured gain: the smallest range that holds it.
// in: gain in V. out: index into ADS_PGA_FS_V, 0..5. NaN or > 4.096 is 5.
inline int adsPgaIndex(float gainV) {
  for (int i = 0; i < ADS_PGA_RANGES - 1; i++) {
    if (gainV <= ADS_PGA_FS_V[i]) return i;
  }
  return ADS_PGA_RANGES - 1;
}

// Full scale the chip runs at for a configured gain, which is the only
// valid scale for its counts. in: gain in V. out: full scale in V.
inline float adsPgaFullScaleV(float gainV) {
  return ADS_PGA_FS_V[adsPgaIndex(gainV)];
}

// A single-ended count at or below this sits on ground.
constexpr int16_t ADS_GROUND_COUNTS = 8;
// AC RMS in counts below this is idle noise, not a signal that swings.
constexpr double  ADS_AC_FLOOR_COUNTS = 16.0;

// Running sums over raw counts, exact in integers, and the lowest count.
struct AdsRms {
  int64_t  sum   = 0;
  int64_t  sumSq = 0;
  uint32_t n     = 0;
  int16_t  min   = std::numeric_limits<int16_t>::max();
};

inline void adsRmsAdd(AdsRms& a, int16_t raw) {
  a.sum   += raw;
  a.sumSq += (int64_t)raw * raw;
  a.n++;
  if (raw < a.min) a.min = raw;
}

// RMS of the AC part in counts. in: sums. out: counts, 0 with no samples.
inline double adsRmsAcCounts(const AdsRms& a) {
  if (a.n == 0) return 0.0;
  const double mean = (double)a.sum / a.n;
  return std::sqrt(std::fmax(0.0, (double)a.sumSq / a.n - mean * mean));
}

// RMS of the AC part, so a CT bias or ADC offset does not read as current.
// in: sums, full scale in V. out: volts, 0 with no samples.
inline float adsRmsAcVolts(const AdsRms& a, float fullScaleV) {
  return (float)(adsRmsAcCounts(a) * fullScaleV / 32768.0);
}

// Mean of the samples, the reading of a DC sensor such as a pressure
// transducer. in: sums, full scale in V. out: volts, 0 with no samples.
inline float adsMeanVolts(const AdsRms& a, float fullScaleV) {
  if (a.n == 0) return 0.0f;
  return (float)((double)a.sum / a.n * fullScaleV / 32768.0);
}

enum class AdsClip : uint8_t { Unknown, Clean, Clipped };

// Ground clipping on a single-ended AC channel, a bare CT with no bias.
// out: Clipped swings and touches ground, Clean swings clear, Unknown idle.
inline AdsClip adsClipAtGround(const AdsRms& a) {
  if (adsRmsAcCounts(a) <= ADS_AC_FLOOR_COUNTS) return AdsClip::Unknown;
  return a.min <= ADS_GROUND_COUNTS ? AdsClip::Clipped : AdsClip::Clean;
}

// Poll ready() until it holds or timeoutUs has passed since the call. The
// last poll follows the deadline, so a preempted task does not read a timeout.
template <class Ready, class NowUs>
inline bool adsWaitReady(Ready ready, NowUs nowUs, uint32_t timeoutUs) {
  const uint32_t start = nowUs();
  for (;;) {
    const bool late = (uint32_t)(nowUs() - start) >= timeoutUs;
    if (ready()) return true;
    if (late) return false;
  }
}
