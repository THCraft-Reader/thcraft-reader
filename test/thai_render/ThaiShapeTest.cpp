#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <ThaiCluster.h>
#include <ThaiShape.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <utility>
#include <vector>

namespace {
void put16(std::vector<uint8_t>& b, size_t p, uint16_t n) {
  b[p] = n;
  b[p + 1] = n >> 8;
}
void put32(std::vector<uint8_t>& b, size_t p, uint32_t n) {
  put16(b, p, n);
  put16(b, p + 2, n >> 16);
}
bool covered(void*, uint32_t cp) { return (cp >= 0xE01 && cp <= 0xE2E) || cp == 0xE001; }
std::vector<uint8_t> payload() {
  const uint32_t base = 28 + ThaiShapeView::DENSE_COUNT * 4;
  const uint32_t offsets = base + ThaiShapeView::DENSE_COUNT * 8;
  const uint32_t data = offsets + 8;
  std::vector<uint8_t> b(data + 9);
  put16(b, 0, ThaiShapeView::DENSE_COUNT);
  put16(b, 2, 1);
  put32(b, 4, 28);
  put32(b, 8, base);
  put32(b, 12, offsets);
  put32(b, 16, data);
  put16(b, 20, 30);
  put16(b, 22, -10);
  put16(b, 24, 40);
  for (uint32_t i = 0; i < ThaiShapeView::DENSE_COUNT; ++i) {
    put16(b, 28 + i * 4, i);
    put16(b, base + i * 8, 0xE01 + i / 82);
    put16(b, base + i * 8 + 2, 100 + i % 82);
  }
  put32(b, offsets, data);
  put32(b, offsets + 4, b.size());
  b[data] = 1;
  put16(b, data + 1, 0xE001);
  put16(b, data + 5, -13);
  put16(b, data + 7, 27);
  return b;
}
std::vector<ThaiGlyphPlacement> placements(ThaiGlyphCursor& cursor) {
  std::vector<ThaiGlyphPlacement> result;
  ThaiGlyphPlacement p;
  while (cursor.next(p)) result.push_back(p);
  return result;
}
}  // namespace

TEST(ThaiShapeTest, SelectsExactNormalStackedAmAndDescenderKeys) {
  auto bytes = payload();
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(bytes.data(), bytes.size(), covered, nullptr));
  struct Case {
    const char* text;
    uint32_t base;
    uint16_t variant;
  };
  for (const auto& c : {Case{"ก", 0xE01, 0},
                        {"กี่", 0xE01, 22},
                        {"น้ำ", 0xE19, 79},
                        {"นํ้า", 0xE19, 79},
                        {"ญู", 0xE0D, 49},
                        {"ฐุ", 0xE10, 42}}) {
    SCOPED_TRACE(c.text);
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(c.text, shape));
    const auto output = placements(cursor);
    ASSERT_EQ(2u, output.size());
    EXPECT_EQ(c.base, output[0].codepoint);
    EXPECT_EQ(100 + c.variant, output[0].advanceFP);
    EXPECT_EQ(ThaiGlyphPlacement::ClusterStart | ThaiGlyphPlacement::RecipeStart, output[0].flags);
    EXPECT_EQ(0xE001u, output[1].codepoint);
    EXPECT_EQ(-13, output[1].xOffsetFP);
    EXPECT_EQ(27, output[1].yOffsetFP);
    EXPECT_EQ(ThaiGlyphPlacement::ClusterEnd | ThaiGlyphPlacement::RecipeEnd, output[1].flags);
    EXPECT_EQ(std::string_view(c.text).size(), cursor.consumedBytes());
    EXPECT_EQ(0u, output[0].sourceBegin);
    EXPECT_EQ(std::string_view(c.text).size(), output[0].sourceEnd);
    EXPECT_EQ(output[0].sourceBegin, output[1].sourceBegin);
    EXPECT_EQ(output[0].sourceEnd, output[1].sourceEnd);
  }
}

TEST(ThaiShapeTest, OuterClusterIncludesNativeLeadingVowelAndPreservesFollowingText) {
  auto bytes = payload();
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(bytes.data(), bytes.size(), covered, nullptr));
  ThaiGlyphCursor cursor;
  ASSERT_TRUE(cursor.begin("เก่งX", shape));
  const auto output = placements(cursor);
  ASSERT_EQ(3u, output.size());
  EXPECT_EQ(0xE40u, output[0].codepoint);
  EXPECT_EQ(ThaiGlyphPlacement::Native | ThaiGlyphPlacement::ClusterStart, output[0].flags);
  EXPECT_EQ(0u, output[1].flags & ThaiGlyphPlacement::ClusterStart);
  EXPECT_EQ(std::string_view("เก่").size(), cursor.consumedBytes());
  ASSERT_TRUE(cursor.begin("งX", shape));
  EXPECT_EQ(3u, cursor.consumedBytes());
}

TEST(ThaiShapeTest, UnsupportedClusterEmitsNothingIncludingItsSupportedPrefix) {
  auto bytes = payload();
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(bytes.data(), bytes.size(), covered, nullptr));
  for (const char* text : {"ก่ี่ข", "กีีข", "กํุ่ข"}) {
    ThaiGlyphCursor cursor;
    thai::Cluster cluster{};
    size_t expected = 0;
    ASSERT_TRUE(thai::nextCluster(text, expected, cluster, true));
    EXPECT_FALSE(cursor.begin(text, shape));
    EXPECT_EQ(expected, cursor.consumedBytes());
    ThaiGlyphPlacement p;
    EXPECT_FALSE(cursor.next(p));
  }
  // A missing later recipe rejects the entire TCC, not merely that recipe.
  const uint32_t key = (0xE2D - 0xE01) * 82;
  put16(bytes, 28 + key * 4, 0xFFFF);
  put16(bytes, 30 + key * 4, 0xFFFF);
  ASSERT_TRUE(shape.validate(bytes.data(), bytes.size(), covered, nullptr));
  ThaiGlyphCursor cursor;
  EXPECT_FALSE(cursor.begin("เรื่อง", shape));
  EXPECT_EQ(std::string_view("เรื่อ").size(), cursor.consumedBytes());
  ThaiGlyphPlacement placement;
  EXPECT_FALSE(cursor.next(placement));
  EXPECT_FALSE(cursor.begin("A", shape));
  EXPECT_EQ(0u, cursor.consumedBytes());
  ThaiShapeView absent;
  EXPECT_FALSE(cursor.begin("กี่", absent));
  EXPECT_EQ(0u, cursor.consumedBytes());
}

TEST(ThaiShapeTest, SpacingUnitsCarryOriginalSpansThroughNativeAndReorderedGlyphs) {
  auto bytes = payload();
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(bytes.data(), bytes.size(), covered, nullptr));
  struct Case {
    std::string_view text;
    std::vector<std::string_view> units;
  };
  for (const auto& c : {Case{"เพื่อ", {"เ", "พื่", "อ"}}, Case{"ภา", {"ภ", "า"}}, Case{"ไท", {"ไ", "ท"}},
                        Case{"เรื่อ", {"เ", "รื่", "อ"}}, Case{"เก่", {"เ", "ก่"}}, Case{"น้ำ", {"น้ำ"}}, Case{"นํ้า", {"นํ้า"}}}) {
    SCOPED_TRACE(c.text);
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(c.text, shape));
    EXPECT_EQ(c.text.size(), cursor.consumedBytes());
    const auto output = placements(cursor);
    size_t index = 0;
    size_t source = 0;
    std::vector<size_t> renderedBoundaries;
    for (const auto unit : c.units) {
      ASSERT_LT(index, output.size());
      EXPECT_EQ(unit, c.text.substr(source, unit.size()));
      if (source) renderedBoundaries.push_back(source);
      const size_t end = source + unit.size();
      do {
        ASSERT_LT(index, output.size());
        const auto& placement = output[index++];
        EXPECT_EQ(source, placement.sourceBegin);
        EXPECT_EQ(end, placement.sourceEnd);
        if (placement.flags & (ThaiGlyphPlacement::Native | ThaiGlyphPlacement::RecipeEnd)) break;
      } while (true);
      source = end;
    }
    EXPECT_EQ(index, output.size());
    EXPECT_EQ(source, c.text.size());
    thai::JustificationBoundaryCursor boundaries(c.text);
    size_t offset;
    std::vector<size_t> spacingBoundaries;
    while (boundaries.next(offset)) spacingBoundaries.push_back(offset);
    EXPECT_EQ(renderedBoundaries, spacingBoundaries);
  }
}

TEST(ThaiShapeTest, RejectsUnsafeTablesAndUncoveredRecipeGlyphs) {
  const auto valid = payload();
  for (size_t size : {size_t{0}, size_t{27}, valid.size() - 1}) {
    ThaiShapeView shape;
    EXPECT_FALSE(shape.validate(valid.data(), size, covered, nullptr));
  }
  for (const auto [offset, value] :
       {std::pair<size_t, uint32_t>{4, UINT32_MAX}, {8, 28}, {12, UINT32_MAX}, {16, UINT32_MAX}}) {
    auto b = valid;
    put32(b, offset, value);
    ThaiShapeView shape;
    EXPECT_FALSE(shape.validate(b.data(), b.size(), covered, nullptr));
  }
  auto b = valid;
  put16(b, 28, 0xFFFF);
  ThaiShapeView shape;
  EXPECT_FALSE(shape.validate(b.data(), b.size(), covered, nullptr));
  b = valid;
  put16(b, 28 + ThaiShapeView::DENSE_COUNT * 4, 0xFFFF);
  EXPECT_FALSE(shape.validate(b.data(), b.size(), covered, nullptr));
  b = valid;
  b[b.size() - 9] = 6;
  EXPECT_FALSE(shape.validate(b.data(), b.size(), covered, nullptr));
}

TEST(ThaiShapeTest, PreparedFontRecipesPrewarmAlternatesAndSurviveCacheEviction) {
  const char* path = std::getenv("THAI_SHAPE_TEST_FONT");
  if (!path) GTEST_SKIP() << "Set THAI_SHAPE_TEST_FONT to a prepared shaped NotoSansThai cpfont";
  SdCardFont font;
  ASSERT_TRUE(font.load(path));
  const ThaiShapeView* shape = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, shape);
  EpdFontFamily family(font.getEpdFont());
  EXPECT_EQ(shape, family.getThaiShape(EpdFontFamily::BOLD_ITALIC));
  const char* text = "กี่ น้ำ ญู ฐุ ปี่";
  ASSERT_EQ(0, font.buildAdvanceTable(text, 1));
  ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
  bool alternate = false;
  for (const char* cluster : {"กี่", "น้ำ", "นํ้า", "ญู", "ฐุ", "ปี่"}) {
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(cluster, *shape));
    const auto output = placements(cursor);
    const auto before = probe::readCalls;
    for (const auto& p : output) {
      const EpdGlyph* glyph = font.getEpdFont()->getGlyph(p.codepoint);
      ASSERT_NE(nullptr, glyph);
      EXPECT_FALSE(font.isOverflowGlyph(glyph));
      EXPECT_TRUE(font.getEpdFont()->hasCodepoint(p.codepoint));
      if (p.codepoint >= 0xE000 && p.codepoint <= 0xF8FF) alternate = true;
    }
    EXPECT_EQ(before, probe::readCalls);
  }
  EXPECT_TRUE(alternate);
  const auto identity = font.contentHash();
  font.clearCache();
  font.releaseResidentCaches();
  EXPECT_EQ(shape, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(identity, font.contentHash());
  ThaiGlyphCursor cursor;
  ASSERT_TRUE(cursor.begin("กี่", *shape));
  EXPECT_EQ(std::string_view("กี่").size(), cursor.consumedBytes());
}

TEST(ThaiShapeTest, LayoutPreparationDoesNotChangeResidentPageKerning) {
  const char* path = std::getenv("THAI_SHAPE_TEST_FONT");
  ASSERT_NE(nullptr, path);
  SdCardFont font;
  ASSERT_TRUE(font.load(path));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
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
