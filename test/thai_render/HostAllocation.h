#pragma once
#include <cstddef>
namespace probe {
void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));
void deallocate(void* pointer) noexcept;
size_t allocationLive() noexcept;
size_t allocationHighWater() noexcept;
}
