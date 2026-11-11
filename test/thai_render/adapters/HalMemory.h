#pragma once

#include <cstddef>
#include <initializer_list>
#include <vector>

namespace probe {
// Simulated allocation headroom, never exported as a device heap measurement.
inline size_t internalHeadroomBytes = 256u * 1024u * 1024u;
struct InternalHeapSample {
  size_t freeBytes;
  size_t largestBlockBytes;
};
inline std::vector<InternalHeapSample> internalHeapSamples;
inline size_t internalHeapSampleIndex = 0;
inline void setInternalHeapSamples(std::initializer_list<InternalHeapSample> samples) {
  internalHeapSamples.assign(samples);
  internalHeapSampleIndex = 0;
}
inline void resetInternalHeapSamples() {
  internalHeapSamples.clear();
  internalHeapSampleIndex = 0;
}
}  // namespace probe

class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };
  static HeapStats getInternal8BitHeap() {
    if (!probe::internalHeapSamples.empty()) {
      const auto& sample = probe::internalHeapSamples[probe::internalHeapSampleIndex];
      if (probe::internalHeapSampleIndex + 1 < probe::internalHeapSamples.size()) ++probe::internalHeapSampleIndex;
      return {sample.freeBytes, sample.freeBytes, sample.freeBytes, sample.largestBlockBytes};
    }
    const size_t free = probe::internalHeadroomBytes;
    return {free, free, free, free};
  }
  static HeapStats getDefaultHeap() { return getInternal8BitHeap(); }
  static HeapStats getPsramHeap() { return {0, 0, 0, 0}; }
};
