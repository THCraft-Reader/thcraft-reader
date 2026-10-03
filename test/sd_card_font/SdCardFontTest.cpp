#include <HalMemory.h>
#include <HalStorage.h>
#include <MinizConfig.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

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

// Six Latin glyphs 'A'..'F' plus U+FFFD, with left classes for 'A' and 'C',
// right classes for 'B' and 'D', and a class matrix whose top-left 2×2 corner
// holds their pairs. `extraEntries` appends entries for U+0100 onwards to both
// class tables, so the tables span several read blocks. They use `extraClass`,
// or classes 3..classCount in turn when it is 0. A `classCount` of 0 writes no
// kern data. `ligature` adds one pair, E+F -> A.
void makeKerningFont(uint16_t extraEntries = 0, uint8_t classCount = 2, uint8_t extraClass = 2, bool ligature = false) {
  sdFontTestCompanion.clear();
  constexpr uint32_t KERN_GLYPHS = 7;
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  constexpr size_t KERN_OFFSET = GLYPH_OFFSET + KERN_GLYPHS * sizeof(EpdGlyph);
  constexpr uint8_t LEFT[][2] = {{'A', 1}, {'C', 2}};
  constexpr uint8_t RIGHT[][2] = {{'B', 1}, {'D', 2}};
  constexpr int8_t MATRIX[2][2] = {{-3, 0}, {4, -5}};
  const uint16_t entries = classCount ? 2 + extraEntries : 0;
  const size_t matrixBytes = static_cast<size_t>(classCount) * classCount;
  const size_t ligatureOffset = KERN_OFFSET + entries * 3 * 2 + matrixBytes;
  const size_t bitmapOffset = ligatureOffset + (ligature ? 8 : 0);
  sdFontTestFile.assign(bitmapOffset + KERN_GLYPHS * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, KERN_GLYPHS);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put16(49, entries);  // left class entries
  put16(51, entries);  // right class entries
  sdFontTestFile[53] = classCount;
  sdFontTestFile[54] = classCount;
  sdFontTestFile[55] = ligature ? 1 : 0;
  put32(56, 64);
  put32(64, 'A');
  put32(68, 'F');
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, KERN_GLYPHS - 1);
  for (uint32_t i = 0; i < KERN_GLYPHS; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    glyph.dataOffset = i * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
  }
  size_t at = KERN_OFFSET;
  for (const auto* table : {LEFT, RIGHT}) {
    if (classCount == 0) break;
    for (size_t i = 0; i < 2; ++i, at += 3) {
      put16(at, table[i][0]);
      sdFontTestFile[at + 2] = table[i][1];
    }
    for (uint16_t i = 0; i < extraEntries; ++i, at += 3) {
      put16(at, 0x100 + i);
      sdFontTestFile[at + 2] = extraClass ? extraClass : 3 + i % (classCount - 2);
    }
  }
  for (size_t row = 0; classCount > 0 && row < 2; ++row) {
    std::memcpy(sdFontTestFile.data() + at + row * classCount, MATRIX[row], sizeof(MATRIX[row]));
  }
  if (ligature) {
    put32(ligatureOffset, 'E' << 16 | 'F');
    put32(ligatureOffset + 4, 'A');
  }
}

std::string latinPage(const char* ascii, uint32_t first, uint32_t count) {
  std::string text = ascii;
  for (uint32_t cp = first; cp < first + count; ++cp) {
    text.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    text.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return text;
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
  sdFontTestCompanion[p] = n;
  sdFontTestCompanion[p + 1] = n >> 8;
}
void shapePut32(size_t p, uint32_t n) {
  shapePut16(p, n);
  shapePut16(p + 2, n >> 16);
}
void updateShapeCrc(uint32_t glyphCount = GLYPHS) {
  const size_t metadataEnd = sdFontTestFile.size() - glyphCount * BITMAP_BYTES;
  shapePut32(16, mz_crc32(MZ_CRC32_INIT, sdFontTestFile.data(), metadataEnd));
  shapePut32(20, mz_crc32(MZ_CRC32_INIT, sdFontTestCompanion.data() + 32, sdFontTestCompanion.size() - 32));
}
void makeShape(uint32_t glyphCount = GLYPHS, uint16_t first = FIRST) {
  constexpr uint32_t base = 28 + ThaiShapeView::DENSE_COUNT * 4;
  constexpr uint32_t offsets = base + 8;
  constexpr uint32_t data = offsets + 8;
  constexpr uint32_t styleSize = data + 1;
  sdFontTestCompanion.assign(44 + styleSize, 0);
  std::memcpy(sdFontTestCompanion.data(), "CPSHAPE\0", 8);
  shapePut16(8, 1);
  shapePut16(10, 32);
  shapePut32(12, sdFontTestFile.size());
  shapePut32(24, sdFontTestCompanion.size());
  shapePut32(28, 1);
  shapePut32(36, 44);
  shapePut32(40, styleSize);
  shapePut16(44, 1);
  shapePut16(46, 1);
  shapePut32(48, 28);
  shapePut32(52, base);
  shapePut32(56, offsets);
  shapePut32(60, data);
  shapePut16(64, 32);
  shapePut16(68, 32);
  shapePut16(44 + base, first);
  shapePut16(44 + base + 2, 32 << 4);
  shapePut32(44 + offsets, data);
  shapePut32(48 + offsets, styleSize);
  updateShapeCrc(glyphCount);
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
  makeFont();
  makeShape();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto* shape = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, shape);
  const auto hash = font.contentHash();
  ASSERT_EQ(0, font.prewarm(page(FIRST, 4).c_str(), 1));
  EXPECT_EQ(shape, font.getEpdFont()->getThaiShape());
  font.clearCache();
  font.clearPersistentCache();
  font.releaseResidentCaches();
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
      case 0:
        sdFontTestCompanion.resize(20);
        break;
      case 1:
        shapePut16(8, 2);
        break;
      case 2:
        shapePut32(12, sdFontTestFile.size() + 1);
        break;
      case 3:
        sdFontTestCompanion[16] ^= 1;
        break;
      case 4:
        sdFontTestCompanion.back() ^= 1;
        break;
      case 5:
        sdFontTestCompanion.resize(ThaiShapeView::MAX_FAMILY_BYTES + 1);
        break;
      case 6:
        shapePut16(44 + 28, 2);
        updateShapeCrc();
        break;
      case 7:
        shapePut32(56, UINT32_MAX);
        updateShapeCrc();
        break;
      case 8:
        shapePut16(44 + 28 + ThaiShapeView::DENSE_COUNT * 4, 1);
        updateShapeCrc();
        break;
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
  makeFont();
  makeShape();
  struct ResetFailure {
    ~ResetFailure() { failNextArraySize = 0; }
  } guard;
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
  makeFont();
  makeShape();
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
  makeFont();
  makeShape();
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

TEST(SdCardFontTest, PagesKernWithTheFontsClassMatrix) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(0, epd->getKerning('A', 'D'));
  EXPECT_EQ(4, epd->getKerning('C', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning('B', 'D'));
  EXPECT_EQ(0, epd->getKerning('E', 'B'));
}

TEST(SdCardFontTest, KernRequestsServedFromAKernFreeMiniStillKern) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  struct Step {
    const char* text;
    uint32_t left, right;
    int8_t kern;
  };
  // A subset without kerning pairs, then subsets whose pairs the earlier ones did not cover.
  for (const Step& step : {Step{"EF", 'E', 'F', 0}, Step{"AB", 'A', 'B', -3}, Step{"CD", 'C', 'D', -5}}) {
    ASSERT_EQ(0, font.prewarm(step.text, 1, false, true, false));
    EXPECT_EQ(step.kern, font.getEpdFont()->getKerning(step.left, step.right)) << step.text;
  }
}

TEST(SdCardFontTest, RedrawsAfterAKernFreeRebuildStillKern) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, false, false));  // kern-free rebuild, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));     // the page again, served from that cache
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
}

TEST(SdCardFontTest, ClassIdsPastTheMatrixAreUnkerned) {
  makeKerningFont(1, 2, 250);  // U+0100 claims class 250 in a 2×2 matrix
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(1, font.prewarm(latinPage("ABCD", 0x100, 1).c_str(), 1, false, true, false));  // U+0100 has no glyph
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(0, epd->getKerning('A', 0x100));
  EXPECT_EQ(0, epd->getKerning(0x100, 'B'));
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
}

TEST(SdCardFontTest, PagesCanUseAll255KernClasses) {
  makeKerningFont(253, 255, 0);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(253, font.prewarm(latinPage("ABCD", 0x100, 253).c_str(), 1, false, true, false));
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
  EXPECT_EQ(0, epd->getKerning(0x100, 0x1FC));
}

TEST(SdCardFontTest, AFailedKernBuildKeepsTheLigatures) {
  makeKerningFont(253, 255, 0, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  failNextArraySize = 255 * 255;  // the mini kern matrix
  ASSERT_EQ(253, font.prewarm(latinPage("ABCDEF", 0x100, 253).c_str(), 1, false, true, false));
  EXPECT_EQ(0U, failNextArraySize);
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(static_cast<uint32_t>('A'), epd->getLigature('E', 'F'));
  EXPECT_EQ(0, epd->getKerning('A', 'B'));
}

TEST(SdCardFontTest, LigatureRequestsServedFromAKernFreeMiniGetLigatures) {
  makeKerningFont(0, 0, 2, true);  // ligatures, no kern classes
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm("AEF", 1, false, false, false));  // kern-free prewarm, e.g. a UI string
  ASSERT_EQ(0, font.prewarm("EF", 1, false, true, false));
  EXPECT_EQ(static_cast<uint32_t>('A'), font.getEpdFont()->getLigature('E', 'F'));
}

TEST(SdCardFontTest, LayoutKernReplacementDoesNotInvalidateResidentPageKerning) {
  makeKerningFont();
  makeShape(7, 'A');
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  ASSERT_EQ(0, font.prewarm("ABCD", 1, false, true, false));
  ASSERT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  ASSERT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));

  ASSERT_EQ(0, font.buildAdvanceTable("AB", 1));
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  EXPECT_EQ(0, font.getEpdFont()->getKerning('C', 'D'));
  ASSERT_EQ(0, font.prewarm("CD", 1, false, true, false));
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
}
