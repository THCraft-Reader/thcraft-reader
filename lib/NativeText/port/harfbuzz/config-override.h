#ifndef NATIVE_HARFBUZZ_CONFIG_H
#define NATIVE_HARFBUZZ_CONFIG_H

#define HAVE_FREETYPE 1
// These APIs are supplied by the pinned FreeType 2.14.3 build, not discovered
// from a host installation.  They keep FT/HB variation and transform state in sync.
#define HAVE_FT_GET_VAR_BLEND_COORDINATES 1
#define HAVE_FT_DONE_MM_VAR 1
#define HAVE_FT_GET_TRANSFORM 1

#define HB_CUSTOM_MALLOC

#ifndef HB_OPTIMIZE_SIZE
#define HB_OPTIMIZE_SIZE
#endif
#define HB_NO_AAT
#define HB_NO_MATH
#define HB_NO_COLOR
#define HB_NO_DRAW
#define HB_NO_PAINT
#define HB_NO_MMAP
#define HB_NO_OPEN
#define HB_NO_GETENV
#define HB_NO_SETLOCALE
#define HB_NO_BUFFER_SERIALIZE
#define HB_NO_BUFFER_VERIFY

// Do not select HB_TINY/HB_MINI/HB_LEAN: they remove legacy kern, variations,
// and shaping fallbacks.  All OpenType script shapers and Unicode data remain.
// The amalgamation excludes subset/raster/vector/GPU libraries.  Disable
// external backends even if the surrounding firmware supplies feature macros.
#undef HAVE_CORETEXT
#undef HAVE_DIRECTWRITE
#undef HAVE_GDI
#undef HAVE_UNISCRIBE
#undef HAVE_GLIB
#undef HAVE_ICU
#undef HAVE_GRAPHITE2
#undef HAVE_WASM
#undef HB_HAS_CAIRO
#undef HB_HAS_SUBSET
#undef HB_HAS_RASTER
#undef HB_HAS_VECTOR
#undef HB_HAS_GPU

#endif
