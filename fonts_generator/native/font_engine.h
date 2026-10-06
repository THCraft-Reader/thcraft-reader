#ifndef FONT_GENERATOR_ENGINE_H
#define FONT_GENERATOR_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FGFace FGFace;
typedef struct {
    int32_t ascender, descender, line_height;
    uint32_t x_ppem, y_ppem;
} FGMetrics;
typedef struct {
    uint32_t width, height;
    int32_t left, top;
    uint32_t advance_fp;
    int32_t pitch;
    const uint8_t *pixels;
} FGBitmap;
typedef struct {
    uint32_t glyph_id;
    int32_t advance_fp, x_fp, y_fp, y_advance_fp;
} FGShapedGlyph;

FGFace *fg_open(const char *path, int point_size, int autohint);
void fg_close(FGFace *face);
uint32_t fg_glyph_index(FGFace *face, uint32_t codepoint);
int fg_metrics(FGFace *face, FGMetrics *output);
int fg_raster(FGFace *face, uint32_t glyph_id, FGBitmap *output);
int fg_shape(FGFace *face, const uint32_t *codepoints, uint32_t count,
             FGShapedGlyph *output, uint32_t capacity);
const char *fg_error(void);
const char *fg_freetype_version(void);
const char *fg_harfbuzz_version(void);

#ifdef __cplusplus
}
#endif
#endif
