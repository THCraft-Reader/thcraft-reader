#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <ThaiCluster.h>
#include <ThaiShape.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "HostAllocation.h"

namespace {
struct Metrics {
  std::array<ThaiGlyphMetrics, ThaiShapeView::GLYPH_COUNT> glyphs{};
  uint64_t coverage = 0;
  ThaiShapeView view;
  Metrics() {
    for (uint32_t cp = 0xE01; cp <= 0xE2E; ++cp) set(cp, {16, 19, 320, 2, 19});
    set(0xE1B, {16, 26, 320, 2, 26});
    set(0xE0D, {26, 25, 480, 2, 19});
    set(0xE10, {16, 27, 320, 2, 20});
    for (uint32_t cp : {0xE31, 0xE34, 0xE35, 0xE36, 0xE37, 0xE47}) set(cp, {16, 6, 0, -18, 27});
    for (uint32_t cp : {0xE38, 0xE39, 0xE3A}) set(cp, {8, 7, 0, -10, -2});
    for (uint32_t cp = 0xE48; cp <= 0xE4E; ++cp) set(cp, {3, 8, 0, -6, 29});
    set(0xE4D, {8, 7, 0, -9, 27});
    set(0xE32, {11, 19, 216, 0, 19});
  }
  void set(uint32_t cp, ThaiGlyphMetrics metrics) {
    const int index = ThaiShapeView::glyphIndex(cp);
    glyphs[index] = metrics;
    coverage |= uint64_t{1} << index;
    view.setMetrics(glyphs.data(), coverage);
  }
  void omit(uint32_t cp) {
    coverage &= ~(uint64_t{1} << ThaiShapeView::glyphIndex(cp));
    view.setMetrics(glyphs.data(), coverage);
  }
};
std::vector<ThaiGlyphPlacement> placements(ThaiGlyphCursor& cursor) {
  std::vector<ThaiGlyphPlacement> output;
  ThaiGlyphPlacement glyph;
  while (cursor.next(glyph)) output.push_back(glyph);
  return output;
}
int top(const ThaiShapeView& shape, const ThaiGlyphPlacement& p) {
  return shape.glyph(p.codepoint)->top * 16 + p.yOffsetFP;
}
int bottom(const ThaiShapeView& shape, const ThaiGlyphPlacement& p) {
  return top(shape, p) - shape.glyph(p.codepoint)->height * 16;
}
}  // namespace

TEST(ThaiShapeTest, StackedMarksClearOneAnotherWithoutAdvancing) {
  Metrics metrics;
  ThaiGlyphCursor cursor;
  ASSERT_TRUE(cursor.begin("กี่", metrics.view));
  const auto glyphs = placements(cursor);
  ASSERT_EQ(3u, glyphs.size());
  EXPECT_EQ(0xE01u, glyphs[0].codepoint);
  EXPECT_EQ(0xE35u, glyphs[1].codepoint);
  EXPECT_EQ(0xE48u, glyphs[2].codepoint);
  EXPECT_GE(bottom(metrics.view, glyphs[1]), top(metrics.view, glyphs[0]) + 16);
  EXPECT_GE(bottom(metrics.view, glyphs[2]), top(metrics.view, glyphs[1]) + 16);
  EXPECT_EQ(320, glyphs[0].advanceFP);
  EXPECT_EQ(0, glyphs[1].advanceFP);
  EXPECT_EQ(0, glyphs[2].advanceFP);
  EXPECT_EQ(0, glyphs[1].xOffsetFP);
  EXPECT_EQ(0, glyphs[2].xOffsetFP);
}

TEST(ThaiShapeTest, RightAscendersMoveUpperMarksAwayFromStemInsteadOfAboveIt) {
  Metrics metrics;
  metrics.set(0xE1D, {16, 26, 320, 2, 26});
  metrics.set(0xE1F, {16, 26, 320, 2, 26});
  for (const char* text : {"ปี่", "ฝี่", "ฟี่"}) {
    SCOPED_TRACE(text);
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(text, metrics.view));
    const auto glyphs = placements(cursor);
    ASSERT_EQ(3u, glyphs.size());
    EXPECT_LT(glyphs[1].xOffsetFP, 0);
    EXPECT_LT(glyphs[2].xOffsetFP, 0);
    EXPECT_EQ(0, glyphs[1].yOffsetFP);
    EXPECT_GE(bottom(metrics.view, glyphs[2]), top(metrics.view, glyphs[1]) + 16);
  }
}

TEST(ThaiShapeTest, SlopedAndLeftTailsKeepNativeHorizontalPlacementForUpperVowels) {
  struct Base {
    const char* text;
    uint32_t codepoint;
  };
  for (const auto& base : {Base{"ส", 0xE2A}, Base{"ช", 0xE0A}, Base{"ฬ", 0xE2C}}) {
    Metrics metrics;
    metrics.set(base.codepoint, {16, 26, 320, 2, 26});
    for (const char* vowel : {"ั", "ิ", "ี", "ึ", "ื"}) {
      for (const char* tone : {"", "่"}) {
        const std::string text = std::string(base.text) + vowel + tone;
        SCOPED_TRACE(text);
        ThaiGlyphCursor cursor;
        ASSERT_TRUE(cursor.begin(text, metrics.view));
        const auto glyphs = placements(cursor);
        ASSERT_EQ(*tone ? 3u : 2u, glyphs.size());
        for (const auto& glyph : glyphs) EXPECT_EQ(0, glyph.xOffsetFP);
      }
    }
  }
}

TEST(ThaiShapeTest, BelowMarksClearTheActualDescender) {
  Metrics metrics;
  for (const char* text : {"ญู", "ฐุ"}) {
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(text, metrics.view));
    const auto glyphs = placements(cursor);
    ASSERT_EQ(2u, glyphs.size());
    EXPECT_LE(top(metrics.view, glyphs[1]), bottom(metrics.view, glyphs[0]) - 16);
    EXPECT_LT(glyphs[1].yOffsetFP, 0);
    EXPECT_EQ(0, glyphs[1].advanceFP);
  }
}

TEST(ThaiShapeTest, SaraAmSpellingsShareVisualOrderAndAtomicSourceSpan) {
  Metrics metrics;
  ThaiGlyphCursor composed, decomposed;
  ASSERT_TRUE(composed.begin("น้ำ", metrics.view));
  ASSERT_TRUE(decomposed.begin("นํ้า", metrics.view));
  const auto first = placements(composed), second = placements(decomposed);
  ASSERT_EQ(4u, first.size());
  ASSERT_EQ(first.size(), second.size());
  const uint32_t expected[] = {0xE19, 0xE4D, 0xE49, 0xE32};
  for (size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(expected[i], first[i].codepoint);
    EXPECT_EQ(first[i].codepoint, second[i].codepoint);
    EXPECT_EQ(first[i].advanceFP, second[i].advanceFP);
    EXPECT_EQ(first[i].xOffsetFP, second[i].xOffsetFP);
    EXPECT_EQ(first[i].yOffsetFP, second[i].yOffsetFP);
    EXPECT_EQ(0u, first[i].sourceBegin);
    EXPECT_EQ(std::string_view("น้ำ").size(), first[i].sourceEnd);
    EXPECT_EQ(0u, second[i].sourceBegin);
    EXPECT_EQ(std::string_view("นํ้า").size(), second[i].sourceEnd);
  }
  EXPECT_EQ(216, first.back().advanceFP);
  EXPECT_TRUE(first.front().flags & ThaiGlyphPlacement::RecipeStart);
  EXPECT_TRUE(first.back().flags & ThaiGlyphPlacement::RecipeEnd);
}

TEST(ThaiShapeTest, MissingMarkRejectsWholeOuterClusterWithoutPrefixPlacements) {
  Metrics metrics;
  metrics.omit(0xE48);
  ThaiGlyphCursor cursor;
  ASSERT_FALSE(cursor.begin("เก่งX", metrics.view));
  thai::Cluster cluster{};
  size_t end = 0;
  ASSERT_TRUE(thai::nextCluster("เก่งX", end, cluster, true));
  EXPECT_EQ(end, cursor.consumedBytes());
  ThaiGlyphPlacement glyph;
  EXPECT_FALSE(cursor.next(glyph));
}

TEST(ThaiShapeTest, MissingSaraAmComponentKeepsSourceNative) {
  Metrics metrics;
  metrics.omit(0xE32);
  ThaiGlyphCursor cursor;
  EXPECT_FALSE(cursor.begin("น้ำ", metrics.view));
  EXPECT_EQ(std::string_view("น้ำ").size(), cursor.consumedBytes());
  ThaiGlyphPlacement glyph;
  EXPECT_FALSE(cursor.next(glyph));
}

TEST(ThaiShapeTest, MalformedDependentRunsDoNotExposePartialShaping) {
  Metrics metrics;
  for (const char* text : {"่ก", "ก่ี่ข", "กีีข", "กํุ่ข"}) {
    ThaiGlyphCursor cursor;
    EXPECT_FALSE(cursor.begin(text, metrics.view)) << text;
    ThaiGlyphPlacement glyph;
    EXPECT_FALSE(cursor.next(glyph));
    thai::Cluster cluster{};
    size_t end = 0;
    ASSERT_TRUE(thai::nextCluster(text, end, cluster, true));
    EXPECT_EQ(end, cursor.consumedBytes());
  }
}

TEST(ThaiShapeTest, CursorCopyAndIndependentCursorsHaveNoSharedLeasesOrAllocations) {
  Metrics metrics;
  ThaiGlyphCursor first, second, third;
  const auto allocations = probe::allocationCalls();
  const bool admitted =
      first.begin("เก่งX", metrics.view) && second.begin("น้ำ", metrics.view) && third.begin("ญู", metrics.view);
  ThaiGlyphPlacement glyph;
  const bool stepped = first.next(glyph);
  auto copy = first;
  std::array<ThaiGlyphPlacement, 8> a{}, b{};
  size_t na = 0, nb = 0;
  while (first.next(glyph)) a[na++] = glyph;
  while (copy.next(glyph)) b[nb++] = glyph;
  const auto after = probe::allocationCalls();
  EXPECT_TRUE(admitted);
  EXPECT_TRUE(stepped);
  EXPECT_EQ(allocations, after);
  ASSERT_EQ(na, nb);
  for (size_t i = 0; i < na; ++i) {
    EXPECT_EQ(a[i].codepoint, b[i].codepoint);
    EXPECT_EQ(a[i].sourceBegin, b[i].sourceBegin);
    EXPECT_EQ(a[i].sourceEnd, b[i].sourceEnd);
    EXPECT_EQ(a[i].flags, b[i].flags);
  }
}

TEST(ThaiShapeTest, UnpositionedTextRemainsOnNativeLigaturePath) {
  Metrics metrics;
  ThaiGlyphCursor cursor;
  EXPECT_FALSE(cursor.begin("ก", metrics.view));
  EXPECT_EQ(3u, cursor.consumedBytes());
  EXPECT_FALSE(cursor.begin("Latin", metrics.view));
  EXPECT_EQ(0u, cursor.consumedBytes());
}

TEST(ThaiShapeTest, NativeCpfontPrewarmIncludesDecomposedGlyphsAndSnapshotSurvivesEviction) {
  const char* path = std::getenv("THAI_SHAPE_TEST_FONT");
  ASSERT_NE(nullptr, path);
  SdCardFont font;
  ASSERT_TRUE(font.load(path));
  const auto* shape = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, shape);
  EpdFontFamily family(16, font.getEpdFont());
  EXPECT_EQ(shape, family.getThaiShape(EpdFontFamily::BOLD_ITALIC));
  ASSERT_EQ(0, font.prewarm("กี่ น้ำ ญู ฐุ ปี่", 1, false, true, false));
  for (const char* text : {"กี่", "น้ำ", "นํ้า", "ญู", "ฐุ", "ปี่"}) {
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(text, *shape));
    const auto output = placements(cursor);
    const auto before = probe::readCalls;
    for (const auto& p : output) {
      EXPECT_LT(p.codepoint, 0xE000u);
      ASSERT_TRUE(font.getEpdFont()->hasCodepoint(p.codepoint));
      const auto* glyph = font.getEpdFont()->getGlyph(p.codepoint);
      ASSERT_NE(nullptr, glyph);
      EXPECT_FALSE(font.isOverflowGlyph(glyph));
    }
    EXPECT_EQ(before, probe::readCalls);
  }
  const auto identity = font.contentHash();
  font.releaseResidentCaches();
  EXPECT_EQ(identity, font.contentHash());
  EXPECT_EQ(shape, font.getEpdFont()->getThaiShape());
  ThaiGlyphCursor cursor;
  const auto reads = probe::readCalls, allocations = probe::allocationCalls();
  const bool admitted = cursor.begin("กี่", *shape);
  ThaiGlyphPlacement glyph;
  size_t count = 0;
  while (cursor.next(glyph)) ++count;
  EXPECT_TRUE(admitted);
  EXPECT_EQ(3u, count);
  EXPECT_EQ(reads, probe::readCalls);
  EXPECT_EQ(allocations, probe::allocationCalls());
}

TEST(ThaiShapeTest, LayoutPreparationDoesNotChangeResidentPageKerning) {
  const char* path = std::getenv("THAI_SHAPE_TEST_FONT");
  ASSERT_NE(nullptr, path);
  SdCardFont font;
  ASSERT_TRUE(font.load(path));
  ASSERT_EQ(0, font.prewarm("AV To", 1, false, true, false));
  const auto kern = font.getEpdFont()->getKerning('A', 'V');
  ASSERT_LT(kern, 0);
  int width, height;
  font.getEpdFont()->getTextDimensions("AV", &width, &height);
  ASSERT_EQ(0, font.buildAdvanceTable("To", 1));
  ASSERT_EQ(0, font.prewarm("AV To", 1, false, true, false));
  EXPECT_EQ(kern, font.getEpdFont()->getKerning('A', 'V'));
  int afterWidth, afterHeight;
  font.getEpdFont()->getTextDimensions("AV", &afterWidth, &afterHeight);
  EXPECT_EQ(width, afterWidth);
  EXPECT_EQ(height, afterHeight);
}

TEST(ThaiShapeTest, NativeLigaturesTakePriorityOnlyForTheirMatchingSourcePair) {
  Metrics metrics;
  EpdFontData font{};
  const EpdLigaturePair pair{(0xE01u << 16) | 0xE35u, 'C'};
  font.ligaturePairs = &pair;
  font.ligaturePairCount = 1;
  metrics.view.setNativeFont(font);
  ThaiGlyphCursor cursor;
  EXPECT_FALSE(cursor.begin("กี่", metrics.view));
  EXPECT_EQ(std::string_view("กี่").size(), cursor.consumedBytes());
  ThaiGlyphPlacement glyph;
  EXPECT_FALSE(cursor.next(glyph));
  ASSERT_TRUE(cursor.begin("กี่", metrics.view, false));
  const auto smallCaps = placements(cursor);
  ASSERT_EQ(3u, smallCaps.size());
  EXPECT_EQ(0xE35u, smallCaps[1].codepoint);
  EXPECT_EQ(0xE48u, smallCaps[2].codepoint);
  EXPECT_GT(smallCaps[2].yOffsetFP, smallCaps[1].yOffsetFP);
  EXPECT_TRUE(cursor.begin("กิ่", metrics.view));
  EXPECT_TRUE(cursor.next(glyph));
  EXPECT_EQ(0xE01u, glyph.codepoint);
}
