#ifndef NATIVE_FREETYPE_STDLIB_H
#define NATIVE_FREETYPE_STDLIB_H

#include <freetype/config/ftstdlib.h>

#include "NativeAllocator.h"

// Route even FT_Init_FreeType's default memory manager through the shared
// allocator; FT_New_Library may still supply its own FT_Memory callbacks.
#undef ft_smalloc
#undef ft_scalloc
#undef ft_srealloc
#undef ft_sfree
#define ft_smalloc native_text_malloc
#define ft_scalloc native_text_calloc
#define ft_srealloc native_text_realloc
#define ft_sfree native_text_free

#endif
