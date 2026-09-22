#include "NativeAllocator.h"

extern "C" void* hb_malloc_impl(size_t bytes) { return native_text_malloc(bytes); }
extern "C" void* hb_calloc_impl(size_t count, size_t bytes) { return native_text_calloc(count, bytes); }
extern "C" void* hb_realloc_impl(void* pointer, size_t bytes) { return native_text_realloc(pointer, bytes); }
extern "C" void hb_free_impl(void* pointer) { native_text_free(pointer); }
