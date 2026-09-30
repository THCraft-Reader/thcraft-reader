#pragma once

#include <cstddef>

namespace probe {
// Simulated allocation headroom, never exported as a device heap measurement.
inline size_t internalHeadroomBytes = 256u * 1024u * 1024u;
}

class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };
  static HeapStats getInternal8BitHeap() {
    const size_t free = probe::internalHeadroomBytes;
    return {free, free, free, free};
  }
};
