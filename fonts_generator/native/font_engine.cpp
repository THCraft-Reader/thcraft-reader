#include "font_engine.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ot.h>

#include <climits>
#include <cstdio>
#include <new>
#include <vector>

struct FGFace {
    FT_Library library = nullptr;
    FT_Face ft = nullptr;
    hb_blob_t *blob = nullptr;
    hb_font_t *font = nullptr;
    hb_buffer_t *buffer = nullptr;
    FT_Int32 load_flags = FT_LOAD_RENDER;
    std::vector<uint8_t> normalized;
};

static char last_error[256] = {};

static int fail(const char *message, int code = 0) {
    if (code) {
        std::snprintf(last_error, sizeof(last_error), "%s (error %d)", message, code);
    } else {
        std::snprintf(last_error, sizeof(last_error), "%s", message);
    }
    return 0;
}

static int64_t floor_div(int64_t value, int64_t divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}

static int32_t fp4(hb_position_t value) {
    return static_cast<int32_t>(floor_div(static_cast<int64_t>(value) + 2, 4));
}

extern "C" const char *fg_error(void) { return last_error; }
#define FG_STRINGIFY_INNER(value) #value
#define FG_STRINGIFY(value) FG_STRINGIFY_INNER(value)
extern "C" const char *fg_freetype_version(void) {
    return FG_STRINGIFY(FREETYPE_MAJOR) "." FG_STRINGIFY(FREETYPE_MINOR) "." FG_STRINGIFY(FREETYPE_PATCH);
}
extern "C" const char *fg_harfbuzz_version(void) { return hb_version_string(); }

extern "C" void fg_close(FGFace *face) {
    if (!face) return;
    if (face->buffer) hb_buffer_destroy(face->buffer);
    if (face->font) hb_font_destroy(face->font);
    if (face->ft) FT_Done_Face(face->ft);
    if (face->library) FT_Done_FreeType(face->library);
    if (face->blob) hb_blob_destroy(face->blob);
    delete face;
}

extern "C" FGFace *fg_open(const char *path, int point_size, int autohint) {
    last_error[0] = 0;
    if (!path || point_size <= 0 || point_size > 255) {
        fail("Invalid font path or point size");
        return nullptr;
    }
    FGFace *face = new (std::nothrow) FGFace;
    if (!face) {
        fail("Cannot allocate font face");
        return nullptr;
    }
    face->blob = hb_blob_create_from_file_or_fail(path);
    if (!face->blob || hb_blob_get_length(face->blob) > LONG_MAX) {
        fail("Cannot read font file");
        fg_close(face);
        return nullptr;
    }
    FT_Error error = FT_Init_FreeType(&face->library);
    if (!error) {
        unsigned length = 0;
        const char *data = hb_blob_get_data(face->blob, &length);
        error = FT_New_Memory_Face(face->library, reinterpret_cast<const FT_Byte *>(data),
                                   static_cast<FT_Long>(length), 0, &face->ft);
    }
    if (!error) error = FT_Select_Charmap(face->ft, FT_ENCODING_UNICODE);
    if (!error) error = FT_Set_Char_Size(face->ft, point_size * 64, point_size * 64, 150, 150);
    if (error) {
        fail("Cannot initialize font face", error);
        fg_close(face);
        return nullptr;
    }
    hb_face_t *hb_face = hb_face_create(face->blob, 0);
    face->font = hb_font_create(hb_face);
    hb_face_destroy(hb_face);
    face->buffer = hb_buffer_create();
    if (!hb_font_get_face(face->font) || !hb_face_get_glyph_count(hb_font_get_face(face->font)) ||
        !hb_buffer_allocation_successful(face->buffer)) {
        fail("Cannot initialize HarfBuzz font");
        fg_close(face);
        return nullptr;
    }
    hb_ot_font_set_funcs(face->font);
    // point_size * 150 / 72 * 64 = point_size * 400 / 3; no half ties.
    const int scale = (point_size * 400 + 1) / 3;
    hb_font_set_scale(face->font, scale, scale);
    hb_font_set_ppem(face->font, face->ft->size->metrics.x_ppem, face->ft->size->metrics.y_ppem);
    if (autohint) face->load_flags |= FT_LOAD_FORCE_AUTOHINT;
    return face;
}

extern "C" uint32_t fg_glyph_index(FGFace *face, uint32_t codepoint) {
    return face ? FT_Get_Char_Index(face->ft, codepoint) : 0;
}

extern "C" int fg_metrics(FGFace *face, FGMetrics *output) {
    last_error[0] = 0;
    if (!face || !output) return fail("Invalid metrics arguments");
    const FT_Size_Metrics &m = face->ft->size->metrics;
    output->ascender = static_cast<int32_t>(-floor_div(-static_cast<int64_t>(m.ascender), 64));
    output->descender = static_cast<int32_t>(floor_div(m.descender, 64));
    output->line_height = static_cast<int32_t>(-floor_div(-static_cast<int64_t>(m.height), 64));
    output->x_ppem = m.x_ppem;
    output->y_ppem = m.y_ppem;
    return 1;
}

extern "C" int fg_raster(FGFace *face, uint32_t glyph_id, FGBitmap *output) {
    last_error[0] = 0;
    if (!face || !output) return fail("Invalid raster arguments");
    FT_Error error = FT_Load_Glyph(face->ft, glyph_id, face->load_flags);
    if (error) return fail("Cannot rasterize glyph", error);
    const FT_GlyphSlot slot = face->ft->glyph;
    const FT_Bitmap &bitmap = slot->bitmap;
    const int64_t advance = floor_div(static_cast<int64_t>(slot->linearHoriAdvance) + 2048, 4096);
    if (advance < 0 || advance > UINT32_MAX) return fail("Glyph advance is outside the unsigned fixed-point range");
    *output = {bitmap.width, bitmap.rows, slot->bitmap_left, slot->bitmap_top,
               static_cast<uint32_t>(advance), bitmap.pitch, bitmap.buffer};
    if (!bitmap.width || !bitmap.rows) return 1;
    if (!bitmap.buffer || bitmap.pitch == INT_MIN) return fail("Invalid FreeType bitmap storage");
    if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && bitmap.num_grays == 256 && bitmap.pitch > 0) {
        if (static_cast<uint32_t>(bitmap.pitch) < bitmap.width) return fail("Invalid FreeType grayscale pitch");
        return 1;
    }
    unsigned bits = 0;
    switch (bitmap.pixel_mode) {
        case FT_PIXEL_MODE_MONO: bits = 1; break;
        case FT_PIXEL_MODE_GRAY2: bits = 2; break;
        case FT_PIXEL_MODE_GRAY4: bits = 4; break;
        case FT_PIXEL_MODE_GRAY: bits = 8; break;
        default: return fail("Unsupported FreeType bitmap pixel mode", bitmap.pixel_mode);
    }
    const size_t pitch = static_cast<size_t>(bitmap.pitch < 0 ? -bitmap.pitch : bitmap.pitch);
    if (pitch < (static_cast<size_t>(bitmap.width) * bits + 7) / 8 ||
        bitmap.width > INT_MAX || bitmap.rows > SIZE_MAX / bitmap.width) {
        return fail("Invalid FreeType bitmap dimensions");
    }
    if (bits == 8 && bitmap.num_grays < 2) return fail("Invalid FreeType grayscale palette");
    face->normalized.resize(static_cast<size_t>(bitmap.width) * bitmap.rows);
    const unsigned maximum = bits == 8 ? bitmap.num_grays - 1 : (1u << bits) - 1;
    for (uint32_t y = 0; y < bitmap.rows; ++y) {
        const size_t row = bitmap.pitch >= 0 ? y : bitmap.rows - 1 - y;
        const uint8_t *source = bitmap.buffer + row * pitch;
        uint8_t *target = face->normalized.data() + static_cast<size_t>(y) * bitmap.width;
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            const unsigned offset = (x % (8 / bits)) * bits;
            const unsigned sample = (source[x / (8 / bits)] >> (8 - bits - offset)) & ((1u << bits) - 1);
            if (sample > maximum) return fail("Invalid FreeType grayscale sample");
            target[x] = static_cast<uint8_t>((sample * 255 + maximum / 2) / maximum);
        }
    }
    output->pitch = static_cast<int32_t>(bitmap.width);
    output->pixels = face->normalized.data();
    return 1;
}

extern "C" int fg_shape(FGFace *face, const uint32_t *codepoints, uint32_t count,
                         FGShapedGlyph *output, uint32_t capacity) {
    last_error[0] = 0;
    if (!face || (!codepoints && count) || (!output && capacity) || count > INT_MAX) {
        fail("Invalid shaping arguments");
        return -1;
    }
    hb_buffer_reset(face->buffer);
    hb_buffer_add_codepoints(face->buffer, codepoints, static_cast<int>(count), 0, static_cast<int>(count));
    hb_buffer_set_script(face->buffer, HB_SCRIPT_THAI);
    hb_buffer_set_language(face->buffer, hb_language_from_string("th", -1));
    hb_buffer_set_direction(face->buffer, HB_DIRECTION_LTR);
    hb_shape(face->font, face->buffer, nullptr, 0);
    if (!hb_buffer_allocation_successful(face->buffer)) {
        fail("HarfBuzz shaping allocation failed");
        return -1;
    }
    unsigned length = 0;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(face->buffer, &length);
    const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(face->buffer, nullptr);
    if (length > capacity || length > INT_MAX) {
        fail("Shaping output capacity is too small");
        return -1;
    }
    for (unsigned i = 0; i < length; ++i) {
        if (positions[i].y_advance) {
            fail("Thai shaping produced a nonzero vertical advance");
            return -1;
        }
        output[i] = {info[i].codepoint, fp4(positions[i].x_advance),
                     fp4(positions[i].x_offset), fp4(positions[i].y_offset), 0};
    }
    return static_cast<int>(length);
}
