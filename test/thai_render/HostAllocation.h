#pragma once
#include <cstddef>
namespace probe {
enum class NullableAllocationKind { Any, Scalar, Array };
struct NullableAllocationFailure {
  size_t bytes = 0;    // Zero matches any size.
  size_t ordinal = 0;  // Zero disables injection; otherwise one-based among matches.
  NullableAllocationKind kind = NullableAllocationKind::Any;
  size_t matches = 0;
  size_t failures = 0;
};
NullableAllocationFailure nullableAllocationFailure() noexcept;
void setNullableAllocationFailure(NullableAllocationFailure failure = {}) noexcept;
class ScopedNullableAllocationFailure {
 public:
  explicit ScopedNullableAllocationFailure(size_t bytes, size_t ordinal = 1,
                                           NullableAllocationKind kind = NullableAllocationKind::Any) noexcept;
  ~ScopedNullableAllocationFailure();
  ScopedNullableAllocationFailure(const ScopedNullableAllocationFailure&) = delete;
  ScopedNullableAllocationFailure& operator=(const ScopedNullableAllocationFailure&) = delete;

 private:
  NullableAllocationFailure previous_;
};
void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t));
void deallocate(void* pointer) noexcept;
size_t allocationLive() noexcept;
size_t allocationHighWater() noexcept;
size_t allocationLiveCount() noexcept;
size_t allocationCalls() noexcept;
}  // namespace probe
