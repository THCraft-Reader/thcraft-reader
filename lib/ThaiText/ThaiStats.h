#pragma once

#include "ThaiConfig.h"

#if THAI_ENGINE_STATS
#include <cstdint>

namespace thai {
// Cumulative, modulo-2^32 counters. Subtract scoped snapshots with unsigned
// arithmetic; never reset counters shared by the render and reader tasks.
// Each field is atomic independently, not a transaction across all fields.
struct StatsSnapshot {
  uint32_t layout_us;
  uint32_t segment_us;
  uint32_t draw_us;
  uint32_t input_bytes;
  uint32_t clusters;
  uint32_t words;
  uint32_t unknown_clusters;
  uint32_t max_pending_bytes;
  uint32_t refresh_ms;
};

StatsSnapshot statsSnapshot();
void recordLayoutMicros(uint32_t elapsed);
void recordSegmentMicros(uint32_t elapsed);
void recordDrawMicros(uint32_t elapsed);
void recordInputBytes(uint32_t bytes, uint32_t pendingBytes);
// Counts committed valid clusters, dictionary words, and unknown Thai clusters;
// lookahead retries, punctuation and malformed-source fallback are not unknowns.
void recordSegments(uint32_t clusters, uint32_t words, uint32_t unknownClusters);
// HAL request start through software-observed completion, including transfers
// and any delay before the caller waits. Not BUSY-pin waveform time or CPU wait.
void recordRefreshMillis(uint32_t elapsed);
}  // namespace thai
#endif
