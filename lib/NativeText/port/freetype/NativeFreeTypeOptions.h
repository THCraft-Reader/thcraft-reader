#ifndef NATIVE_FREETYPE_OPTIONS_H
#define NATIVE_FREETYPE_OPTIONS_H

// Inherit the pinned upstream defaults, then remove only unsupported facilities.
#include <freetype/config/ftoption.h>

#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_SYSTEM_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_GUESSING_EMBEDDED_RFORK
#undef TT_CONFIG_OPTION_EMBEDDED_BITMAPS
#undef TT_CONFIG_OPTION_COLOR_LAYERS
#undef TT_SUPPORT_COLRV1
#undef TT_CONFIG_OPTION_BDF

// Memory faces and caller-owned FT_Stream callbacks remain supported; pathname
// loading must not bypass the firmware storage adapter through stdio.
#define NATIVE_TEXT_NO_PATHNAME_STREAM 1

// Preserve upstream TrueType/CFF variation support, bytecode and auto-hinting,
// Unicode charmaps, and glyph names.  HarfBuzz owns OpenType shaping/positioning.
#endif
