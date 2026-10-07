#include <HalMemory.h>
#include <HalStorage.h>
#include <MinizConfig.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {
size_t failNextArraySize = 0;
size_t failNextScalarSize = 0;
size_t failArrayOrdinal = 1;
size_t failScalarOrdinal = 1;
size_t nullableCalls = 0;
size_t allAllocationCalls = 0;
size_t cacheAllocationCalls = 0;
size_t maximumNullableAllocation = static_cast<size_t>(-1);
struct Allocation {
  void* pointer = nullptr;
  size_t size = 0;
};
Allocation nullableAllocations[4096];
size_t nullableLiveBytes = 0;
size_t nullableLiveCount = 0;

void* allocateNullable(size_t size, bool array) {
  ++nullableCalls;
  ++allAllocationCalls;
  if (!array && size == sizeof(ThaiShapeCache)) ++cacheAllocationCalls;
  if (size > maximumNullableAllocation) return nullptr;
  size_t& failSize = array ? failNextArraySize : failNextScalarSize;
  size_t& ordinal = array ? failArrayOrdinal : failScalarOrdinal;
  if (failSize && size == failSize && --ordinal == 0) {
    failSize = 0;
    ordinal = 1;
    return nullptr;
  }
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (allocation) {
    for (auto& entry : nullableAllocations) {
      if (entry.pointer) continue;
      entry = {allocation, size};
      nullableLiveBytes += size;
      ++nullableLiveCount;
      return allocation;
    }
    std::abort();
  }
  return nullptr;
}

void releaseAllocation(void* allocation) {
  if (!allocation) return;
  for (auto& entry : nullableAllocations) {
    if (entry.pointer != allocation) continue;
    nullableLiveBytes -= entry.size;
    --nullableLiveCount;
    entry = {};
    break;
  }
  std::free(allocation);
}

void* allocateRequired(size_t size) {
  ++allAllocationCalls;
  void* allocation = std::malloc(size == 0 ? 1 : size);
  if (!allocation) {
    std::fputs("Unexpected host OOM in SdCardFontTest\n", stderr);
    std::exit(EXIT_FAILURE);
  }
  return allocation;
}
}  // namespace

// Scalar and array replacements use one matching malloc/free family. Only
// nullable production allocations are tracked; the accounting itself allocates nothing.
void* operator new(size_t size) { return allocateRequired(size); }
void* operator new[](size_t size) { return allocateRequired(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return allocateNullable(size, false); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return allocateNullable(size, true); }
void operator delete(void* allocation) noexcept { releaseAllocation(allocation); }
void operator delete(void* allocation, size_t) noexcept { releaseAllocation(allocation); }
void operator delete(void* allocation, const std::nothrow_t&) noexcept { releaseAllocation(allocation); }
void operator delete[](void* allocation) noexcept { releaseAllocation(allocation); }
void operator delete[](void* allocation, size_t) noexcept { releaseAllocation(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { releaseAllocation(allocation); }

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

// `glyphs` Hangul glyphs from FIRST, the last of them U+FFFD.
void makeFont(uint32_t glyphs = GLYPHS) {
  sdFontTestCompanion.clear();
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  const size_t BITMAP_OFFSET = GLYPH_OFFSET + glyphs * sizeof(EpdGlyph);
  sdFontTestFile.assign(BITMAP_OFFSET + glyphs * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 2);
  put32(40, glyphs);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put32(56, 64);
  put32(64, FIRST);
  put32(68, FIRST + glyphs - 2);
  put32(76, 0xFFFD);
  put32(80, 0xFFFD);
  put32(84, glyphs - 1);
  for (uint32_t i = 0; i < glyphs; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = 32 << 4;
    glyph.top = 32;
    glyph.dataLength = BITMAP_BYTES;
    // Store bitmaps in reverse glyph order to exercise sorted reads on rebuild.
    glyph.dataOffset = (glyphs - 1 - i) * BITMAP_BYTES;
    std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + i * sizeof(glyph), &glyph, sizeof(glyph));
    std::memset(sdFontTestFile.data() + BITMAP_OFFSET + glyph.dataOffset, i % 251, BITMAP_BYTES);
  }
}

// Six Latin glyphs 'A'..'F' plus U+FFFD, with left classes for 'A' and 'C',
// right classes for 'B' and 'D', and a class matrix whose top-left 2×2 corner
// holds their pairs. `extraEntries` appends entries for U+0100 onwards to both
// class tables, so the tables span several read blocks. They use `extraClass`,
// or classes 3..classCount in turn when it is 0. A `classCount` of 0 writes no
// kern data. `ligature` adds one pair, E+F -> A. `metricFixture` additionally
// covers separators, Thai/CJK and the reverse-dependency chain A+B -> C, E+F -> A.
void makeKerningFont(uint16_t extraEntries = 0, uint8_t classCount = 2, uint8_t extraClass = 2, bool ligature = false,
                     bool metricFixture = false) {
  sdFontTestCompanion.clear();
  const uint32_t KERN_GLYPHS = metricFixture ? 14 : 7;
  const size_t GLYPH_OFFSET = 64 + (metricFixture ? 6 : 2) * sizeof(EpdUnicodeInterval);
  const size_t KERN_OFFSET = GLYPH_OFFSET + KERN_GLYPHS * sizeof(EpdGlyph);
  constexpr uint8_t LEFT[][2] = {{'A', 1}, {'C', 2}};
  constexpr uint8_t RIGHT[][2] = {{'B', 1}, {'D', 2}};
  constexpr int8_t MATRIX[2][2] = {{-3, 0}, {4, -5}};
  const uint16_t entries = classCount ? 2 + extraEntries : 0;
  const size_t matrixBytes = static_cast<size_t>(classCount) * classCount;
  const size_t ligatureOffset = KERN_OFFSET + entries * 3 * 2 + matrixBytes;
  const size_t bitmapOffset = ligatureOffset + (metricFixture ? 16 : ligature ? 8 : 0);
  sdFontTestFile.assign(bitmapOffset + KERN_GLYPHS * BITMAP_BYTES, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, metricFixture ? 6 : 2);
  put32(40, KERN_GLYPHS);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  put16(49, entries);  // left class entries
  put16(51, entries);  // right class entries
  sdFontTestFile[53] = classCount;
  sdFontTestFile[54] = classCount;
  sdFontTestFile[55] = metricFixture ? 2 : ligature ? 1 : 0;
  put32(56, 64);
  if (metricFixture) {
    constexpr EpdUnicodeInterval intervals[] = {{' ', ' ', 0},     {'-', '-', 1},        {'A', 'F', 2},
                                                {0xE01, 0xE04, 8}, {0x4E00, 0x4E00, 12}, {0xFFFD, 0xFFFD, 13}};
    std::memcpy(sdFontTestFile.data() + 64, intervals, sizeof(intervals));
  } else {
    put32(64, 'A');
    put32(68, 'F');
    put32(76, 0xFFFD);
    put32(80, 0xFFFD);
    put32(84, KERN_GLYPHS - 1);
  }
  for (uint32_t i = 0; i < KERN_GLYPHS; ++i) {
    EpdGlyph glyph{};
    glyph.width = 32;
    glyph.height = 32;
    glyph.advanceX = metricFixture ? (10 + i) << 4 : 32 << 4;
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
  if (metricFixture) {
    put32(ligatureOffset, 'A' << 16 | 'B');
    put32(ligatureOffset + 4, 'C');
    put32(ligatureOffset + 8, 'E' << 16 | 'F');
    put32(ligatureOffset + 12, 'A');
  } else if (ligature) {
    put32(ligatureOffset, 'E' << 16 | 'F');
    put32(ligatureOffset + 4, 'A');
  }
}

// Duplicate a native fixture's style payload so emission really traverses four
// active styles rather than resolving four missing styles to regular.
void duplicateNativeStyles() {
  const auto regular = sdFontTestFile;
  const size_t styleBytes = regular.size() - 64;
  sdFontTestFile.assign(160 + 4 * styleBytes, 0);
  std::memcpy(sdFontTestFile.data(), regular.data(), 32);
  sdFontTestFile[12] = 4;
  for (uint8_t style = 0; style < 4; ++style) {
    const size_t toc = 32 + style * 32;
    std::memcpy(sdFontTestFile.data() + toc, regular.data() + 32, 32);
    sdFontTestFile[toc] = style;
    put32(toc + 24, 160 + style * styleBytes);
    std::memcpy(sdFontTestFile.data() + 160 + style * styleBytes, regular.data() + 64, styleBytes);
  }
}

void makeMetricRangeFont(uint32_t count, bool nativeLigatures = false) {
  sdFontTestCompanion.clear();
  constexpr size_t glyphOffset = 64 + 4 * sizeof(EpdUnicodeInterval);
  const size_t ligatureOffset = glyphOffset + (count + 3) * sizeof(EpdGlyph);
  const size_t bitmapOffset = ligatureOffset + (nativeLigatures ? sizeof(EpdLigaturePair) : 0);
  sdFontTestFile.assign(bitmapOffset + 1, 0);
  std::memcpy(sdFontTestFile.data(), "CPFONT\0\0", 8);
  put16(8, CPFONT_VERSION);
  sdFontTestFile[12] = 1;
  put32(36, 4);
  put32(40, count + 3);
  sdFontTestFile[44] = 32;
  put16(45, 32);
  sdFontTestFile[55] = nativeLigatures ? 1 : 0;
  put32(56, 64);
  const EpdUnicodeInterval intervals[] = {
      {' ', ' ', 0}, {'-', '-', 1}, {0x4E00, 0x4E00 + count - 1, 2}, {0xFFFD, 0xFFFD, count + 2}};
  std::memcpy(sdFontTestFile.data() + 64, intervals, sizeof(intervals));
  for (uint32_t i = 0; i < count + 3; ++i) {
    EpdGlyph glyph{};
    glyph.width = glyph.height = 1;
    glyph.advanceX = (10 + i % 17) << 4;
    glyph.top = 1;
    glyph.dataLength = 1;
    std::memcpy(sdFontTestFile.data() + glyphOffset + i * sizeof(glyph), &glyph, sizeof(glyph));
  }
  if (nativeLigatures) {
    put32(ligatureOffset, 'E' << 16 | 'F');
    put32(ligatureOffset + 4, 0x4E00);
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

struct ShapeTestScope {
  size_t headroom = probe::internalHeadroomBytes;
  size_t liveBytes = nullableLiveBytes;
  size_t liveCount = nullableLiveCount;
  size_t allocationLimit = maximumNullableAllocation;
  ~ShapeTestScope() {
    failNextArraySize = failNextScalarSize = 0;
    failArrayOrdinal = failScalarOrdinal = 1;
    maximumNullableAllocation = allocationLimit;
    probe::internalHeadroomBytes = headroom;
    probe::resetInternalHeapSamples();
    sdFontTestResetShapeFaults();
    EXPECT_EQ(liveBytes, nullableLiveBytes);
    EXPECT_EQ(liveCount, nullableLiveCount);
    EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  }
};

void expectShape(SdCardFont& font, uint8_t style = 0, uint32_t expected = FIRST) {
  const auto* view = font.getEpdFont(style)->getThaiShape();
  ASSERT_NE(nullptr, view);
  const auto allocations = allAllocationCalls;
  ThaiGlyphCursor cursor;
  ThaiGlyphPlacement placement{};
  const bool admitted = cursor.begin("\xE0\xB8\x81", *view);
  const bool placed = cursor.next(placement);
  const bool ended = !cursor.next(placement);
  const auto allocationsAfter = allAllocationCalls;
  ASSERT_TRUE(admitted);
  ASSERT_TRUE(placed);
  EXPECT_TRUE(ended);
  EXPECT_EQ(allocations, allocationsAfter);
  EXPECT_EQ(expected, placement.codepoint);
  EXPECT_EQ(3u, cursor.consumedBytes());
}

std::string shapeKeyText(uint32_t key) {
  std::string text = page(0xE01 + key / 82, 1);
  const uint32_t variant = key % 82;
  if (variant >= 77) {
    if (variant != 77) text += page(0xE48 + variant - 78, 1);
    text += page(0xE33, 1);
  } else {
    const uint32_t vowel = variant / 7, terminal = variant % 7;
    const uint16_t vowels[] = {0, 0xE31, 0xE34, 0xE35, 0xE36, 0xE37, 0xE38, 0xE39, 0xE3A, 0xE47, 0xE4D};
    if (vowel) text += page(vowels[vowel], 1);
    if (terminal) text += page(terminal == 6 ? 0xE4E : 0xE47 + terminal, 1);
  }
  return text;
}

void makeFourStyleShape() {
  makeFont();
  const auto regular = sdFontTestFile;
  constexpr size_t nativeMetadataBytes = 24 + GLYPHS * sizeof(EpdGlyph);
  const size_t nativeStyleBytes = regular.size() - 64;
  sdFontTestFile.assign(160 + 4 * nativeStyleBytes, 0);
  std::memcpy(sdFontTestFile.data(), regular.data(), 32);
  sdFontTestFile[12] = 4;
  for (uint8_t i = 0; i < 4; ++i) {
    const size_t toc = 32 + i * 32;
    std::memcpy(sdFontTestFile.data() + toc, regular.data() + 32, 32);
    sdFontTestFile[toc] = i;
    sdFontTestFile[toc + 12] = 32 + i;
    put16(toc + 13, 32 + i);
    put16(toc + 15, -i);
    put32(toc + 24, 160 + i * nativeStyleBytes);
    std::memcpy(sdFontTestFile.data() + 160 + i * nativeStyleBytes, regular.data() + 64, nativeStyleBytes);
  }
  constexpr uint32_t base = 28 + ThaiShapeView::DENSE_COUNT * 4;
  constexpr uint32_t offsets = base + 4 * 8;
  constexpr uint32_t data = offsets + 8;
  constexpr uint32_t styleSize = data + 1;
  sdFontTestCompanion.assign(80 + 4 * styleSize, 0);
  std::memcpy(sdFontTestCompanion.data(), "CPSHAPE\0", 8);
  shapePut16(8, 1);
  shapePut16(10, 32);
  shapePut32(12, sdFontTestFile.size());
  shapePut32(24, sdFontTestCompanion.size());
  shapePut32(28, 4);
  for (uint8_t i = 0; i < 4; ++i) {
    const uint32_t section = 80 + i * styleSize;
    sdFontTestCompanion[32 + i * 12] = i;
    shapePut32(36 + i * 12, section);
    shapePut32(40 + i * 12, styleSize);
    shapePut16(section, 4);
    shapePut16(section + 2, 1);
    shapePut32(section + 4, 28);
    shapePut32(section + 8, base);
    shapePut32(section + 12, offsets);
    shapePut32(section + 16, data);
    shapePut16(section + 20, 32 + i);
    shapePut16(section + 22, -i);
    shapePut16(section + 24, 32 + i);
    for (uint32_t key = 0; key < ThaiShapeView::DENSE_COUNT; ++key) shapePut16(section + 28 + key * 4, (key + i) % 4);
    for (uint8_t recipe = 0; recipe < 4; ++recipe) {
      shapePut16(section + base + recipe * 8, FIRST + i * 4 + recipe);
      shapePut16(section + base + recipe * 8 + 2, (32 + i) << 4);
      shapePut16(section + base + recipe * 8 + 4, -i * 16);
      shapePut16(section + base + recipe * 8 + 6, i * 16);
    }
    shapePut32(section + offsets, data);
    shapePut32(section + offsets + 4, styleSize);
  }
  uint32_t crc = mz_crc32(MZ_CRC32_INIT, sdFontTestFile.data(), 160);
  for (uint8_t i = 0; i < 4; ++i)
    crc = mz_crc32(crc, sdFontTestFile.data() + 160 + i * nativeStyleBytes, nativeMetadataBytes);
  shapePut32(16, crc);
  shapePut32(20, mz_crc32(MZ_CRC32_INIT, sdFontTestCompanion.data() + 32, sdFontTestCompanion.size() - 32));
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
  ShapeTestScope guard;
  makeFont();
  SdCardFont native;
  ASSERT_TRUE(native.load("fixture.cpfont"));
  const auto nativeHash = native.contentHash();
  for (int scenario = 0; scenario < 15; ++scenario) {
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
      case 9:
        sdFontTestCompanion[33] = 1;
        updateShapeCrc();
        break;
      case 10:
        shapePut32(36, 45);
        updateShapeCrc();
        break;
      case 11:
        shapePut16(64, 31);
        updateShapeCrc();
        break;
      case 12:
        shapePut16(44 + 28, 0xFFFF);
        updateShapeCrc();
        break;
      case 13:
        shapePut32(48 + 28 + ThaiShapeView::DENSE_COUNT * 4 + 8, UINT32_MAX);
        updateShapeCrc();
        break;
      case 14:
        sdFontTestCompanion.back() = 6;
        updateShapeCrc();
        break;
    }
    for (const size_t headroom : {guard.headroom, size_t{45 * 1024}}) {
      probe::internalHeadroomBytes = headroom;
      SdCardFont font;
      ASSERT_TRUE(font.load("fixture.cpfont"));
      EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
      EXPECT_EQ(nativeHash, font.contentHash());
      EXPECT_FALSE(font.hasThaiShapeError());
      ASSERT_EQ(0, font.prewarm(page(FIRST, 1).c_str(), 1));
      expectPageBitmaps(font, FIRST, 1);
    }
  }
}

TEST(SdCardFontTest, CompanionAllocationFailureUsesCachedBackingWithIdenticalIdentity) {
  makeFont();
  makeShape();
  ShapeTestScope guard;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  const auto hash = font.contentHash();
  failNextArraySize = sdFontTestCompanion.size();
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, failNextArraySize);
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(hash, font.contentHash());
  EXPECT_EQ(1u, sdFontTestShapeOpenHandles);
  expectShape(font);
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  EXPECT_EQ(hash, font.contentHash());
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

TEST(SdCardFontTest, LowHeadroomKeepsShapingAndIdentityAcrossCacheReleaseAndReload) {
  makeFont();
  makeShape();
  ShapeTestScope guard;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto hash = font.contentHash();
  for (const size_t headroom : {45u * 1024, 50u * 1024}) {
    probe::internalHeadroomBytes = headroom;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    const auto* view = font.getEpdFont()->getThaiShape();
    ASSERT_NE(nullptr, view);
    EXPECT_EQ(hash, font.contentHash());
    expectShape(font);
    font.clearCache();
    font.clearPersistentCache();
    font.releaseResidentCaches();
    EXPECT_EQ(view, font.getEpdFont()->getThaiShape());
    EXPECT_EQ(hash, font.contentHash());
    expectShape(font);
  }
}

TEST(SdCardFontTest, FontSizeReloadsPreserveRecipesAcrossBackingTransitions) {
  ShapeTestScope guard;
  const auto liveBefore = nullableLiveBytes;
  {
    SdCardFont font;
    uint32_t previousHash = 0;
    constexpr uint8_t sizes[] = {16, 18, 20, 26, 16};
    for (size_t i = 0; i < std::size(sizes); ++i) {
      makeFont();
      makeShape();
      const uint8_t size = sizes[i];
      sdFontTestFile[44] = size;
      put16(45, size);
      shapePut16(44 + 20, size);
      shapePut16(44 + 24, size);
      shapePut16(44 + 28 + ThaiShapeView::DENSE_COUNT * 4 + 2, size << 4);
      updateShapeCrc();
      probe::internalHeadroomBytes = i == 2 || i == 3 ? 45 * 1024 : guard.headroom;
      ASSERT_TRUE(font.load("fixture.cpfont"));
      const auto* shape = font.getEpdFont()->getThaiShape();
      ASSERT_NE(nullptr, shape);
      EXPECT_EQ(size, shape->lineAdvance());
      EXPECT_NE(previousHash, font.contentHash());
      previousHash = font.contentHash();
      ThaiGlyphCursor cursor;
      ASSERT_TRUE(cursor.begin("กี่", *shape));
      ThaiGlyphPlacement placement;
      ASSERT_TRUE(cursor.next(placement));
      EXPECT_EQ(FIRST, placement.codepoint);
      EXPECT_EQ(size << 4, placement.advanceFP);
      EXPECT_FALSE(cursor.next(placement));
      EXPECT_EQ(i == 2 || i == 3 ? 1u : 0u, sdFontTestShapeOpenHandles);
    }
  }
  EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  EXPECT_EQ(liveBefore, nullableLiveBytes);
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

TEST(SdCardFontTest, ResidentPostallocationReserveFallsThroughBeforeCachedAllocation) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  probe::setInternalHeapSamples({{100 * 1024, 100 * 1024}, {50 * 1024, 16 * 1024}});
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(1u, sdFontTestShapeOpenHandles);
  for (const auto& allocation : nullableAllocations) {
    if (allocation.pointer) EXPECT_NE(sdFontTestCompanion.size(), allocation.size);
  }
  expectShape(font);
}

TEST(SdCardFontTest, SdBackedLoadSkipsResidentBufferAndIndexesDespiteHeadroom) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont", /*residentThaiShape=*/false));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(1u, sdFontTestShapeOpenHandles);
  constexpr size_t indexBytes = ThaiShapeView::DENSE_COUNT * 4;
  for (const auto& allocation : nullableAllocations) {
    if (!allocation.pointer) continue;
    EXPECT_NE(sdFontTestCompanion.size(), allocation.size);
    EXPECT_NE(indexBytes, allocation.size);
  }
  expectShape(font);
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  expectShape(font);
}

TEST(SdCardFontTest, MinimumCacheOomIsNativeOnlyLeakFreeAndReloadRecovers) {
  ShapeTestScope guard;
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto nativeHash = font.contentHash();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  failNextScalarSize = sizeof(ThaiShapeCache);
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, failNextScalarSize);
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_FALSE(font.hasThaiShapeError());
  EXPECT_EQ(nativeHash, font.contentHash());
  EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  ASSERT_TRUE(font.load("fixture.cpfont"));
  expectShape(font);
  EXPECT_NE(nativeHash, font.contentHash());
}

TEST(SdCardFontTest, FourDistinctStylesPublishAtomicallyInEveryBackingMode) {
  ShapeTestScope guard;
  makeFourStyleShape();
  SdCardFont font;
  uint32_t hash = 0;
  std::vector<ThaiGlyphPlacement> resident[4];
  for (auto& placements : resident) placements.reserve(ThaiShapeView::DENSE_COUNT);
  for (int mode = 0; mode < 3; ++mode) {
    probe::internalHeadroomBytes = mode == 2 ? 45 * 1024 : guard.headroom;
    if (mode == 1) failNextArraySize = sdFontTestCompanion.size();
    ASSERT_TRUE(font.load("fixture.cpfont"));
    if (mode == 0) hash = font.contentHash();
    EXPECT_EQ(hash, font.contentHash());
    for (uint8_t style = 0; style < 4; ++style) {
      const auto* view = font.getEpdFont(style)->getThaiShape();
      ASSERT_NE(nullptr, view);
      EXPECT_EQ(32 + style, view->ascender());
      EXPECT_EQ(-style, view->descender());
      EXPECT_EQ(32 + style, view->lineAdvance());
      expectShape(font, style, FIRST + style * 5);
      for (uint32_t key = 0; key < ThaiShapeView::DENSE_COUNT; ++key) {
        const auto text = shapeKeyText(key);
        ThaiGlyphCursor cursor;
        ASSERT_TRUE(cursor.begin(text, *view)) << key;
        ThaiGlyphPlacement placement;
        ASSERT_TRUE(cursor.next(placement)) << key;
        EXPECT_EQ(FIRST + style * 4 + (key + style) % 4, placement.codepoint);
        EXPECT_EQ((32 + style) << 4, placement.advanceFP);
        EXPECT_EQ(-style * 16, placement.xOffsetFP);
        EXPECT_EQ(style * 16, placement.yOffsetFP);
        if (mode == 0) {
          resident[style].push_back(placement);
        } else {
          const auto& expected = resident[style][key];
          EXPECT_EQ(expected.codepoint, placement.codepoint);
          EXPECT_EQ(expected.advanceFP, placement.advanceFP);
          EXPECT_EQ(expected.xOffsetFP, placement.xOffsetFP);
          EXPECT_EQ(expected.yOffsetFP, placement.yOffsetFP);
          EXPECT_EQ(expected.flags, placement.flags);
          EXPECT_EQ(expected.sourceBegin, placement.sourceBegin);
          EXPECT_EQ(expected.sourceEnd, placement.sourceEnd);
        }
        EXPECT_EQ(text.size(), cursor.consumedBytes());
        EXPECT_FALSE(cursor.next(placement));
      }
    }
  }
  // Corrupt only the final style, retaining a correct payload CRC.
  constexpr size_t styleSize = 28 + ThaiShapeView::DENSE_COUNT * 4 + 32 + 8 + 1;
  shapePut16(80 + 3 * styleSize + 28, 4);
  shapePut32(20, mz_crc32(MZ_CRC32_INIT, sdFontTestCompanion.data() + 32, sdFontTestCompanion.size() - 32));
  for (const size_t headroom : {guard.headroom, size_t{45 * 1024}}) {
    probe::internalHeadroomBytes = headroom;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    for (uint8_t style = 0; style < 4; ++style) EXPECT_EQ(nullptr, font.getEpdFont(style)->getThaiShape());
    EXPECT_NE(hash, font.contentHash());
    EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  }
}

TEST(SdCardFontTest, OptionalIndexesAreAllOrNoneOnOomAndPostallocationReserve) {
  ShapeTestScope guard;
  makeFourStyleShape();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto hash = font.contentHash();
  constexpr size_t indexBytes = ThaiShapeView::DENSE_COUNT * 4;
  for (int scenario = 0; scenario < 3; ++scenario) {
    // The scripted initial sample skips residency but allows all four indexes.
    if (scenario < 2) {
      probe::setInternalHeapSamples({{45 * 1024, 16 * 1024}, {256 * 1024, 16 * 1024}});
      failNextArraySize = indexBytes;
      failArrayOrdinal = scenario == 0 ? 1 : 4;
    } else {
      probe::setInternalHeapSamples(
          {{45 * 1024, 16 * 1024}, {256 * 1024, 32 * 1024}, {100 * 1024, 32 * 1024}, {50 * 1024, 32 * 1024}});
    }
    ASSERT_TRUE(font.load("fixture.cpfont"));
    EXPECT_EQ(0u, failNextArraySize);
    EXPECT_EQ(hash, font.contentHash());
    for (const auto& allocation : nullableAllocations) {
      if (allocation.pointer) EXPECT_NE(indexBytes, allocation.size);
    }
    for (uint8_t style = 0; style < 4; ++style) expectShape(font, style, FIRST + style * 5);
  }
}

TEST(SdCardFontTest, SharedCacheHasOneAllocationAndStagesOutliveUnloadedOwners) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  const auto allocationsBefore = cacheAllocationCalls;
  const auto liveBefore = nullableLiveBytes;
  auto first = std::make_unique<SdCardFont>();
  auto second = std::make_unique<SdCardFont>();
  ASSERT_TRUE(first->load("fixture.cpfont"));
  ASSERT_TRUE(second->load("fixture.cpfont"));
  EXPECT_EQ(allocationsBefore + 1, cacheAllocationCalls);
  EXPECT_EQ(2u, sdFontTestShapeOpenHandles);
  ThaiGlyphCursor firstCursor, secondCursor;
  ASSERT_TRUE(firstCursor.begin("\xE0\xB8\x81", *first->getEpdFont()->getThaiShape()));
  ASSERT_TRUE(secondCursor.begin("\xE0\xB8\x81", *second->getEpdFont()->getThaiShape()));
  first.reset();
  second->releaseResidentCaches();
  second.reset();
  EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
  EXPECT_EQ(liveBefore + sizeof(ThaiShapeCache), nullableLiveBytes);
  ThaiGlyphPlacement placement;
  ASSERT_TRUE(firstCursor.next(placement));
  EXPECT_EQ(FIRST, placement.codepoint);
  EXPECT_EQ(liveBefore + sizeof(ThaiShapeCache), nullableLiveBytes);
  ASSERT_TRUE(secondCursor.next(placement));
  EXPECT_EQ(FIRST, placement.codepoint);
  EXPECT_EQ(liveBefore, nullableLiveBytes);
  EXPECT_FALSE(firstCursor.next(placement));
  EXPECT_FALSE(secondCursor.next(placement));
}

TEST(SdCardFontTest, WarmClustersAndOrdinaryCacheMissesDoNotAllocate) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  font.releaseResidentCaches();
  const auto allocations = nullableCalls;
  const auto coldReads = sdFontTestShapeReads;
  expectShape(font);
  ASSERT_GT(sdFontTestShapeReads, coldReads);
  const auto warmReads = sdFontTestShapeReads;
  const auto warmBytes = sdFontTestShapeBytes;
  for (int i = 0; i < 20; ++i) expectShape(font);
  EXPECT_EQ(warmReads, sdFontTestShapeReads);
  EXPECT_EQ(warmBytes, sdFontTestShapeBytes);
  font.clearCache();
  font.clearPersistentCache();
  expectShape(font);
  EXPECT_EQ(warmReads, sdFontTestShapeReads);
  font.releaseResidentCaches();
  expectShape(font);
  EXPECT_GT(sdFontTestShapeReads, warmReads);
  EXPECT_EQ(allocations, nullableCalls);
}

TEST(SdCardFontTest, CachedReadFaultsLatchWithoutRetriesOrFallbackCachePublication) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  SdCardFont font;
  for (int failure = 0; failure < 5; ++failure) {
    SCOPED_TRACE(failure);
    ASSERT_TRUE(font.load("fixture.cpfont"));
    ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
    EXPECT_FALSE(font.hasThaiShapeError());
    const auto hash = font.contentHash();
    font.releaseResidentCaches();
    switch (failure) {
      case 0:
        sdFontTestFailShapeSeekAt = sdFontTestShapeSeeks + 1;
        break;
      case 1:
        sdFontTestFailShapeReadAt = sdFontTestShapeReads + 1;
        break;
      case 2:
        sdFontTestZeroShapeReadAt = sdFontTestShapeReads + 1;
        break;
      case 3:
        sdFontTestShortShapeReadAt = sdFontTestShapeReads + 1;
        break;
      case 4:
        sdFontTestShapeEof = 100;
        break;
    }
    EXPECT_EQ(-1, font.prewarm("\xE0\xB8\x81", 1));
    EXPECT_TRUE(font.hasThaiShapeError());
    EXPECT_EQ(hash, font.contentHash());
    EXPECT_FALSE(font.hasAdvanceTable());
    const auto reads = sdFontTestShapeReads;
    const auto seeks = sdFontTestShapeSeeks;
    EXPECT_EQ(-1, font.buildAdvanceTable("\xE0\xB8\x81", 1));
    EXPECT_EQ(-1, font.prewarm("native", 1));
    font.clearCache();
    font.clearPersistentCache();
    font.releaseResidentCaches();
    EXPECT_TRUE(font.hasThaiShapeError());
    EXPECT_EQ(reads, sdFontTestShapeReads);
    EXPECT_EQ(seeks, sdFontTestShapeSeeks);
    sdFontTestResetShapeFaults();
  }
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_FALSE(font.hasThaiShapeError());
  expectShape(font);
}

TEST(SdCardFontTest, ValidationReadFailureNeverPublishesAnActiveStyle) {
  ShapeTestScope guard;
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto nativeHash = font.contentHash();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  // Header succeeds; the first cached block fill fails.
  sdFontTestShortShapeReadAt = sdFontTestShapeReads + 2;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(nativeHash, font.contentHash());
  EXPECT_FALSE(font.hasThaiShapeError());
  EXPECT_EQ(0u, sdFontTestShapeOpenHandles);
}

TEST(SdCardFontTest, FullyPagedAndIndexedLookupDifferOnlyInDenseHalRequests) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  SdCardFont font;
  size_t requests[2] = {};
  for (int indexed = 0; indexed < 2; ++indexed) {
    probe::internalHeadroomBytes = indexed ? guard.headroom : 45 * 1024;
    if (indexed) failNextArraySize = sdFontTestCompanion.size();
    ASSERT_TRUE(font.load("fixture.cpfont"));
    font.releaseResidentCaches();
    const auto before = sdFontTestShapeReads;
    expectShape(font);
    requests[indexed] = sdFontTestShapeReads - before;
  }
  // This fixture's base/offset/count records fit one final block; key zero is
  // in block zero. A copied dense index removes precisely that cold HAL read.
  EXPECT_EQ(2u, requests[0]);
  EXPECT_EQ(1u, requests[1]);
}

TEST(SdCardFontTest, CachedStyleFallbackUsesThePublishedRegularShape) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto* shape = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, shape);
  for (uint8_t style = 0; style < 4; ++style) {
    EXPECT_EQ(0, font.resolveStyle(style));
    EXPECT_EQ(1, font.resolveStyleMask(1u << style));
  }
  EXPECT_EQ(0, font.prewarm("\xE0\xB8\x81", 0x0F, false, false));
  EXPECT_EQ(shape, font.getEpdFont()->getThaiShape());
  EXPECT_FALSE(font.hasThaiShapeError());
}

TEST(SdCardFontTest, AdvanceCollectionFaultsRejectBothPackedSegmentsAndExtraText) {
  ShapeTestScope guard;
  makeFont();
  makeShape();
  probe::internalHeadroomBytes = 45 * 1024;
  SdCardFont font;
  for (const bool extra : {false, true}) {
    ASSERT_TRUE(font.load("fixture.cpfont"));
    font.releaseResidentCaches();
    const char* segment = extra ? "A" : "\xE0\xB8\x81";
    const size_t length = std::strlen(segment) + 1;
    sdFontTestFailShapeReadAt = sdFontTestShapeReads + 1;
    EXPECT_EQ(-1,
              font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, extra ? "\xE0\xB8\x81" : nullptr));
    EXPECT_TRUE(font.hasThaiShapeError());
    EXPECT_FALSE(font.hasAdvanceTable());
    sdFontTestResetShapeFaults();
  }
}

TEST(SdCardFontTest, SmallExactPackedMetricsFitSmallAllocations) {
  ShapeTestScope guard;
  makeKerningFont(0, 2, 2, true, true);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  maximumNullableAllocation = 4096;
  const char first[] = "A\0A\0";
  const char second[] = "D\0D\0";
  const char* segments[] = {first, second};
  const size_t lengths[] = {sizeof(first), sizeof(second)};
  ASSERT_EQ(0, font.buildAdvanceTablePacked(segments, lengths, 2, true, true, 1, "BC", true));
  for (const auto cp : {' ', '-', 'A', 'B', 'C', 'D'}) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance(cp, 0, advance));
    const int index = cp == ' ' ? 0 : cp == '-' ? 1 : cp - 'A' + 2;
    EXPECT_EQ((10 + index) << 4, advance);
  }
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
}

TEST(SdCardFontTest, RepeatedRawAndShapedAlphabetsDoNotSaturateMetricWorkspace) {
  ShapeTestScope guard;
  for (const bool shaped : {false, true}) {
    SCOPED_TRACE(shaped);
    makeKerningFont(0, 2, 2, true, true);
    if (shaped) makeShape(14, 'C');
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    maximumNullableAllocation = 4096;
    std::string text;
    for (int i = 0; i < 5000; ++i) text += shaped ? "\xE0\xB8\x81" : "CD";
    const char* segment = text.c_str();
    const size_t length = text.size();
    ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, true, true, 1, "D", true));
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance('C', 0, advance));
    EXPECT_EQ(14 << 4, advance);
    ASSERT_TRUE(font.getAdvance('D', 0, advance));
    EXPECT_EQ(15 << 4, advance);
    if (shaped) {
      ASSERT_TRUE(font.getAdvance(0xE01, 0, advance));
      EXPECT_EQ(18 << 4, advance);  // Raw fallback is retained alongside recipe output.
      ThaiGlyphCursor cursor;
      ASSERT_TRUE(cursor.begin("\xE0\xB8\x81", *font.getEpdFont()->getThaiShape()));
      ThaiGlyphPlacement placement{};
      ASSERT_TRUE(cursor.next(placement));
      EXPECT_EQ(static_cast<uint32_t>('C'), placement.codepoint);
      EXPECT_EQ(32 << 4, placement.advanceFP);
    }
    EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
    maximumNullableAllocation = guard.allocationLimit;
  }
}

TEST(SdCardFontTest, ConservativeSaturationAcrossStylesIsNotUniqueOverflow) {
  ShapeTestScope guard;
  makeMetricRangeFont(64, true);
  duplicateNativeStyles();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto alphabet = page(0x4E00, 64);
  for (const int copies : {1, 150}) {
    SCOPED_TRACE(copies);
    font.releaseResidentCaches();
    std::string text;
    for (int i = 0; i < copies; ++i) text += alphabet;
    const char* segment = text.c_str();
    const size_t length = text.size();
    ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, true, true, 0x0F, nullptr, true));
    for (uint8_t style = 0; style < 4; ++style) {
      for (uint32_t i = 0; i < 64; ++i) {
        uint16_t advance = 0;
        ASSERT_TRUE(font.getAdvance(0x4E00 + i, style, advance));
        EXPECT_EQ((10 + (i + 2) % 17) << 4, advance);
      }
    }
  }
}

TEST(SdCardFontTest, ExactMainCapReservesSeparatorsAndRejectsOnlyTheNextIdentity) {
  ShapeTestScope guard;
  makeMetricRangeFont(4097);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  for (uint32_t count : {4096u, 4097u}) {
    SCOPED_TRACE(count);
    font.releaseResidentCaches();
    const auto text = page(0x4E00, count);
    const char* segment = text.c_str();
    const size_t length = text.size();
    const int result = font.buildAdvanceTablePacked(&segment, &length, 1, true, true, 1, nullptr, true);
    if (count == 4097) {
      EXPECT_EQ(-1, result);
      EXPECT_FALSE(font.hasAdvanceTable());  // No partial metric publication.
      continue;
    }
    ASSERT_EQ(0, result);
    for (const uint32_t i : {0u, 4095u}) {
      const auto* glyph = font.getEpdFont()->getGlyph(0x4E00 + i);
      ASSERT_NE(nullptr, glyph);
      EXPECT_EQ((10 + (i + 2) % 17) << 4, glyph->advanceX);
    }
    for (const char separator : {' ', '-'}) {
      const auto* glyph = font.getEpdFont()->getGlyph(separator);
      ASSERT_NE(nullptr, glyph);
      EXPECT_EQ((separator == ' ' ? 10 : 11) << 4, glyph->advanceX);
    }
  }
}

TEST(SdCardFontTest, ReverseDependencyLigaturesUseRequestMembershipAcrossStyles) {
  ShapeTestScope guard;
  makeKerningFont(0, 2, 2, true, true);
  duplicateNativeStyles();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_EQ(0, font.buildAdvanceTable("AF", 0x0F));  // Stale A/F must not close the next E/B request.
  const char* unrelated = "EB";
  const size_t unrelatedLength = 2;
  ASSERT_EQ(0, font.buildAdvanceTablePacked(&unrelated, &unrelatedLength, 1, false, false, 0x0F, "D", true));
  for (uint8_t style = 0; style < 4; ++style) {
    uint16_t advance = 0;
    EXPECT_FALSE(font.getAdvance('C', style, advance));
    EXPECT_EQ(0, font.getEpdFont(style)->getKerning('C', 'D'));
  }
  // Separate words prevent native emission from doing the chain for closure.
  const char packed[] = "E\0F\0B\0";
  const char* segment = packed;
  const size_t length = sizeof(packed);
  ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 0x0F, "D", true));
  for (uint8_t style = 0; style < 4; ++style) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance('C', style, advance));
    EXPECT_EQ(14 << 4, advance);
    EXPECT_EQ(-5, font.getEpdFont(style)->getKerning('C', 'D'));
    const char* suffix = "FB";
    EXPECT_EQ(static_cast<uint32_t>('C'), font.getEpdFont(style)->applyLigatures('E', suffix));
    EXPECT_EQ('\0', *suffix);
  }
}

TEST(SdCardFontTest, MixedMetricsMatchRenderPrewarmAfterCacheRelease) {
  ShapeTestScope guard;
  makeKerningFont(0, 2, 2, true, true);
  makeShape(14, 'C');
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  const char* words[] = {"\xE0\xB8\x81", "D", "EF", "\xE4\xB8\x80", "B"};
  const size_t lengths[] = {3, 1, 2, 3, 1};
  constexpr uint32_t codepoints[] = {0xE01, 'C', 'D', 'E', 'F', 'A', 0x4E00, 'B', ' ', '-'};
  uint16_t cold[std::size(codepoints)] = {};
  for (int pass = 0; pass < 3; ++pass) {
    SCOPED_TRACE(pass);
    if (pass != 1) font.releaseResidentCaches();
    ASSERT_EQ(0, font.buildAdvanceTablePacked(words, lengths, std::size(words), true, true, 1, nullptr, true));
    for (size_t i = 0; i < std::size(codepoints); ++i) {
      uint16_t advance = 0;
      ASSERT_TRUE(font.getAdvance(codepoints[i], 0, advance));
      if (pass == 0) cold[i] = advance;
      EXPECT_EQ(cold[i], advance);
    }
    EXPECT_EQ(18 << 4, cold[0]);                             // Native fallback source.
    EXPECT_EQ(14 << 4, cold[1]);                             // Recipe/closure output.
    EXPECT_EQ(22 << 4, cold[6]);                             // CJK glyph.
    EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));  // Across packed words.
    EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
    const char* suffix = "F";
    EXPECT_EQ(static_cast<uint32_t>('A'), font.getEpdFont()->applyLigatures('E', suffix));
    ThaiGlyphCursor cursor;
    ASSERT_TRUE(cursor.begin(words[0], *font.getEpdFont()->getThaiShape()));
    ThaiGlyphPlacement placement{};
    ASSERT_TRUE(cursor.next(placement));
    EXPECT_EQ(static_cast<uint32_t>('C'), placement.codepoint);
    EXPECT_EQ(32 << 4, placement.advanceFP);
    EXPECT_EQ(0, placement.xOffsetFP);
    EXPECT_EQ(0, placement.yOffsetFP);
    EXPECT_FALSE(cursor.next(placement));
    ASSERT_EQ(0, font.prewarm("\xE0\xB8\x81 D EF \xE4\xB8\x80 B-", 1, false, true, false));
    for (size_t i = 0; i < std::size(codepoints); ++i) {
      const auto* glyph = font.getEpdFont()->getGlyph(codepoints[i]);
      ASSERT_NE(nullptr, glyph);
      EXPECT_EQ(cold[i], glyph->advanceX);
    }
    EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
  }
}

TEST(SdCardFontTest, EmptyExactRequestResetsKerningWithoutDiscardingCachedAdvances) {
  ShapeTestScope guard;
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const char* segment = "AB";
  const size_t length = 2;
  ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, nullptr, true));
  ASSERT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
  ASSERT_EQ(0, font.buildAdvanceTablePacked(nullptr, nullptr, 0, false, false, 1, nullptr, true));
  EXPECT_EQ(0, font.getEpdFont()->getKerning('A', 'B'));
  uint16_t advance = 0;
  ASSERT_TRUE(font.getAdvance('A', 0, advance));
  EXPECT_EQ(32 << 4, advance);
}

TEST(SdCardFontTest, MetricWorkspaceOomDoesNotPoisonLaterRequests) {
  ShapeTestScope guard;
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  // Load kern/ligature tables first so the denied allocation is the workspace.
  ASSERT_EQ(0, font.buildAdvanceTablePacked(nullptr, nullptr, 0, false, false, 1, nullptr, true));
  const char* segment = "AB";
  const size_t length = 2;
  maximumNullableAllocation = 0;
  EXPECT_EQ(-1, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, nullptr, true));
  EXPECT_FALSE(font.hasAdvanceTable());
  EXPECT_FALSE(font.hasThaiShapeError());
  maximumNullableAllocation = 4096;
  ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, nullptr, true));
  uint16_t advance = 0;
  ASSERT_TRUE(font.getAdvance('A', 0, advance));
  EXPECT_EQ(32 << 4, advance);
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
}

TEST(SdCardFontTest, SizingShapeReadFailurePreventsMetricPublicationAndReloadRecovers) {
  ShapeTestScope guard;
  makeKerningFont(0, 2, 2, true, true);
  makeShape(14, 'C');
  probe::internalHeadroomBytes = 45 * 1024;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_NE(nullptr, font.getEpdFont()->getThaiShape());
  font.releaseResidentCaches();
  sdFontTestFailShapeReadAt = sdFontTestShapeReads + 1;
  const char* segment = "\xE0\xB8\x81";
  const size_t length = 3;
  EXPECT_EQ(-1, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, "D", true));
  EXPECT_TRUE(font.hasThaiShapeError());
  EXPECT_FALSE(font.hasAdvanceTable());
  EXPECT_EQ(0, font.getEpdFont()->getKerning('C', 'D'));
  sdFontTestResetShapeFaults();
  ASSERT_TRUE(font.load("fixture.cpfont"));
  ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, "D", true));
  EXPECT_FALSE(font.hasThaiShapeError());
  uint16_t advance = 0;
  ASSERT_TRUE(font.getAdvance('C', 0, advance));
  EXPECT_EQ(14 << 4, advance);
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));
}

TEST(SdCardFontTest, RedrawingAPrewarmedPageReadsNothing) {
  makeKerningFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  for (const char* text : {"ABCD", "DEF"}) {  // with and without kerning pairs
    font.clearCache();
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    font.clearCache();
    sdFontTestReads = 0;
    ASSERT_EQ(0, font.prewarm(text, 1, false, true, false));
    EXPECT_EQ(0U, sdFontTestReads) << text;
  }
}

TEST(SdCardFontTest, LaterPagesReadOnlyTheKernClassBlocksTheyUse) {
  makeKerningFont(300);  // five 64-entry blocks per class table
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  const size_t firstReads = sdFontTestReads;
  EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));

  font.releaseResidentCaches();
  sdFontTestReads = 0;
  ASSERT_EQ(0, font.prewarm("ABCDEF", 1, false, true, false));
  EXPECT_EQ(firstReads - 8, sdFontTestReads);  // 4 of the 5 blocks skipped in each table
  const EpdFont* epd = font.getEpdFont();
  EXPECT_EQ(-3, epd->getKerning('A', 'B'));
  EXPECT_EQ(4, epd->getKerning('C', 'B'));
  EXPECT_EQ(-5, epd->getKerning('C', 'D'));
}

TEST(SdCardFontTest, AdvancesStayCorrectWhenPagesAddCodepointsOutOfOrder) {
  makeFont();
  for (uint32_t i = 0; i < GLYPHS; ++i) {
    put16(64 + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (20 + i % 13) << 4);
  }
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  std::vector<uint32_t> added;
  for (uint32_t block : {3U, 0U, 5U, 1U, 4U, 2U}) {  // each merge lands before, between or after earlier ones
    ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST + block * 80, 80).c_str(), 1));
    for (uint32_t cp = FIRST + block * 80; cp < FIRST + block * 80 + 80; ++cp) added.push_back(cp);
    for (uint32_t cp : added) {
      uint16_t advance = 0;
      ASSERT_TRUE(font.getAdvance(cp, 0, advance)) << std::hex << cp;
      ASSERT_EQ((20 + (cp - FIRST) % 13) << 4, advance) << std::hex << cp;
    }
  }
}

TEST(SdCardFontTest, AdvanceMergesPastTheCapKeepTheLowestCodepoints) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  constexpr uint32_t GLYPH_COUNT = 1001;
  makeFont(GLYPH_COUNT);
  for (uint32_t i = 0; i < GLYPH_COUNT; ++i) {
    put16(64 + 24 + i * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (20 + i % 13) << 4);
  }
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // Reference: the sorted union of every page, truncated to the cap; a full table takes nothing more.
  std::vector<uint32_t> expected;
  for (uint32_t block : {6U, 1U, 9U, 3U, 0U, 7U, 4U, 8U, 2U, 5U}) {
    ASSERT_GE(font.buildAdvanceTable(page(FIRST + block * 100, 100).c_str(), 1), 0);
    if (expected.size() < CACHE_LIMIT) {
      for (uint32_t cp = FIRST + block * 100; cp < FIRST + block * 100 + 100; ++cp) expected.push_back(cp);
      std::sort(expected.begin(), expected.end());
      if (expected.size() > CACHE_LIMIT) expected.resize(CACHE_LIMIT);
    }
    for (uint32_t cp = FIRST; cp < FIRST + 1000; ++cp) {
      const bool cached = std::binary_search(expected.begin(), expected.end(), cp);
      uint16_t advance = 0;
      ASSERT_EQ(cached, font.getAdvance(cp, 0, advance)) << block << " " << std::hex << cp;
      if (cached) ASSERT_EQ((20 + (cp - FIRST) % 13) << 4, advance) << block << " " << std::hex << cp;
    }
  }
}

TEST(SdCardFontTest, AnEmptyOrFailedAdvanceBuildLeavesNoTable) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  font.buildAdvanceTable("", 1);
  EXPECT_FALSE(font.hasAdvanceTable());
  failNextArraySize = (10 + 2) * sizeof(uint32_t);  // the codepoint scratch, sized to the text
  EXPECT_EQ(-1, font.buildAdvanceTable(page(FIRST, 10).c_str(), 1));
  EXPECT_EQ(0U, failNextArraySize);
  EXPECT_FALSE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 10).c_str(), 1));
  EXPECT_TRUE(font.hasAdvanceTable());
}
