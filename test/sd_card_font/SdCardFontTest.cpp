#include <HalStorage.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>
#include <MinizConfig.h>
#include <HalMemory.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {
size_t failNextArraySize = 0;
}  // namespace

// Keep array allocation and deletion paired, including arrays allocated by the test framework.
void* operator new[](size_t size) {
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in SdCardFontTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  if (failNextArraySize != 0 && size == failNextArraySize) {
    failNextArraySize = 0;
    return nullptr;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation, size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }

namespace {
constexpr uint32_t FIRST = 0xAC00;
constexpr uint32_t GLYPHS = 513;
constexpr uint16_t BITMAP_BYTES = 128;

void put16(size_t at, uint16_t value) {
  sdFontTestFile[at] = value;
  sdFontTestFile[at + 1] = value >> 8;
}
void put32(size_t at, uint32_t value) {
  put16(at, value);
  put16(at + 2, value >> 16);
}

void makeFont() {
  sdFontTestCompanion.clear();
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  constexpr size_t BITMAP_OFFSET = GLYPH_OFFSET + GLYPHS * sizeof(EpdGlyph);
  sdFontTestFile.assign(BITMAP_OFFSET + GLYPHS * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, GLYPHS);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + GLYPHS - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, GLYPHS - 1);
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    // Store bitmaps in reverse glyph order to exercise sorted reads on rebuild.
    glyph.dataOffset = (GLYPHS - 1 - i) * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    std::memset(sdFontTestFile.data() + BITMAP_OFFSET + glyph.dataOffset, i % 251, BITMAP_BYTES);
  }
}

std::string page(uint32_t first, uint32_t count) {
  std::string text;
  text.reserve(count * 3);
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
}

uint32_t residentCount(SdCardFont& font) {
  const auto* data = font.getEpdFont()->data;
  uint32_t count = 0;
  for (uint32_t i = 0; i < data->intervalCount; ++i) {
    count += data->intervals[i].last - data->intervals[i].first + 1;
  }
  return count;
}

void expectPageBitmaps(SdCardFont& font, uint32_t first, uint32_t count) {
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    const EpdGlyph* glyph = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) glyph = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, glyph) << cp;
    ASSERT_EQ(BITMAP_BYTES, glyph->dataLength);
    for (uint16_t i = 0; i < BITMAP_BYTES; ++i) {
      ASSERT_EQ((cp - FIRST) % 251, data->bitmap[glyph->dataOffset + i]);
    }
  }
}

void shapePut16(size_t p, uint16_t n) {
  sdFontTestCompanion[p] = n; sdFontTestCompanion[p + 1] = n >> 8;
}
void shapePut32(size_t p, uint32_t n) { shapePut16(p, n); shapePut16(p + 2, n >> 16); }
void updateShapeCrc() {
  constexpr size_t metadataEnd = 64 + 24 + GLYPHS * sizeof(EpdGlyph);
  shapePut32(16, mz_crc32(MZ_CRC32_INIT, sdFontTestFile.data(), metadataEnd));
  shapePut32(20, mz_crc32(MZ_CRC32_INIT, sdFontTestCompanion.data() + 32, sdFontTestCompanion.size() - 32));
}
void makeShape() {
  constexpr uint32_t base = 28 + ThaiShapeView::DENSE_COUNT * 4;
  constexpr uint32_t offsets = base + 8;
  constexpr uint32_t data = offsets + 8;
  constexpr uint32_t styleSize = data + 1;
  sdFontTestCompanion.assign(44 + styleSize, 0);
  std::memcpy(sdFontTestCompanion.data(), "CPSHAPE\0", 8);
  shapePut16(8, 1); shapePut16(10, 32);
  shapePut32(12, sdFontTestFile.size()); shapePut32(24, sdFontTestCompanion.size()); shapePut32(28, 1);
  shapePut32(36, 44); shapePut32(40, styleSize);
  shapePut16(44, 1); shapePut16(46, 1);
  shapePut32(48, 28); shapePut32(52, base); shapePut32(56, offsets); shapePut32(60, data);
  shapePut16(64, 32); shapePut16(68, 32);
  shapePut16(44 + base, FIRST); shapePut16(44 + base + 2, 32 << 4);
  shapePut32(44 + offsets, data); shapePut32(48 + offsets, styleSize);
  updateShapeCrc();
}
}  // namespace

TEST(SdCardFontTest, CompletePagesReplaceEarlierGlyphs) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t offset : {0U, 100U, 200U}) {
    const uint32_t first = FIRST + offset;
    const auto text = page(first, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(101U, residentCount(font));  // includes replacement glyph
    expectPageBitmaps(font, first, 100);
  }
}

TEST(SdCardFontTest, IncrementalUiStringsStillAccumulate) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1));
  ASSERT_EQ(0, font.prewarm(page(FIRST + 5, 5).c_str(), 1));
  EXPECT_EQ(11U, residentCount(font));
  expectPageBitmaps(font, FIRST, 10);
}

TEST(SdCardFontTest, UnderusedBuffersKeepTheFreshlyPrewarmedPageUntilItIsDrawn) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 200).c_str(), 1, false, false, false));
  font.clearCache();
  for (uint32_t offset : {300U, 400U, 200U, 0U}) {
    const auto text = page(FIRST + offset, 100);
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    font.clearCache();  // idle prewarm scope closes
    font.clearCache();  // actual page render scope opens
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
    EXPECT_EQ(0U, sdFontTestReads);
    expectPageBitmaps(font, FIRST + offset, 100);
  }
}

TEST(SdCardFontTest, BitmapGrowthPreservesReadOrderAndAllGlyphData) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (uint32_t count : {50U, 100U, 200U, 400U}) {
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(page(FIRST, count).c_str(), 1, false, false, false));
    EXPECT_EQ(count + 1, residentCount(font));
    expectPageBitmaps(font, FIRST, count);
  }
}

TEST(SdCardFontTest, FragmentedBitmapGrowthRebuildsMetadataAndKeepsThePrefetch) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct HeapReportGuard {
    ~HeapReportGuard() { ESP.largestBlock = 200 * 1024; }
  } guard;
  ESP.largestBlock = 8 * 1024;
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  expectPageBitmaps(font, FIRST, 200);
  font.clearCache();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, sdFontTestReads);
}

TEST(SdCardFontTest, BitmapAllocationRetriesAfterEvictingRebuildableAdvances) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const auto text = page(FIRST, 200);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  ASSERT_TRUE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.prewarm(page(FIRST, 50).c_str(), 1, false, false, false));
  struct AllocationGuard {
    ~AllocationGuard() {
      ESP.largestBlock = 200 * 1024;
      failNextArraySize = 0;
    }
  } guard;
  ESP.largestBlock = 8 * 1024;
  failNextArraySize = 201 * BITMAP_BYTES;
  ASSERT_EQ(0, font.prewarm(text.c_str(), 1, false, false, false));
  EXPECT_EQ(0U, failNextArraySize);
  EXPECT_FALSE(font.hasAdvanceTable());
  expectPageBitmaps(font, FIRST, 200);

  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 200; ++cp) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance(cp, 0, advance));
    EXPECT_EQ(32 << 4, advance);
  }
}

TEST(SdCardFontTest, ZeroAdvanceIsCachedWithoutRepeatedStorageFaults) {
  makeFont();
  put16(64 + 24 + offsetof(EpdGlyph, advanceX), 0);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto text = page(FIRST, 1);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  uint16_t advance = 123;
  ASSERT_TRUE(font.getAdvance(FIRST, 0, advance));
  EXPECT_EQ(0, advance);
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  EXPECT_EQ(0u, sdFontTestReads);
  EXPECT_FALSE(font.getAdvance(FIRST + 1, 0, advance));
}

TEST(SdCardFontTest, CompanionStaysPublishedAcrossAllResidentCacheEvictions) {
  makeFont(); makeShape();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto* shape = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, shape);
  const auto hash = font.contentHash();
  ASSERT_EQ(0, font.prewarm(page(FIRST, 4).c_str(), 1));
  EXPECT_EQ(shape, font.getEpdFont()->getThaiShape());
  font.clearCache(); font.clearPersistentCache(); font.releaseResidentCaches();
  EXPECT_EQ(shape, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(hash, font.contentHash());
  sdFontTestReads = 0;
  ThaiGlyphCursor cursor;
  ASSERT_TRUE(cursor.begin("กี่", *shape));
  ThaiGlyphPlacement glyph;
  ASSERT_TRUE(cursor.next(glyph));
  EXPECT_EQ(FIRST, glyph.codepoint);
  EXPECT_EQ(0u, sdFontTestReads);
  sdFontTestCompanion.clear();
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_NE(hash, font.contentHash());
}

TEST(SdCardFontTest, CorruptOrOversizedCompanionCannotFailNativeFontLoad) {
  makeFont();
  SdCardFont native;
  ASSERT_TRUE(native.load("fixture.cpfont"));
  const auto nativeHash = native.contentHash();
  for (int scenario = 0; scenario < 9; ++scenario) {
    SCOPED_TRACE(scenario);
    makeShape();
    switch (scenario) {
      case 0: sdFontTestCompanion.resize(20); break;
      case 1: shapePut16(8, 2); break;
      case 2: shapePut32(12, sdFontTestFile.size() + 1); break;
      case 3: sdFontTestCompanion[16] ^= 1; break;
      case 4: sdFontTestCompanion.back() ^= 1; break;
      case 5: sdFontTestCompanion.resize(ThaiShapeView::MAX_FAMILY_BYTES + 1); break;
      case 6: shapePut16(44 + 28, 2); updateShapeCrc(); break;
      case 7: shapePut32(56, UINT32_MAX); updateShapeCrc(); break;
      case 8: shapePut16(44 + 28 + ThaiShapeView::DENSE_COUNT * 4, 1); updateShapeCrc(); break;
    }
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
    EXPECT_EQ(nativeHash, font.contentHash());
    ASSERT_EQ(0, font.prewarm(page(FIRST, 1).c_str(), 1));
    expectPageBitmaps(font, FIRST, 1);
  }
}

TEST(SdCardFontTest, CompanionAllocationFailureKeepsNativeThenHealthyReopenShapes) {
  makeFont(); makeShape();
  struct ResetFailure { ~ResetFailure() { failNextArraySize = 0; } } guard;
  failNextArraySize = sdFontTestCompanion.size();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, failNextArraySize);
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  const auto nativeHash = font.contentHash();
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_NE(nativeHash, font.contentHash());
}

TEST(SdCardFontTest, SameSizeNativeMetricsEditChangesActiveFontIdentity) {
  makeFont(); makeShape();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  const auto before = font.contentHash();
  const auto bytes = sdFontTestFile.size();
  put16(64 + 24 + offsetof(EpdGlyph, advanceX), 17 << 4);
  updateShapeCrc();  // recipe bytes and their payload CRC do not change
  ASSERT_EQ(bytes, sdFontTestFile.size());
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_NE(before, font.contentHash());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 1).c_str(), 1));
  uint16_t advance = 0;
  ASSERT_TRUE(font.getAdvance(FIRST, 0, advance));
  EXPECT_EQ(17 << 4, advance);
}

TEST(SdCardFontTest, CompanionHeadroomRejectionIsStableUntilFontReload) {
  makeFont(); makeShape();
  struct HeadroomGuard {
    size_t saved = probe::internalHeadroomBytes;
    ~HeadroomGuard() { probe::internalHeadroomBytes = saved; }
  } guard;
  probe::internalHeadroomBytes = 50 * 1024;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  const auto hash = font.contentHash();
  probe::internalHeadroomBytes = guard.saved;
  ASSERT_EQ(0, font.prewarm(page(FIRST, 1).c_str(), 1));
  font.releaseResidentCaches();
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(hash, font.contentHash());
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_NE(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_NE(hash, font.contentHash());
}
