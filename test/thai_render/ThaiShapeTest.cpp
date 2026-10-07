#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <ThaiCluster.h>
#include <ThaiShape.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "HostAllocation.h"

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
struct MemoryIo {
  const std::vector<uint8_t>& bytes;
  size_t calls = 0;
  size_t requested = 0;
  size_t failCall = 0;
  uint32_t lastOffset = 0;
  size_t lastCount = 0;
  static bool read(void* context, uint32_t offset, uint8_t* output, size_t count) {
    auto& io = *static_cast<MemoryIo*>(context);
    ++io.calls;
    io.requested += count;
    io.lastOffset = offset;
    io.lastCount = count;
    if ((io.failCall && io.calls >= io.failCall) || offset > io.bytes.size() || count > io.bytes.size() - offset)
      return false;
    std::memcpy(output, io.bytes.data() + offset, count);
    return true;
  }
};

void expectPlacements(const std::vector<ThaiGlyphPlacement>& expected, const std::vector<ThaiGlyphPlacement>& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(expected[i].codepoint, actual[i].codepoint);
    EXPECT_EQ(expected[i].advanceFP, actual[i].advanceFP);
    EXPECT_EQ(expected[i].xOffsetFP, actual[i].xOffsetFP);
    EXPECT_EQ(expected[i].yOffsetFP, actual[i].yOffsetFP);
    EXPECT_EQ(expected[i].flags, actual[i].flags);
    EXPECT_EQ(expected[i].sourceBegin, actual[i].sourceBegin);
    EXPECT_EQ(expected[i].sourceEnd, actual[i].sourceEnd);
  }
}

void appendThai(std::string& text, uint32_t cp) {
  text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
  text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
  text.push_back(static_cast<char>(0x80 | (cp & 63)));
}

std::string keyText(uint32_t key) {
  std::string text;
  appendThai(text, 0xE01 + key / 82);
  const uint32_t variant = key % 82;
  if (variant >= 77) {
    if (variant != 77) appendThai(text, 0xE48 + variant - 78);
    appendThai(text, 0xE33);
  } else {
    const uint32_t vowel = variant / 7, terminal = variant % 7;
    const uint16_t vowels[] = {0, 0xE31, 0xE34, 0xE35, 0xE36, 0xE37, 0xE38, 0xE39, 0xE3A, 0xE47, 0xE4D};
    if (vowel) appendThai(text, vowels[vowel]);
    if (terminal) appendThai(text, terminal == 6 ? 0xE4E : 0xE47 + terminal);
  }
  return text;
}

std::vector<uint8_t> styledPayload(unsigned style, uint8_t suffixCount) {
  auto bytes = payload();
  const uint32_t base = 28 + ThaiShapeView::DENSE_COUNT * 4;
  const uint32_t offsets = base + ThaiShapeView::DENSE_COUNT * 8;
  const uint32_t data = offsets + 8;
  bytes.resize(data + 1 + suffixCount * 8);
  put16(bytes, 20, 30 + style);
  put16(bytes, 22, -10 - style);
  put16(bytes, 24, 40 + style * 2);
  for (uint32_t i = 0; i < ThaiShapeView::DENSE_COUNT; ++i) {
    put16(bytes, 28 + i * 4, (i + style * 83) % ThaiShapeView::DENSE_COUNT);
    put16(bytes, base + i * 8 + 2, 100 + i % 82 + style * 100);
    put16(bytes, base + i * 8 + 4, -int(style) - 3);
    put16(bytes, base + i * 8 + 6, 7 + style);
  }
  put32(bytes, offsets + 4, bytes.size());
  bytes[data] = suffixCount;
  for (uint8_t i = 0; i < suffixCount; ++i) {
    put16(bytes, data + 1 + i * 8, 0xE001);
    put16(bytes, data + 3 + i * 8, i + style);
    put16(bytes, data + 5 + i * 8, -13 - i);
    put16(bytes, data + 7 + i * 8, 27 + i);
  }
  return bytes;
}

}  // namespace

TEST(ThaiShapeTest, SelectsExactNormalStackedAmAndDescenderKeys) {
  auto bytes = payload();
  ThaiShapeSource source;
  source.setResident(bytes.data(), bytes.size());
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(source, 0, bytes.size(), covered, nullptr));
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
  ThaiShapeSource source;
  source.setResident(bytes.data(), bytes.size());
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(source, 0, bytes.size(), covered, nullptr));
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
  ThaiShapeSource source;
  source.setResident(bytes.data(), bytes.size());
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(source, 0, bytes.size(), covered, nullptr));
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
  ASSERT_TRUE(shape.validate(source, 0, bytes.size(), covered, nullptr));
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
  ThaiShapeSource source;
  source.setResident(bytes.data(), bytes.size());
  ThaiShapeView shape;
  ASSERT_TRUE(shape.validate(source, 0, bytes.size(), covered, nullptr));
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
    ThaiShapeSource source;
    source.setResident(valid.data(), valid.size());
    ThaiShapeView shape;
    EXPECT_FALSE(shape.validate(source, 0, size, covered, nullptr));
  }
  for (const auto [offset, value] :
       {std::pair<size_t, uint32_t>{4, UINT32_MAX}, {8, 28}, {12, UINT32_MAX}, {16, UINT32_MAX}}) {
    auto b = valid;
    put32(b, offset, value);
    ThaiShapeSource source;
    source.setResident(b.data(), b.size());
    ThaiShapeView shape;
    EXPECT_FALSE(shape.validate(source, 0, b.size(), covered, nullptr));
  }
  auto b = valid;
  put16(b, 28, 0xFFFF);
  ThaiShapeSource source;
  source.setResident(b.data(), b.size());
  ThaiShapeView shape;
  EXPECT_FALSE(shape.validate(source, 0, b.size(), covered, nullptr));
  b = valid;
  put16(b, 28 + ThaiShapeView::DENSE_COUNT * 4, 0xFFFF);
  source.setResident(b.data(), b.size());
  EXPECT_FALSE(shape.validate(source, 0, b.size(), covered, nullptr));
  b = valid;
  b[b.size() - 9] = 6;
  source.setResident(b.data(), b.size());
  EXPECT_FALSE(shape.validate(source, 0, b.size(), covered, nullptr));
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

TEST(ThaiShapeTest, SharedCacheSplitsCrossingReadsUsesMruAndClampsFinalBlock) {
  std::vector<uint8_t> bytes(9 * ThaiShapeCache::BLOCK_BYTES + 17);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 17 + i / 512);
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  uint8_t output[41];
  for (size_t count : {size_t{4}, size_t{8}, size_t{41}}) {
    cache.invalidate(source);
    const auto calls = io.calls;
    ASSERT_TRUE(source.readBytes(510, output, count));
    EXPECT_EQ(0, std::memcmp(output, bytes.data() + 510, count));
    EXPECT_EQ(calls + 2, io.calls);
  }
  cache.invalidate(source);
  for (unsigned i = 0; i < 8; ++i) ASSERT_TRUE(source.readBytes(i * 512, output, 1));
  auto calls = io.calls;
  for (unsigned i = 0; i < 8; ++i) ASSERT_TRUE(source.readBytes(i * 512, output, 1));
  EXPECT_EQ(calls, io.calls);
  ASSERT_TRUE(source.readBytes(0, output, 1));  // Protect block zero; block one is now LRU.
  ASSERT_TRUE(source.readBytes(8 * 512, output, 1));
  ASSERT_TRUE(source.readBytes(0, output, 1));
  EXPECT_EQ(calls + 1, io.calls);
  ASSERT_TRUE(source.readBytes(512, output, 1));
  EXPECT_EQ(calls + 2, io.calls);
  EXPECT_EQ(bytes[512], output[0]);
  ASSERT_TRUE(source.readBytes(9 * 512, output, 17));
  EXPECT_EQ(17u, io.lastCount);
  EXPECT_EQ(0, std::memcmp(output, bytes.data() + 9 * 512, 17));
  EXPECT_LE(sizeof(ThaiShapeCache), 14u * 1024u);
}

TEST(ThaiShapeTest, SourceFailureLatchesOnceInvalidatesVictimAndResetsCleanly) {
  std::vector<uint8_t> bytes(1024, 42);
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  unsigned failures = 0;
  source.setFailureCallback(
      [](void* p, uint32_t offset) {
        ++*static_cast<unsigned*>(p);
        EXPECT_EQ(512u, offset);
      },
      &failures);
  uint8_t output[8]{};
  ASSERT_TRUE(source.readBytes(0, output, sizeof(output)));
  io.failCall = io.calls + 1;
  EXPECT_FALSE(source.readBytes(512, output, sizeof(output)));
  const auto calls = io.calls;
  EXPECT_TRUE(source.failed());
  EXPECT_FALSE(source.readBytes(0, output, sizeof(output)));  // Even a warm block is unavailable after a fault.
  EXPECT_FALSE(source.readBytes(512, output, sizeof(output)));
  EXPECT_EQ(calls, io.calls);
  EXPECT_EQ(1u, failures);
  bytes.assign(1024, 99);
  io.failCall = 0;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  ASSERT_TRUE(source.readBytes(0, output, sizeof(output)));
  EXPECT_EQ(99, output[0]);
  EXPECT_FALSE(source.failed());
  EXPECT_FALSE(source.readBytes(UINT32_MAX, output, sizeof(output)));
  EXPECT_TRUE(source.failed());
  EXPECT_EQ(calls + 1, io.calls);  // Invalid ranges never reach the callback.
}

TEST(ThaiShapeTest, EveryDenseKeyMatchesResidentAcrossFourDistinctStylesAndOptionalIndexes) {
  std::vector<uint8_t> family(3, 0);
  uint32_t offsets[4], sizes[4];
  for (unsigned style = 0; style < 4; ++style) {
    auto bytes = styledPayload(style, style == 0 ? 0 : style == 3 ? 5 : 1);
    offsets[style] = family.size();
    sizes[style] = bytes.size();
    family.insert(family.end(), bytes.begin(), bytes.end());
  }
  ThaiShapeCache cache;
  MemoryIo io{family};
  ThaiShapeSource resident, cached;
  resident.setResident(family.data(), family.size());
  cached.setCached(&io, MemoryIo::read, family.size(), cache);
  ThaiShapeView oracle[4], paged[4];
  for (unsigned style = 0; style < 4; ++style) {
    ASSERT_TRUE(oracle[style].validate(resident, offsets[style], sizes[style], covered, nullptr));
    ASSERT_TRUE(paged[style].validate(cached, offsets[style], sizes[style], covered, nullptr));
    EXPECT_EQ(30 + style, paged[style].ascender());
    EXPECT_EQ(-10 - int(style), paged[style].descender());
    EXPECT_EQ(40 + style * 2, paged[style].lineAdvance());
  }
  for (bool indexed : {false, true}) {
    for (unsigned style = 0; style < 4; ++style) {
      paged[style].setDenseIndex(indexed ? family.data() + offsets[style] + 28 : nullptr);
      for (uint32_t key = 0; key < ThaiShapeView::DENSE_COUNT; ++key) {
        SCOPED_TRACE(key);
        SCOPED_TRACE(style);
        SCOPED_TRACE(indexed);
        auto text = keyText(key);
        ThaiGlyphCursor expected, actual;
        ASSERT_TRUE(expected.begin(text, oracle[style]));
        ASSERT_TRUE(actual.begin(text, paged[style]));
        EXPECT_EQ(expected.consumedBytes(), actual.consumedBytes());
        const auto expectedOutput = placements(expected);
        const auto calls = io.calls;
        expectPlacements(expectedOutput, placements(actual));
        EXPECT_EQ(calls, io.calls);
        const auto id = (key + style * 83) % ThaiShapeView::DENSE_COUNT;
        ASSERT_FALSE(expectedOutput.empty());
        EXPECT_EQ(0xE01 + id / 82, expectedOutput[0].codepoint);
        EXPECT_EQ(100 + id % 82 + style * 100, expectedOutput[0].advanceFP);
      }
    }
  }
}

TEST(ThaiShapeTest, AdmissionAndCacheMissesAllocateNothingAndFittingClustersStayWarm) {
  auto bytes = payload();
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  ThaiShapeView view;
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  cache.invalidate(source);
  ThaiGlyphCursor cursor;
  const auto allocations = probe::allocationCalls();
  const bool admitted = cursor.begin("เรื่อ", view);
  ThaiGlyphPlacement placement;
  size_t count = 0;
  while (cursor.next(placement)) ++count;
  const auto after = probe::allocationCalls();
  ASSERT_TRUE(admitted);
  EXPECT_EQ(5u, count);
  EXPECT_EQ(allocations, after);
  const auto calls = io.calls;
  for (unsigned i = 0; i < 20; ++i) {
    ASSERT_TRUE(cursor.begin("เรื่อ", view));
    while (cursor.next(placement)) {
    }
  }
  EXPECT_EQ(calls, io.calls);
  EXPECT_FALSE(cache.hasLeases());
  // Dense RAM lookup must not request its block: only suffix metadata/data and base records remain.
  view.setDenseIndex(bytes.data() + 28);
  cache.invalidate(source);
  ASSERT_TRUE(cursor.begin("ก", view));
  while (cursor.next(placement)) {
  }
  const auto indexedReads = io.calls - calls;
  cache.invalidate(source);
  view.setDenseIndex(nullptr);
  const auto beforePaged = io.calls;
  ASSERT_TRUE(cursor.begin("ก", view));
  while (cursor.next(placement)) {
  }
  EXPECT_EQ(indexedReads + 1, io.calls - beforePaged);
}

TEST(ThaiShapeTest, ResolvedRecipesOutliveBlockEvictionUntilTheirSourceIsInvalidated) {
  auto bytes = payload();
  const uint32_t missing = (0xE2D - 0xE01) * 82;
  put16(bytes, 28 + missing * 4, UINT16_MAX);
  put16(bytes, 30 + missing * 4, UINT16_MAX);
  const auto longRecipe = styledPayload(3, 5);
  ThaiShapeSource resident;
  resident.setResident(bytes.data(), bytes.size());
  ThaiShapeView oracle;
  ASSERT_TRUE(oracle.validate(resident, 0, bytes.size(), covered, nullptr));
  ThaiGlyphCursor expected;
  ASSERT_TRUE(expected.begin("กี่", oracle));
  const auto expectedOutput = placements(expected);

  ThaiShapeCache cache;
  MemoryIo io{bytes}, longIo{longRecipe};
  ThaiShapeSource source, longSource;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  longSource.setCached(&longIo, MemoryIo::read, longRecipe.size(), cache);
  ThaiShapeView view, longView;
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  ASSERT_TRUE(longView.validate(longSource, 0, longRecipe.size(), covered, nullptr));
  // Blocks 40..47 hold none of the dense, base, offset or suffix bytes used below.
  const auto evictBlocks = [&](const ThaiShapeSource& owner) {
    uint8_t byte;
    for (unsigned i = 0; i < ThaiShapeCache::BLOCK_COUNT; ++i) {
      ASSERT_TRUE(owner.readBytes((40 + i) * ThaiShapeCache::BLOCK_BYTES, &byte, 1));
    }
  };
  ThaiGlyphCursor cursor;
  ASSERT_TRUE(cursor.begin("กี่", view));
  expectPlacements(expectedOutput, placements(cursor));
  EXPECT_FALSE(cursor.begin(keyText(missing), view));
  evictBlocks(source);
  auto calls = io.calls;
  ASSERT_TRUE(cursor.begin("กี่", view));
  expectPlacements(expectedOutput, placements(cursor));
  EXPECT_FALSE(cursor.begin(keyText(missing), view));
  EXPECT_FALSE(view.failed());
  EXPECT_EQ(calls, io.calls);

  cache.invalidate(source);
  ASSERT_TRUE(cursor.begin("กี่", view));
  expectPlacements(expectedOutput, placements(cursor));
  EXPECT_LT(calls, io.calls);

  // Six records exceed the retained recipe size: the cluster stays on the block path.
  ASSERT_TRUE(cursor.begin("กี่", longView));
  EXPECT_EQ(6u, placements(cursor).size());
  evictBlocks(longSource);
  calls = longIo.calls;
  ASSERT_TRUE(cursor.begin("กี่", longView));
  EXPECT_EQ(6u, placements(cursor).size());
  EXPECT_LT(calls, longIo.calls);
  EXPECT_FALSE(cache.hasLeases());
}

TEST(ThaiShapeTest, CopiesMovesAndInterleavedStagesRemainIndependentOfEvictionAndLateIoFailure) {
  auto bytes = payload();
  ThaiShapeSource resident;
  resident.setResident(bytes.data(), bytes.size());
  ThaiShapeView oracle;
  ASSERT_TRUE(oracle.validate(resident, 0, bytes.size(), covered, nullptr));
  ThaiGlyphCursor expected;
  ASSERT_TRUE(expected.begin("เรื่อ", oracle));
  auto whole = placements(expected);
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  ThaiShapeView view;
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  ThaiGlyphCursor first, second;
  ASSERT_TRUE(first.begin("เรื่อ", view));
  ThaiGlyphPlacement placement;
  ASSERT_TRUE(first.next(placement));  // Native leading vowel.
  ASSERT_TRUE(first.next(placement));  // Halfway through first recipe.
  auto copy = first;
  ThaiGlyphCursor assigned;
  assigned = first;
  ThaiGlyphCursor moved(std::move(copy));
  ThaiGlyphCursor moveAssigned;
  moveAssigned = std::move(assigned);
  EXPECT_FALSE(copy.next(placement));
  EXPECT_FALSE(assigned.next(placement));
  ASSERT_TRUE(second.begin("เรื่อ", view));
  uint8_t byte;
  for (unsigned i = 0; i < 9; ++i) ASSERT_TRUE(source.readBytes(i * 512, &byte, 1));
  io.failCall = io.calls + 1;
  cache.invalidate(source);
  EXPECT_FALSE(source.readBytes(0, &byte, 1));  // Fault after both successful admissions.
  const auto calls = io.calls;
  const std::vector<ThaiGlyphPlacement> remainder(whole.begin() + 2, whole.end());
  expectPlacements(remainder, placements(first));
  expectPlacements(whole, placements(second));
  expectPlacements(remainder, placements(moved));
  EXPECT_TRUE(cache.hasLeases());
  expectPlacements(remainder, placements(moveAssigned));
  EXPECT_FALSE(cache.hasLeases());
  EXPECT_EQ(calls, io.calls);
}

TEST(ThaiShapeTest, FaultBeforeOrAfterFirstRecipeRejectsWholeClusterAndExhaustionLatches) {
  auto bytes = payload();
  ThaiShapeSource resident;
  resident.setResident(bytes.data(), bytes.size());
  ThaiShapeView oracle;
  ASSERT_TRUE(oracle.validate(resident, 0, bytes.size(), covered, nullptr));
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  ThaiShapeView view;
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  cache.invalidate(source);
  ThaiGlyphCursor cursor;
  const auto initial = io.calls;
  ASSERT_TRUE(cursor.begin("เรื่อ", view));
  const auto admissionReads = io.calls - initial;
  ASSERT_GT(admissionReads, 1u);
  placements(cursor);
  for (size_t failedRead = 1; failedRead <= admissionReads; ++failedRead) {
    io.failCall = 0;
    source.setCached(&io, MemoryIo::read, bytes.size(), cache);
    ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
    cache.invalidate(source);
    io.failCall = io.calls + failedRead;
    EXPECT_FALSE(cursor.begin("เรื่อ", view));
    EXPECT_EQ(std::string_view("เรื่อ").size(), cursor.consumedBytes());
    ThaiGlyphPlacement p;
    EXPECT_FALSE(cursor.next(p));
    EXPECT_TRUE(view.valid());
    EXPECT_TRUE(view.failed());
    EXPECT_FALSE(cache.hasLeases());
    const auto calls = io.calls;
    EXPECT_FALSE(cursor.begin("เรื่อ", view));
    EXPECT_FALSE(cursor.next(p));
    EXPECT_EQ(calls, io.calls);
  }
  io.failCall = 0;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  ThaiGlyphCursor first, second, excess;
  ASSERT_TRUE(first.begin("เรื่อ", view));
  ASSERT_TRUE(second.begin("เรื่อ", view));
  unsigned failures = 0;
  source.setFailureCallback([](void* p, uint32_t) { ++*static_cast<unsigned*>(p); }, &failures);
  EXPECT_FALSE(excess.begin("เรื่อ", view));
  EXPECT_EQ(std::string_view("เรื่อ").size(), excess.consumedBytes());
  EXPECT_TRUE(source.failed());
  EXPECT_EQ(1u, failures);
  ASSERT_TRUE(cursor.begin("เรื่อ", oracle));
  const auto expected = placements(cursor);
  expectPlacements(expected, placements(first));
  expectPlacements(expected, placements(second));
  EXPECT_FALSE(cache.hasLeases());
}

TEST(ThaiShapeTest, StagesSurviveOwnerTeardownAndReleaseOnRebeginOrDestruction) {
  auto bytes = payload();
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiGlyphCursor retained;
  std::vector<ThaiGlyphPlacement> expected;
  {
    ThaiShapeSource source;
    source.setCached(&io, MemoryIo::read, bytes.size(), cache);
    ThaiShapeView view;
    ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
    ASSERT_TRUE(retained.begin("เพื่อ", view));
    auto counter = retained;
    expected = placements(counter);
  }
  EXPECT_TRUE(cache.hasLeases());
  expectPlacements(expected, placements(retained));
  EXPECT_FALSE(cache.hasLeases());
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  ThaiShapeView view;
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  {
    ThaiGlyphCursor scoped;
    ASSERT_TRUE(scoped.begin("เพื่อ", view));
    ASSERT_TRUE(retained.begin("เพื่อ", view));
    EXPECT_FALSE(retained.begin("A", view));
    EXPECT_TRUE(cache.hasLeases());
  }
  EXPECT_FALSE(cache.hasLeases());
}

TEST(ThaiShapeTest, SharedOwnersDoNotConfuseBlocksAndMissingRecipesDoNotLatchFaults) {
  auto a = styledPayload(0, 0), b = styledPayload(3, 5);
  const uint32_t missing = (0xE2D - 0xE01) * 82;
  put16(b, 28 + missing * 4, UINT16_MAX);
  put16(b, 30 + missing * 4, UINT16_MAX);
  ThaiShapeCache cache;
  MemoryIo ioA{a}, ioB{b};
  ThaiShapeSource sourceA, sourceB;
  sourceA.setCached(&ioA, MemoryIo::read, a.size(), cache);
  sourceB.setCached(&ioB, MemoryIo::read, b.size(), cache);
  ThaiShapeView viewA, viewB;
  ASSERT_TRUE(viewA.validate(sourceA, 0, a.size(), covered, nullptr));
  ASSERT_TRUE(viewB.validate(sourceB, 0, b.size(), covered, nullptr));
  ThaiGlyphCursor cursorA, cursorB;
  ASSERT_TRUE(cursorA.begin("เรื่อ", viewA));
  EXPECT_FALSE(cursorB.begin("เรื่อ", viewB));
  EXPECT_FALSE(viewB.failed());
  ThaiGlyphPlacement p;
  EXPECT_FALSE(cursorB.next(p));
  ASSERT_TRUE(cursorB.begin("กี่", viewB));
  cache.invalidate(sourceA);
  sourceA.reset();
  const auto outputA = placements(cursorA);
  const auto outputB = placements(cursorB);
  EXPECT_EQ(3u, outputA.size());
  EXPECT_EQ(6u, outputB.size());
  EXPECT_FALSE(cache.hasLeases());
  EXPECT_FALSE(sourceB.failed());
}

TEST(ThaiShapeTest, StreamedValidationRejectsEveryStructuralMutationAcceptedByNeitherBacking) {
  const auto original = payload();
  const uint32_t base = 28 + ThaiShapeView::DENSE_COUNT * 4;
  const uint32_t offsets = base + ThaiShapeView::DENSE_COUNT * 8;
  const uint32_t data = offsets + 8;
  struct Mutation {
    uint32_t offset;
    uint32_t value;
    uint8_t width;
  };
  for (const auto mutation : {Mutation{0, 0, 2},
                              {2, 0, 2},
                              {4, UINT32_MAX, 4},
                              {8, 28, 4},
                              {12, UINT32_MAX, 4},
                              {16, UINT32_MAX, 4},
                              {24, 0, 2},
                              {26, 1, 2},
                              {28, UINT16_MAX, 2},
                              {30, 1, 2},
                              {base, UINT16_MAX, 2},
                              {offsets, data - 1, 4},
                              {offsets + 4, data, 4},
                              {offsets + 4, uint32_t(original.size() + 1), 4},
                              {data, 6, 1},
                              {data + 1, UINT16_MAX, 2}}) {
    SCOPED_TRACE(mutation.offset);
    auto bytes = original;
    if (mutation.width == 4)
      put32(bytes, mutation.offset, mutation.value);
    else if (mutation.width == 2)
      put16(bytes, mutation.offset, mutation.value);
    else
      bytes[mutation.offset] = mutation.value;
    ThaiShapeCache cache;
    MemoryIo io{bytes};
    ThaiShapeSource resident, cached;
    resident.setResident(bytes.data(), bytes.size());
    cached.setCached(&io, MemoryIo::read, bytes.size(), cache);
    ThaiShapeView expected, actual;
    EXPECT_FALSE(expected.validate(resident, 0, bytes.size(), covered, nullptr));
    EXPECT_FALSE(actual.validate(cached, 0, bytes.size(), covered, nullptr));
    EXPECT_FALSE(expected.valid());
    EXPECT_FALSE(actual.valid());
  }
}

TEST(ThaiShapeTest, MaximumSuffixCrossesBlockAndRuntimeStructureFaultCannotPublishPrefix) {
  auto style = styledPayload(2, 5);
  const uint32_t data = 28 + ThaiShapeView::DENSE_COUNT * 12 + 8;
  const uint32_t prefix = (512 + 492 - data % 512) % 512;
  std::vector<uint8_t> bytes(prefix, 0);
  bytes.insert(bytes.end(), style.begin(), style.end());
  ThaiShapeCache cache;
  MemoryIo io{bytes};
  ThaiShapeSource source, resident;
  source.setCached(&io, MemoryIo::read, bytes.size(), cache);
  resident.setResident(bytes.data(), bytes.size());
  ThaiShapeView view, oracle;
  ASSERT_TRUE(view.validate(source, prefix, style.size(), covered, nullptr));
  ASSERT_TRUE(oracle.validate(resident, prefix, style.size(), covered, nullptr));
  ThaiGlyphCursor actual, expected;
  cache.invalidate(source);
  ASSERT_TRUE(actual.begin("เรื่อ", view));
  ASSERT_TRUE(expected.begin("เรื่อ", oracle));
  expectPlacements(placements(expected), placements(actual));
  // Emulate a changed on-disk count after validation; the immutable-source contract
  // forbids this, but a detected unsafe structure must still fail atomically.
  bytes[prefix + data] = 255;
  cache.invalidate(source);
  EXPECT_FALSE(actual.begin("เรื่อ", view));
  EXPECT_EQ(std::string_view("เรื่อ").size(), actual.consumedBytes());
  ThaiGlyphPlacement p;
  EXPECT_FALSE(actual.next(p));
  EXPECT_TRUE(view.failed());
  EXPECT_TRUE(view.valid());
  EXPECT_FALSE(cache.hasLeases());
}

TEST(ThaiShapeTest, LastStageReleaseMayDeleteCacheAfterSourceHasRetired) {
  auto bytes = payload();
  MemoryIo io{bytes};
  auto* cache = new ThaiShapeCache;
  ThaiShapeSource source;
  source.setCached(&io, MemoryIo::read, bytes.size(), *cache);
  ThaiShapeView view;
  ASSERT_TRUE(view.validate(source, 0, bytes.size(), covered, nullptr));
  ThaiGlyphCursor cursor;
  ASSERT_TRUE(cursor.begin("เรื่อ", view));
  source.reset();
  cache->setIdleCallback([](ThaiShapeCache* retired) { delete retired; });
  const auto live = probe::allocationLiveCount();
  ThaiGlyphPlacement placement;
  while (cursor.next(placement)) {
  }
  EXPECT_EQ(live - 1, probe::allocationLiveCount());
  EXPECT_FALSE(cursor.next(placement));
}
