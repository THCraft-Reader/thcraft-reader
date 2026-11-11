#include "ThaiStats.h"

#if THAI_ENGINE_STATS
#include <atomic>

namespace thai {
namespace {
std::atomic<uint32_t> layoutUs{0}, segmentUs{0}, drawUs{0}, inputBytes{0};
std::atomic<uint32_t> clusters{0}, words{0}, unknownClusters{0}, maxPendingBytes{0}, refreshMs{0};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Stats must not require heap-backed locks");
}  // namespace

StatsSnapshot statsSnapshot() {
  return {layoutUs.load(std::memory_order_relaxed), segmentUs.load(std::memory_order_relaxed),
          drawUs.load(std::memory_order_relaxed), inputBytes.load(std::memory_order_relaxed),
          clusters.load(std::memory_order_relaxed), words.load(std::memory_order_relaxed),
          unknownClusters.load(std::memory_order_relaxed), maxPendingBytes.load(std::memory_order_relaxed),
          refreshMs.load(std::memory_order_relaxed)};
}

void recordLayoutMicros(uint32_t elapsed) { layoutUs.fetch_add(elapsed, std::memory_order_relaxed); }
void recordSegmentMicros(uint32_t elapsed) { segmentUs.fetch_add(elapsed, std::memory_order_relaxed); }
void recordDrawMicros(uint32_t elapsed) { drawUs.fetch_add(elapsed, std::memory_order_relaxed); }
void recordRefreshMillis(uint32_t elapsed) { refreshMs.fetch_add(elapsed, std::memory_order_relaxed); }

void recordInputBytes(uint32_t bytes, uint32_t pendingBytes) {
  inputBytes.fetch_add(bytes, std::memory_order_relaxed);
  uint32_t previous = maxPendingBytes.load(std::memory_order_relaxed);
  while (previous < pendingBytes &&
         !maxPendingBytes.compare_exchange_weak(previous, pendingBytes, std::memory_order_relaxed)) {}
}

void recordSegments(uint32_t clusterCount, uint32_t wordCount, uint32_t unknownCount) {
  clusters.fetch_add(clusterCount, std::memory_order_relaxed);
  words.fetch_add(wordCount, std::memory_order_relaxed);
  unknownClusters.fetch_add(unknownCount, std::memory_order_relaxed);
}
}  // namespace thai
#endif
