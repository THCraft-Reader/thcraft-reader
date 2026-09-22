#include "NativeAllocator.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifdef ARDUINO
#include <HalMemory.h>
#endif

namespace {
struct alignas(std::max_align_t) AllocationHeader {
  size_t bytes;
};

#ifdef NATIVE_TEXT_DICTIONARY_BUILDER
constexpr size_t ALLOCATION_LIMIT = 64 * 1024 * 1024;
#else
constexpr size_t ALLOCATION_LIMIT = native_text::MEMORY_LIMIT;
#endif
std::atomic<size_t> usedBytes{0};
std::atomic<size_t> peakBytes{0};
std::atomic<size_t> failures{0};
#ifndef ARDUINO
std::atomic<size_t> allocationsRemaining{std::numeric_limits<size_t>::max()};
#endif

bool reserve(size_t bytes) {
  size_t used = usedBytes.load(std::memory_order_relaxed);
  do {
    if (bytes > ALLOCATION_LIMIT - used) return false;
  } while (!usedBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
  return true;
}

void updatePeak() {
  const size_t used = usedBytes.load(std::memory_order_relaxed);
  size_t peak = peakBytes.load(std::memory_order_relaxed);
  while (peak < used && !peakBytes.compare_exchange_weak(peak, used, std::memory_order_relaxed)) {
  }
}

bool injectedFailure() {
#ifndef ARDUINO
  size_t remaining = allocationsRemaining.load(std::memory_order_relaxed);
  while (remaining != std::numeric_limits<size_t>::max()) {
    if (!remaining) return true;
    if (allocationsRemaining.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) break;
  }
#endif
  return false;
}

void* failed() {
  failures.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}
}  // namespace

extern "C" void* native_text_malloc(size_t bytes) {
  // A non-null zero-sized allocation is required by some vendor growth paths.
  if (!bytes) bytes = 1;
  if (bytes > ALLOCATION_LIMIT - sizeof(AllocationHeader) || injectedFailure()) return failed();
  const size_t total = bytes + sizeof(AllocationHeader);
  if (!reserve(total)) return failed();
#ifdef ARDUINO
  void* memory = HalMemory::allocateExternal(total);
#else
  void* memory = std::malloc(total);
#endif
  if (!memory) {
    usedBytes.fetch_sub(total, std::memory_order_relaxed);
    return failed();
  }
  auto* header = static_cast<AllocationHeader*>(memory);
  header->bytes = bytes;
  updatePeak();
  return header + 1;
}

extern "C" void native_text_free(void* pointer) {
  if (!pointer) return;
  auto* header = static_cast<AllocationHeader*>(pointer) - 1;
  const size_t total = header->bytes + sizeof(AllocationHeader);
#ifdef ARDUINO
  HalMemory::freeExternal(header);
#else
  std::free(header);
#endif
  usedBytes.fetch_sub(total, std::memory_order_relaxed);
}

extern "C" void* native_text_calloc(size_t count, size_t bytes) {
  if (bytes && count > std::numeric_limits<size_t>::max() / bytes) return failed();
  const size_t total = count * bytes;
  void* memory = native_text_malloc(total);
  if (memory) std::memset(memory, 0, total);
  return memory;
}

extern "C" void* native_text_realloc(void* pointer, size_t bytes) {
  if (!pointer) return native_text_malloc(bytes);
  if (!bytes) {
    native_text_free(pointer);
    return nullptr;
  }
  auto* header = static_cast<AllocationHeader*>(pointer) - 1;
  const size_t oldBytes = header->bytes;
  if (bytes == oldBytes) return pointer;
  if (bytes > ALLOCATION_LIMIT - sizeof(AllocationHeader) || injectedFailure()) return failed();
  const size_t growth = bytes > oldBytes ? bytes - oldBytes : 0;
  if (growth && !reserve(growth)) return failed();
#ifdef ARDUINO
  void* memory = HalMemory::reallocateExternal(header, bytes + sizeof(AllocationHeader));
#else
  void* memory = std::realloc(header, bytes + sizeof(AllocationHeader));
#endif
  if (!memory) {
    if (growth) usedBytes.fetch_sub(growth, std::memory_order_relaxed);
    return failed();
  }
  if (bytes < oldBytes) usedBytes.fetch_sub(oldBytes - bytes, std::memory_order_relaxed);
  auto* resized = static_cast<AllocationHeader*>(memory);
  resized->bytes = bytes;
  updatePeak();
  return resized + 1;
}

namespace native_text {
AllocationStats allocationStats() {
  return {usedBytes.load(std::memory_order_relaxed), peakBytes.load(std::memory_order_relaxed),
          failures.load(std::memory_order_relaxed)};
}
#ifndef ARDUINO
void failAllocationsAfter(size_t successfulAllocations) {
  allocationsRemaining.store(successfulAllocations, std::memory_order_relaxed);
}
#endif
}  // namespace native_text
