#pragma once

#include <cstddef>

class HalMemory {
 public:
  struct HeapStats {
    size_t freeBytes;
    size_t totalBytes;
    size_t minFreeBytes;
    size_t largestBlockBytes;
  };

  // Default-capability memory includes PSRAM when registered with the allocator.
  static HeapStats getDefaultHeap();
  static HeapStats getInternalHeap();
  static HeapStats getPsramHeap();

  static void* allocateExternal(size_t bytes);
  static void* reallocateExternal(void* pointer, size_t bytes);
  static void freeExternal(void* pointer);
};
