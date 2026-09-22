#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* native_text_malloc(size_t bytes);
void* native_text_calloc(size_t count, size_t bytes);
void* native_text_realloc(void* pointer, size_t bytes);
void native_text_free(void* pointer);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace native_text {
inline constexpr size_t MEMORY_LIMIT = 4 * 1024 * 1024;
struct AllocationStats {
  size_t used;
  size_t peak;
  size_t failures;
};
AllocationStats allocationStats();
#ifndef ARDUINO
// SIZE_MAX disables injection; zero fails the next allocation and all later ones.
void failAllocationsAfter(size_t successfulAllocations);
#endif
}  // namespace native_text
#endif
