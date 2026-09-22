#include "HalMemory.h"

#include <esp_heap_caps.h>

#include <cstdint>

namespace {
HalMemory::HeapStats readHeapStats(uint32_t capabilities) {
  return {heap_caps_get_free_size(capabilities), heap_caps_get_total_size(capabilities),
          heap_caps_get_minimum_free_size(capabilities), heap_caps_get_largest_free_block(capabilities)};
}
}  // namespace

HalMemory::HeapStats HalMemory::getDefaultHeap() { return readHeapStats(MALLOC_CAP_DEFAULT); }

HalMemory::HeapStats HalMemory::getInternalHeap() { return readHeapStats(MALLOC_CAP_INTERNAL); }

HalMemory::HeapStats HalMemory::getPsramHeap() { return readHeapStats(MALLOC_CAP_SPIRAM); }

void* HalMemory::allocateExternal(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void* HalMemory::reallocateExternal(void* pointer, size_t bytes) {
  return heap_caps_realloc(pointer, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void HalMemory::freeExternal(void* pointer) { heap_caps_free(pointer); }
