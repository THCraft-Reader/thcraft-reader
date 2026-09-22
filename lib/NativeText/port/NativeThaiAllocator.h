#ifndef NATIVE_TEXT_THAI_ALLOCATOR_H
#define NATIVE_TEXT_THAI_ALLOCATOR_H

/* Force-include only when compiling the LibThai/libdatrie C sources.
 * Parse the C runtime declarations before replacing allocation calls. Including
 * stdint.h also supplies SIZE_MAX on MSVC, where libdatrie omits that include.
 */
#include <stdint.h>
#include <stdlib.h>

#include "NativeAllocator.h"

#define malloc native_text_malloc
#define calloc native_text_calloc
#define realloc native_text_realloc
#define free native_text_free

#endif
