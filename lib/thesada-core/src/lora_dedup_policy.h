// thesada-fw - lora_dedup_policy.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mesh {

// Fixed-size FIFO ring of recently seen (src, packetId) pairs. Meshtastic
// broadcast retransmits reuse the id - repeats must not republish to MQTT.
template <size_t N = 16>
struct DedupRing {
  uint32_t src[N] = {0};
  uint32_t id[N]  = {0};
  size_t   next   = 0;
  size_t   count  = 0;

  // True if (s, pid) was seen recently; records it otherwise (FIFO evict).
  // in: sender node, packet id. out: true = duplicate, caller drops it.
  bool seenAndRecord(uint32_t s, uint32_t pid) {
    if (pid == 0) return false;  // id 0 = untracked, never drop
    for (size_t i = 0; i < count; i++) {
      if (src[i] == s && id[i] == pid) return true;
    }
    src[next] = s;
    id[next] = pid;
    next = (next + 1) % N;
    if (count < N) count++;
    return false;
  }
};

}  // namespace mesh
