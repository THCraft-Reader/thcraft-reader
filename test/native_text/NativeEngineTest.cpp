#include <NativeAllocator.h>
#include <NativeFontAssets.generated.h>
#include <NativeTextEngine.h>
#include <NativeUtf8.h>
#include <fontIds.h>
#include <ft2build.h>
#include <gtest/gtest.h>
#include FT_FREETYPE_H
#include <hb-ft.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr int CUSTOM_FONT_ID = 991731;

class NativeTextEngineTest : public testing::Test {
 protected:
  NativeTextEngine engine;
  void SetUp() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    engine.shutdown();
    EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
  }
  NativeLineInput input(std::string_view text, int font = NOTOSANS_14_FONT_ID) {
    NativeLineInput result;
    result.text = text;
    result.fontId = font;
    return result;
  }
};

struct ReferenceFace {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  hb_font_t* font = nullptr;
  hb_buffer_t* buffer = nullptr;
  bool openAsset(const char* name) {
    const native_text::assets::FontAsset* asset = nullptr;
    for (size_t i = 0; i < native_text::assets::fontCount; ++i)
      if (!std::strcmp(native_text::assets::fonts[i].name, name)) asset = &native_text::assets::fonts[i];
    if (!asset || FT_Init_FreeType(&library) ||
        FT_New_Memory_Face(library, asset->data, static_cast<FT_Long>(asset->size), 0, &face) ||
        FT_Set_Char_Size(face, 0, 14 * 64, 150, 150))
      return false;
    font = hb_ft_font_create_referenced(face);
    hb_ft_font_set_load_flags(font, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL);
    buffer = hb_buffer_create();
    return font && buffer && hb_buffer_allocation_successful(buffer);
  }
  void shape(std::string_view text) {
    hb_buffer_clear_contents(buffer);
    hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer, HB_SCRIPT_THAI);
    hb_buffer_set_language(buffer, hb_language_from_string("th", -1));
    hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_set_flags(buffer, static_cast<hb_buffer_flags_t>(HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT));
    hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    const hb_feature_t feature{HB_TAG('p', 'n', 'u', 'm'), 1, 0, HB_FEATURE_GLOBAL_END};
    hb_shape(font, buffer, &feature, 1);
  }
  ~ReferenceFace() {
    if (buffer) hb_buffer_destroy(buffer);
    if (font) hb_font_destroy(font);
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
  }
};

void equalGeometry(const NativeGlyphRun& a, const NativeGlyphRun& b) {
  ASSERT_EQ(a.glyphs.size(), b.glyphs.size());
  ASSERT_EQ(a.clusters.size(), b.clusters.size());
  EXPECT_EQ(a.advance26, b.advance26);
  EXPECT_EQ(a.ascender26, b.ascender26);
  EXPECT_EQ(a.descender26, b.descender26);
  EXPECT_EQ(a.ink.left26, b.ink.left26);
  EXPECT_EQ(a.ink.top26, b.ink.top26);
  EXPECT_EQ(a.ink.right26, b.ink.right26);
  EXPECT_EQ(a.ink.bottom26, b.ink.bottom26);
  for (size_t i = 0; i < a.glyphs.size(); ++i) {
    EXPECT_EQ(a.glyphs[i].glyphId, b.glyphs[i].glyphId);
    EXPECT_EQ(a.glyphs[i].faceIdentity, b.glyphs[i].faceIdentity);
    EXPECT_EQ(a.glyphs[i].startByte, b.glyphs[i].startByte);
    EXPECT_EQ(a.glyphs[i].endByte, b.glyphs[i].endByte);
    EXPECT_EQ(a.glyphs[i].x26, b.glyphs[i].x26);
    EXPECT_EQ(a.glyphs[i].y26, b.glyphs[i].y26);
    EXPECT_EQ(a.glyphs[i].advanceX26, b.glyphs[i].advanceX26);
  }
}

TEST_F(NativeTextEngineTest, ThaiMarkPositionsMatchPinnedFreeTypeHarfBuzz) {
  ReferenceFace reference;
  ASSERT_TRUE(reference.openAsset("NotoSansThai-Regular"));
  for (const char* text : {"กิ", "กี่", "กึ", "กุ", "กู้", "น้ำ", "กำ", "ปี่", "ญู", "ฐู", "เก่ง", "นํ้า", "กํา"}) {
    SCOPED_TRACE(text);
    NativeGlyphRun run;
    ASSERT_EQ(engine.shapeLine(input(text), run), TextStatus::Ok);
    reference.shape(text);
    unsigned count = 0;
    const auto* infos = hb_buffer_get_glyph_infos(reference.buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(reference.buffer, nullptr);
    ASSERT_EQ(run.glyphs.size(), count);
    int32_t pen = 0;
    for (unsigned i = 0; i < count; ++i) {
      EXPECT_NE(run.glyphs[i].glyphId, 0u);
      EXPECT_EQ(run.glyphs[i].glyphId, infos[i].codepoint);
      EXPECT_EQ(run.glyphs[i].x26, pen + positions[i].x_offset);
      EXPECT_EQ(run.glyphs[i].y26, -positions[i].y_offset);
      EXPECT_EQ(run.glyphs[i].advanceX26, positions[i].x_advance);
      pen += positions[i].x_advance;
    }
    EXPECT_EQ(run.advance26, pen);
  }
}

TEST_F(NativeTextEngineTest, InlineMarkStylesDoNotSplitTheirBaseOrLeadingVowelCell) {
  const std::string text = "เก่ง";
  NativeGlyphRun regular, marked;
  ASSERT_EQ(engine.shapeLine(input(text), regular), TextStatus::Ok);
  std::array<NativeStyleSpan, 3> spans{{{0, 6, 0, 0}, {6, 9, 1, 0}, {9, static_cast<uint32_t>(text.size()), 0, 0}}};
  auto line = input(text);
  line.spans = spans;
  ASSERT_EQ(engine.shapeLine(line, marked), TextStatus::Ok);
  // The leading vowel and its consonant/marks are one protected base-owned cell.
  for (const auto& cluster : marked.clusters.span()) {
    EXPECT_NE(cluster.startByte, 3u);
    EXPECT_NE(cluster.endByte, 3u);
  }
  equalGeometry(regular, marked);
}

TEST_F(NativeTextEngineTest, OriginalNfdByteCoordinatesSurviveShapingAndFallback) {
  const std::string text = "e\xcc\x81กี่";
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input(text), run), TextStatus::Ok);
  ASSERT_FALSE(run.clusters.empty());
  EXPECT_EQ(run.clusters[0].startByte, 0u);
  EXPECT_EQ(run.clusters[0].endByte, 3u);
  uint32_t covered = 0;
  for (const auto& cluster : run.clusters.span()) {
    EXPECT_EQ(cluster.startByte, covered);
    EXPECT_TRUE(native_text::utf8Boundary(text, cluster.startByte));
    EXPECT_TRUE(native_text::utf8Boundary(text, cluster.endByte));
    covered = cluster.endByte;
  }
  EXPECT_EQ(covered, text.size());
}

TEST_F(NativeTextEngineTest, SyntheticHyphenHasAdvanceButNoSourceSpan) {
  const std::string text = "ภาษาไทย";
  NativeGlyphRun plain, hyphenated;
  ASSERT_EQ(engine.shapeLine(input(text), plain), TextStatus::Ok);
  auto line = input(text);
  line.syntheticSuffixCp = '-';
  ASSERT_EQ(engine.shapeLine(line, hyphenated), TextStatus::Ok);
  ASSERT_GT(hyphenated.glyphs.size(), plain.glyphs.size());
  const auto& suffix = hyphenated.glyphs[hyphenated.glyphs.size() - 1];
  EXPECT_EQ(suffix.startByte, text.size());
  EXPECT_EQ(suffix.endByte, text.size());
  EXPECT_GT(suffix.advanceX26, 0);
  EXPECT_GT(hyphenated.advance26, plain.advance26);
}

TEST_F(NativeTextEngineTest, ExplicitParagraphLevelsControlVisualOrderAndTrailingWhitespace) {
  const std::string text = "אבA ";
  std::array<NativeStyleSpan, 3> spans{{{0, 4, 0, 1}, {4, 5, 0, 2}, {5, 6, 0, 1}}};
  auto line = input(text);
  line.spans = spans;
  line.paragraphLevel = 0;
  line.resolvedLevels = true;
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(line, run), TextStatus::Ok);
  ASSERT_EQ(run.clusters.size(), 4u);
  EXPECT_EQ(run.clusters[0].startByte, 4u);  // Latin embedding moves before Hebrew.
  EXPECT_EQ(run.clusters[1].startByte, 2u);
  EXPECT_EQ(run.clusters[2].startByte, 0u);
  EXPECT_EQ(run.clusters[3].startByte, 5u);  // L1 restores trailing space to base 0.
  EXPECT_EQ(run.clusters[3].bidiLevel, 0u);
}

TEST_F(NativeTextEngineTest, ExhaustedFallbackUsesVisibleReplacementForWholeCluster) {
  const std::string text = "\xf4\x8f\xbf\xbf\xcc\x81";  // U+10FFFF plus an attached acute.
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input(text), run), TextStatus::Ok);
  ASSERT_EQ(run.clusters.size(), 1u);
  EXPECT_EQ(run.clusters[0].startByte, 0u);
  EXPECT_EQ(run.clusters[0].endByte, text.size());
  EXPECT_GT(run.advance26, 0);
  for (const auto& glyph : run.glyphs.span()) {
    EXPECT_NE(glyph.glyphId, 0u);
    NativeBitmapView bitmap;
    ASSERT_EQ(engine.rasterize(glyph, bitmap), TextStatus::Ok);
    EXPECT_GT(bitmap.width, 0);
    EXPECT_GT(bitmap.height, 0);
  }
}

TEST_F(NativeTextEngineTest, GapsAreAppliedOnceAndPartOfTheRunCacheKey) {
  const std::string text = "A B";
  NativeGlyphRun plain, expanded, cached;
  ASSERT_EQ(engine.shapeLine(input(text), plain), TextStatus::Ok);
  std::array<NativeGap, 1> gaps{{{2, 5 * 64}}};
  auto line = input(text);
  line.gaps = gaps;
  ASSERT_EQ(engine.shapeLine(line, expanded), TextStatus::Ok);
  ASSERT_EQ(engine.shapeLine(line, cached), TextStatus::Ok);
  EXPECT_EQ(expanded.advance26, plain.advance26 + 5 * 64);
  ASSERT_EQ(expanded.glyphs.size(), plain.glyphs.size());
  EXPECT_EQ(expanded.glyphs[0].x26, plain.glyphs[0].x26);
  EXPECT_EQ(expanded.glyphs[2].x26, plain.glyphs[2].x26 + 5 * 64);
  equalGeometry(expanded, cached);
}

TEST_F(NativeTextEngineTest, SyntheticStylesMatchRasterBoundsAndLeaveMarkAdvancesZero) {
  const std::array<NativeFontFile, 1> files{{{NATIVE_FONT_PATH, 0, {}}}};
  ASSERT_EQ(engine.registerCustomFont(CUSTOM_FONT_ID, 14, files, false), TextStatus::Ok);
  const std::string text = "กี่";
  NativeGlyphRun regular, bold, italic;
  auto line = input(text, CUSTOM_FONT_ID);
  ASSERT_EQ(engine.shapeLine(line, regular), TextStatus::Ok);
  std::array<NativeStyleSpan, 1> spans{{{0, static_cast<uint32_t>(text.size()), 1, 0}}};
  line.spans = spans;
  ASSERT_EQ(engine.shapeLine(line, bold), TextStatus::Ok);
  spans[0].style = 2;
  ASSERT_EQ(engine.shapeLine(line, italic), TextStatus::Ok);
  ASSERT_EQ(regular.glyphs.size(), bold.glyphs.size());
  EXPECT_GT(bold.advance26, regular.advance26);
  EXPECT_EQ(italic.advance26, regular.advance26);
  bool markSeen = false;
  for (size_t i = 0; i < regular.glyphs.size(); ++i) {
    if (!regular.glyphs[i].advanceX26) {
      markSeen = true;
      EXPECT_EQ(bold.glyphs[i].advanceX26, 0);
    }
  }
  EXPECT_TRUE(markSeen);
  for (const auto& glyph : italic.glyphs.span()) {
    NativeBitmapView bitmap;
    ASSERT_EQ(engine.rasterize(glyph, bitmap), TextStatus::Ok);
    EXPECT_GE(glyph.x26 + bitmap.left * 64, italic.ink.left26);
    EXPECT_LE(glyph.x26 + (bitmap.left + bitmap.width) * 64, italic.ink.right26);
    EXPECT_GE(glyph.y26 - bitmap.top * 64, italic.ink.top26);
    EXPECT_LE(glyph.y26 + (bitmap.height - bitmap.top) * 64, italic.ink.bottom26);
  }
}

TEST_F(NativeTextEngineTest, SuperscriptBaselineAndRasterSizeAreAppliedOnce) {
  const std::string text = "กี่";
  NativeGlyphRun regular, superscript;
  ASSERT_EQ(engine.shapeLine(input(text), regular), TextStatus::Ok);
  auto line = input(text);
  std::array<NativeStyleSpan, 1> spans{{{0, static_cast<uint32_t>(text.size()), 16, 0}}};
  line.spans = spans;
  ASSERT_EQ(engine.shapeLine(line, superscript), TextStatus::Ok);
  ASSERT_FALSE(superscript.glyphs.empty());
  EXPECT_EQ(superscript.glyphs[0].rasterSize26 * 2, regular.glyphs[0].rasterSize26);
  EXPECT_LT(superscript.glyphs[0].y26, regular.glyphs[0].y26);
  EXPECT_LT(superscript.advance26, regular.advance26);
  EXPECT_GE(superscript.ascender26, -superscript.ink.top26);
}

TEST_F(NativeTextEngineTest, RasterCacheAndEvictionPreserveCoverage) {
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input("กี่"), run), TextStatus::Ok);
  ASSERT_FALSE(run.glyphs.empty());
  const auto glyph = run.glyphs[0];
  NativeBitmapView bitmap;
  ASSERT_EQ(engine.rasterize(glyph, bitmap), TextStatus::Ok);
  ASSERT_GT(bitmap.width * bitmap.height, 0);
  const int width = bitmap.width, height = bitmap.height, left = bitmap.left, top = bitmap.top;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
  for (int y = 0; y < height; ++y)
    std::memcpy(pixels.data() + static_cast<size_t>(y) * width, bitmap.data + y * bitmap.pitch, width);
  ASSERT_EQ(engine.rasterize(glyph, bitmap), TextStatus::Ok);
  engine.clearCaches();
  ASSERT_EQ(engine.rasterize(glyph, bitmap), TextStatus::Ok);
  ASSERT_EQ(bitmap.width, width);
  ASSERT_EQ(bitmap.height, height);
  EXPECT_EQ(bitmap.left, left);
  EXPECT_EQ(bitmap.top, top);
  for (int y = 0; y < height; ++y)
    EXPECT_EQ(std::memcmp(pixels.data() + static_cast<size_t>(y) * width, bitmap.data + y * bitmap.pitch, width), 0);
}

TEST_F(NativeTextEngineTest, InvalidUtf8AndInvalidSpansDiscardPreviouslySuccessfulGeometry) {
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input("ภาษาไทย"), run), TextStatus::Ok);
  for (const std::string malformed :
       {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xe0\xb8", 2)}) {
    EXPECT_EQ(engine.shapeLine(input(malformed), run), TextStatus::InvalidText);
    EXPECT_TRUE(run.glyphs.empty());
    EXPECT_TRUE(run.clusters.empty());
    EXPECT_EQ(run.advance26, 0);
  }
  std::array<NativeStyleSpan, 1> spans{{{1, 3, 0, 0}}};
  auto line = input("ก");
  line.spans = spans;
  EXPECT_EQ(engine.shapeLine(line, run), TextStatus::InvalidText);
  EXPECT_TRUE(run.glyphs.empty());
}

TEST_F(NativeTextEngineTest, EmptyInputSucceedsWithoutFontAndOversizedWindowsFail) {
  NativeGlyphRun run;
  EXPECT_EQ(engine.shapeLine(input("", 0), run), TextStatus::Ok);
  EXPECT_EQ(run.advance26, 0);
  EXPECT_EQ(run.ascender26, 0);
  EXPECT_EQ(run.descender26, 0);
  const std::string oversized(4097, 'a');
  EXPECT_EQ(engine.shapeLine(input(oversized), run), TextStatus::CapacityExceeded);
  EXPECT_TRUE(run.glyphs.empty());
  const std::string maximum(4096, 'a');
  auto line = input(maximum);
  line.syntheticSuffixCp = '-';
  ASSERT_EQ(engine.shapeLine(line, run), TextStatus::Ok);
  ASSERT_FALSE(run.glyphs.empty());
  EXPECT_EQ(run.glyphs[run.glyphs.size() - 1].endByte, maximum.size());
}

TEST_F(NativeTextEngineTest, AllocationFailureCannotReturnSuccessfulEmptyTextAndEngineRecovers) {
  NativeGlyphRun run;
  ASSERT_EQ(engine.shapeLine(input("กี่"), run), TextStatus::Ok);
  engine.clearCaches();
  run.glyphs.reset();
  run.clusters.reset();
  native_text::failAllocationsAfter(0);
  EXPECT_EQ(engine.shapeLine(input("ภาษาไทย"), run), TextStatus::OutOfMemory);
  EXPECT_EQ(engine.lastStatus(), TextStatus::OutOfMemory);
  EXPECT_TRUE(run.glyphs.empty());
  EXPECT_TRUE(run.clusters.empty());
  EXPECT_EQ(run.advance26, 0);
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  ASSERT_EQ(engine.shapeLine(input("ภาษาไทย"), run), TextStatus::Ok);
  EXPECT_GT(run.advance26, 0);
}

TEST_F(NativeTextEngineTest, DictionaryAndFontFingerprintsRemainStableAcrossCacheRelease) {
  const auto before = engine.layoutFingerprint(NOTOSANS_14_FONT_ID);
  EXPECT_NE(before, 0u);
  EXPECT_NE(before, engine.layoutFingerprint(NOTOSERIF_14_FONT_ID));
  engine.clearCaches();
  EXPECT_EQ(engine.layoutFingerprint(NOTOSANS_14_FONT_ID), before);
  std::array<uint32_t, 16> breaks{};
  size_t count = 0;
  ASSERT_EQ(engine.findThaiBreaks("ภาษาไทย", breaks, count), TextStatus::Ok);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(breaks[0], std::string("ภาษา").size());
}

TEST_F(NativeTextEngineTest, NominalMetricsKeepTheFreeTypeLineGapSeparateFromEmptyGeometry) {
  ReferenceFace reference;
  ASSERT_TRUE(reference.openAsset("NotoSans-Regular"));
  int32_t height = 0, ascent = 0, descent = 0;
  ASSERT_EQ(engine.fontMetrics(NOTOSANS_14_FONT_ID, 0, height, ascent, descent), TextStatus::Ok);
  EXPECT_EQ(height, reference.face->size->metrics.height);
  EXPECT_EQ(ascent, reference.face->size->metrics.ascender);
  EXPECT_EQ(descent, -reference.face->size->metrics.descender);
  NativeGlyphRun empty;
  ASSERT_EQ(engine.shapeLine(input(""), empty), TextStatus::Ok);
  EXPECT_EQ(empty.ascender26, 0);
  EXPECT_EQ(empty.descender26, 0);
  EXPECT_EQ(engine.fontMetrics(0, 0, height, ascent, descent), TextStatus::InvalidFont);
  EXPECT_EQ(height, 0);
  EXPECT_EQ(ascent, 0);
  EXPECT_EQ(descent, 0);
}
}  // namespace
