#include <HalMemory.h>
#include <NativeAllocator.h>

namespace {
HalMemory::HeapStats nativeBudget() {
  const auto stats = native_text::allocationStats();
  return {native_text::MEMORY_LIMIT - stats.used, native_text::MEMORY_LIMIT, native_text::MEMORY_LIMIT - stats.peak,
          native_text::MEMORY_LIMIT - stats.used};
}
}  // namespace
HalMemory::HeapStats HalMemory::getDefaultHeap() { return nativeBudget(); }
HalMemory::HeapStats HalMemory::getInternalHeap() { return {}; }  // No ESP32 internal heap on the host.
HalMemory::HeapStats HalMemory::getPsramHeap() { return nativeBudget(); }
void* HalMemory::allocateExternal(size_t bytes) { return native_text_malloc(bytes); }
void* HalMemory::reallocateExternal(void* pointer, size_t bytes) { return native_text_realloc(pointer, bytes); }
void HalMemory::freeExternal(void* pointer) { native_text_free(pointer); }
