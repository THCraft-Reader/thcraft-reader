#include "HostAllocation.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
namespace {
struct Header { void* base; size_t bytes; };
std::atomic<size_t> live{0}, highWater{0};
}
namespace probe {
void* allocate(size_t bytes, size_t alignment) {
  alignment = std::max(alignment, alignof(Header));
  if (bytes > std::numeric_limits<size_t>::max() - alignment - sizeof(Header)) throw std::bad_alloc();
  void* base = std::malloc(bytes + alignment + sizeof(Header));
  if (!base) throw std::bad_alloc();
  const auto address = (reinterpret_cast<uintptr_t>(base) + sizeof(Header) + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<Header*>(address) - 1;
  header->base = base;
  header->bytes = bytes;
  const size_t current = live.fetch_add(bytes) + bytes;
  size_t prior = highWater.load();
  while (prior < current && !highWater.compare_exchange_weak(prior, current)) {}
  return reinterpret_cast<void*>(address);
}
void deallocate(void* pointer) noexcept {
  if (!pointer) return;
  auto* header = static_cast<Header*>(pointer) - 1;
  live.fetch_sub(header->bytes);
  std::free(header->base);
}
size_t allocationLive() noexcept { return live.load(); }
size_t allocationHighWater() noexcept { return highWater.load(); }
}
void* operator new(size_t n) { return probe::allocate(n); }
void* operator new[](size_t n) { return probe::allocate(n); }
void operator delete(void* p) noexcept { probe::deallocate(p); }
void operator delete[](void* p) noexcept { probe::deallocate(p); }
void operator delete(void* p, size_t) noexcept { probe::deallocate(p); }
void operator delete[](void* p, size_t) noexcept { probe::deallocate(p); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return probe::allocate(n); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return probe::allocate(n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { probe::deallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { probe::deallocate(p); }
void* operator new(size_t n, std::align_val_t a) { return probe::allocate(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a) { return probe::allocate(n, static_cast<size_t>(a)); }
void operator delete(void* p, std::align_val_t) noexcept { probe::deallocate(p); }
void operator delete[](void* p, std::align_val_t) noexcept { probe::deallocate(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { probe::deallocate(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { probe::deallocate(p); }
