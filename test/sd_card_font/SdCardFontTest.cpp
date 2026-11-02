#include <HalMemory.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace {
size_t failNextArraySize = 0;
size_t failNextScalarSize = 0;
size_t failArrayOrdinal = 1;
size_t failScalarOrdinal = 1;
size_t nullableCalls = 0;
size_t allAllocationCalls = 0;
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

constexpr size_t THAI_METRICS_BYTES = ThaiShapeView::GLYPH_COUNT * sizeof(ThaiGlyphMetrics);
static_assert(THAI_METRICS_BYTES == 504, "Thai metadata must stay bounded");

void makeThaiFont() {
  makeFont(79);  // U+0E01..U+0E4E and U+FFFD, including native gaps ignored by shaping.
  put32(64, 0xE01);
  put32(68, 0xE4E);
  for (uint32_t cp = 0xE01; cp <= 0xE4E; ++cp) {
    if (ThaiShapeView::glyphIndex(cp) >= 46 && cp != 0xE32)
      put16(88 + (cp - 0xE01) * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), 0);
  }
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
    sdFontTestFailReadOffset = static_cast<size_t>(-1);
    EXPECT_EQ(liveBytes, nullableLiveBytes);
    EXPECT_EQ(liveCount, nullableLiveCount);
    EXPECT_EQ(0u, sdFontTestOpenHandles);
  }
};

void expectShape(SdCardFont& font, uint8_t style = 0) {
  const auto* view = font.getEpdFont(style)->getThaiShape();
  ASSERT_NE(nullptr, view);
  const auto allocations = allAllocationCalls;
  const auto reads = sdFontTestReads;
  ThaiGlyphCursor cursor;
  ThaiGlyphPlacement placements[3]{};
  const bool admitted = cursor.begin("กี่", *view);
  const bool placed = cursor.next(placements[0]) && cursor.next(placements[1]) && cursor.next(placements[2]);
  const bool ended = !cursor.next(placements[2]);
  const auto allocationsAfter = allAllocationCalls;
  ASSERT_TRUE(admitted);
  ASSERT_TRUE(placed);
  EXPECT_TRUE(ended);
  EXPECT_EQ(allocations, allocationsAfter);
  EXPECT_EQ(reads, sdFontTestReads);
  EXPECT_EQ(0xE01u, placements[0].codepoint);
  EXPECT_EQ(0xE35u, placements[1].codepoint);
  EXPECT_EQ(0xE48u, placements[2].codepoint);
  EXPECT_EQ(32 << 4, placements[0].advanceFP);
  EXPECT_EQ(0, placements[1].advanceFP);
  EXPECT_EQ(0, placements[2].advanceFP);
  EXPECT_EQ(std::strlen("กี่"), cursor.consumedBytes());
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

TEST(SdCardFontTest, NativeThaiMetricsNeedOneBoundedSnapshotAndNoCompanionOrRetainedFile) {
  ShapeTestScope guard;
  makeThaiFont();
  probe::internalHeadroomBytes = 512;
  const auto opens = sdFontTestOpenCalls;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(opens + 1, sdFontTestOpenCalls);
  EXPECT_EQ(0u, sdFontTestOpenHandles);
  size_t snapshots = 0;
  for (const auto& allocation : nullableAllocations)
    if (allocation.pointer && allocation.size == THAI_METRICS_BYTES) ++snapshots;
  EXPECT_EQ(1u, snapshots);
  const auto* view = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, view);
  for (uint32_t cp = 0xE01; cp <= 0xE4E; ++cp) {
    const auto* metric = view->glyph(cp);
    if (ThaiShapeView::glyphIndex(cp) < 0) {
      EXPECT_EQ(nullptr, metric);
      continue;
    }
    ASSERT_NE(nullptr, metric);
    EXPECT_EQ(32, metric->width);
    EXPECT_EQ(32, metric->height);
    EXPECT_EQ(0, metric->left);
    EXPECT_EQ(32, metric->top);
    EXPECT_EQ(ThaiShapeView::glyphIndex(cp) >= 46 && cp != 0xE32 ? 0 : 32 << 4, metric->advanceX);
  }
  expectShape(font);
}

TEST(SdCardFontTest, NonThaiAndMarksOnlyCoverageAllocateNoSnapshot) {
  ShapeTestScope guard;
  for (const bool marksOnly : {false, true}) {
    makeFont(9);
    if (marksOnly) {
      put32(64, 0xE47);
      put32(68, 0xE4E);
    }
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
    for (const auto& allocation : nullableAllocations)
      if (allocation.pointer) EXPECT_NE(THAI_METRICS_BYTES, allocation.size);
  }
}

TEST(SdCardFontTest, SnapshotStaysPublishedAcrossPrewarmAndEveryCacheTeardown) {
  ShapeTestScope guard;
  makeThaiFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto* view = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, view);
  const auto hash = font.contentHash();
  ASSERT_EQ(0, font.prewarm("\xE0\xB8\x81", 1, false, false));
  EXPECT_EQ(view, font.getEpdFont()->getThaiShape());
  font.clearCache();
  font.clearPersistentCache();
  font.releaseResidentCaches();
  EXPECT_EQ(view, font.getEpdFont()->getThaiShape());
  EXPECT_EQ(hash, font.contentHash());
  expectShape(font);
  ASSERT_EQ(0, font.prewarm("\xE0\xB8\x81", 0x0F, true, false));
  EXPECT_EQ(view, font.getEpdFont()->getThaiShape());
  for (uint8_t style = 0; style < 4; ++style) {
    EXPECT_EQ(0, font.resolveStyle(style));
    EXPECT_EQ(1, font.resolveStyleMask(1u << style));
  }
}

TEST(SdCardFontTest, SnapshotAllocationFailureLeavesNativeFontUsableAndReloadRecovers) {
  ShapeTestScope guard;
  makeThaiFont();
  SdCardFont font;
  failNextArraySize = THAI_METRICS_BYTES;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, failNextArraySize);
  EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
  const auto nativeHash = font.contentHash();
  ASSERT_EQ(0, font.prewarm("\xE0\xB8\x81", 1, false, false));
  ASSERT_NE(nullptr, font.getEpdFont()->getGlyph(0xE01));
  ASSERT_TRUE(font.load("fixture.cpfont"));
  expectShape(font);
  EXPECT_NE(nativeHash, font.contentHash());
}

TEST(SdCardFontTest, IncompleteOrCorruptMetricReadsNeverPublishPartialSnapshots) {
  ShapeTestScope guard;
  for (const bool corrupt : {false, true}) {
    makeThaiFont();
    const size_t lastMetric = 88 + (0xE4E - 0xE01) * sizeof(EpdGlyph);
    if (corrupt) {
      put16(lastMetric + offsetof(EpdGlyph, dataLength), BITMAP_BYTES + 1);
    } else {
      sdFontTestFailReadOffset = lastMetric;
    }
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    EXPECT_EQ(nullptr, font.getEpdFont()->getThaiShape());
    for (const auto& allocation : nullableAllocations)
      if (allocation.pointer) EXPECT_NE(THAI_METRICS_BYTES, allocation.size);
    sdFontTestFailReadOffset = static_cast<size_t>(-1);
    ASSERT_EQ(0, font.prewarm("\xE0\xB8\x81", 1, false, false));
    ASSERT_NE(nullptr, font.getEpdFont()->getGlyph(0xE01));
  }
}

TEST(SdCardFontTest, EachStyleOwnsItsSnapshotAndOneFailureDoesNotPublishIncompleteData) {
  ShapeTestScope guard;
  makeThaiFont();
  duplicateNativeStyles();
  SdCardFont font;
  failNextArraySize = THAI_METRICS_BYTES;
  failArrayOrdinal = 2;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_EQ(0u, failNextArraySize);
  EXPECT_EQ(nullptr, font.getEpdFont(1)->getThaiShape());
  for (const uint8_t style : {0, 2, 3}) expectShape(font, style);
  size_t snapshots = 0;
  for (const auto& allocation : nullableAllocations)
    if (allocation.pointer && allocation.size == THAI_METRICS_BYTES) ++snapshots;
  EXPECT_EQ(3u, snapshots);
  ASSERT_TRUE(font.load("fixture.cpfont"));
  for (uint8_t style = 0; style < 4; ++style) expectShape(font, style);
}

TEST(SdCardFontTest, NativeMetricEditsChangeShapingIdentityWithoutCompanionRebuilds) {
  ShapeTestScope guard;
  makeThaiFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto hash = font.contentHash();
  put16(88 + offsetof(EpdGlyph, advanceX), 17 << 4);
  ASSERT_TRUE(font.load("fixture.cpfont"));
  EXPECT_NE(hash, font.contentHash());
  const auto* view = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, view);
  ASSERT_NE(nullptr, view->glyph(0xE01));
  EXPECT_EQ(17 << 4, view->glyph(0xE01)->advanceX);
}

TEST(SdCardFontTest, MissingNativeMarksKeepTheWholeClusterOnTheNativePath) {
  ShapeTestScope guard;
  makeKerningFont(0, 2, 2, true, true);  // Thai bases only, no Thai marks.
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture.cpfont"));
  const auto* view = font.getEpdFont()->getThaiShape();
  ASSERT_NE(nullptr, view);
  EXPECT_EQ(nullptr, view->glyph(0xE35));
  ThaiGlyphCursor cursor;
  EXPECT_FALSE(cursor.begin("กี่", *view));
  EXPECT_EQ(std::strlen("กี่"), cursor.consumedBytes());
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
  makeKerningFont(0, 2, 2, true, true);
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

TEST(SdCardFontTest, RepeatedLatinAndNativeThaiAlphabetsDoNotSaturateMetricWorkspace) {
  ShapeTestScope guard;
  for (const bool shaped : {false, true}) {
    SCOPED_TRACE(shaped);
    makeKerningFont(0, 2, 2, true, true);
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture.cpfont"));
    maximumNullableAllocation = 4096;
    std::string text;
    for (int i = 0; i < 5000; ++i) text += shaped ? "\xE0\xB8\x81" : "CD";
    const char* segment = text.c_str();
    const size_t length = text.size();
    ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, true, true, 1, "CD", true));
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance('C', 0, advance));
    EXPECT_EQ(14 << 4, advance);
    ASSERT_TRUE(font.getAdvance('D', 0, advance));
    EXPECT_EQ(15 << 4, advance);
    if (shaped) {
      ASSERT_TRUE(font.getAdvance(0xE01, 0, advance));
      EXPECT_EQ(18 << 4, advance);
      ThaiGlyphCursor cursor;
      EXPECT_FALSE(cursor.begin("\xE0\xB8\x81", *font.getEpdFont()->getThaiShape()));
      EXPECT_EQ(3u, cursor.consumedBytes());  // Unmarked bases preserve native shaping.
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
    EXPECT_EQ(18 << 4, cold[0]);                             // Native Thai metrics.
    EXPECT_EQ(14 << 4, cold[1]);                             // Ligature closure output.
    EXPECT_EQ(22 << 4, cold[6]);                             // CJK glyph.
    EXPECT_EQ(-5, font.getEpdFont()->getKerning('C', 'D'));  // Across packed words.
    EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
    const char* suffix = "F";
    EXPECT_EQ(static_cast<uint32_t>('A'), font.getEpdFont()->applyLigatures('E', suffix));
    ThaiGlyphCursor cursor;
    EXPECT_FALSE(cursor.begin(words[0], *font.getEpdFont()->getThaiShape()));
    EXPECT_EQ(3u, cursor.consumedBytes());  // Unmarked bases need no positioned cluster.
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
  maximumNullableAllocation = 4096;
  ASSERT_EQ(0, font.buildAdvanceTablePacked(&segment, &length, 1, false, false, 1, nullptr, true));
  uint16_t advance = 0;
  ASSERT_TRUE(font.getAdvance('A', 0, advance));
  EXPECT_EQ(32 << 4, advance);
  EXPECT_EQ(-3, font.getEpdFont()->getKerning('A', 'B'));
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
  const auto text = page(FIRST, 20);
  failNextArraySize = (text.size() + 2) * sizeof(uint32_t);  // Beyond the inline scratch.
  EXPECT_EQ(-1, font.buildAdvanceTable(text.c_str(), 1));
  EXPECT_EQ(0U, failNextArraySize);
  EXPECT_FALSE(font.hasAdvanceTable());
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  EXPECT_TRUE(font.hasAdvanceTable());
}

TEST(SdCardFontTest, AdvancesBuildForAStyleStoredInAnotherSlot) {
  // An italic-only font: style 2 lives in slot 0 of the compact style array.
  makeFont();
  sdFontTestFile[32] = 2;
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_TRUE(font.hasStyle(2));
  ASSERT_FALSE(font.hasStyle(0));
  const auto text = page(FIRST, 100);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1 << 2));
  for (uint32_t cp = FIRST; cp < FIRST + 100; ++cp) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance(cp, 2, advance));
    EXPECT_EQ(32 << 4, advance);
  }
}

TEST(SdCardFontTest, AFullAdvanceTableSkipsTheScanWithoutAllocating) {
  makeFont(1001);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.buildAdvanceTable(page(FIRST, 768).c_str(), 1));
  uint16_t advance = 0;
  ASSERT_TRUE(font.getAdvance(FIRST + 767, 0, advance));
  ASSERT_EQ(32U << 4, advance);
  const size_t allocationsBefore = nullableCalls;
  EXPECT_EQ(0, font.buildAdvanceTable(page(FIRST + 768, 200).c_str(), 1));
  EXPECT_EQ(allocationsBefore, nullableCalls);
  EXPECT_FALSE(font.getAdvance(FIRST + 768, 0, advance));
}

TEST(SdCardFontTest, OnePageWithMoreCodepointsThanTheTableKeepsTheLowestInAnyOrder) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  makeFont(1001);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // The top 768 first (ascending), then the rest descending: whatever the
  // input order, only the lowest 768 stay cached. Every codepoint appears
  // twice, so repeats must not count twice.
  std::string text;
  for (uint32_t cp = FIRST + 232; cp < FIRST + 1000; ++cp) text += page(cp, 1) + page(cp, 1);
  for (uint32_t cp = FIRST + 231; cp + 1 > FIRST; --cp) text += page(cp, 1) + page(cp, 1);
  ASSERT_EQ(0, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + 1000; ++cp) {
    uint16_t advance = 0;
    ASSERT_EQ(cp < FIRST + CACHE_LIMIT, font.getAdvance(cp, 0, advance)) << std::hex << cp;
    if (cp < FIRST + CACHE_LIMIT) ASSERT_EQ(32U << 4, advance) << std::hex << cp;
  }
}

TEST(SdCardFontTest, CodepointsTheFontLacksDoNotTakeTableSlotsWithoutAReplacementGlyph) {
  constexpr uint32_t CACHE_LIMIT = 768;  // SdCardFont::ADVANCE_CACHE_LIMIT
  makeFont(1001);
  put32(76, 0xFFFC);  // the last glyph is U+FFFC instead of U+FFFD
  put32(80, 0xFFFC);
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  // 300 codepoints below FIRST that the font lacks, each twice, then a full
  // table's worth it has. Only the latter can be cached, so all should be.
  const std::string lacking = page(0x4E00, 300);
  const std::string text = lacking + lacking + page(FIRST, CACHE_LIMIT);
  EXPECT_EQ(300, font.buildAdvanceTable(text.c_str(), 1));
  for (uint32_t cp = FIRST; cp < FIRST + CACHE_LIMIT; ++cp) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance(cp, 0, advance)) << std::hex << cp;
    ASSERT_EQ(32U << 4, advance) << std::hex << cp;
  }
}

TEST(SdCardFontTest, ShortAdvancePreparationAvoidsLargeScratchAndRepeatedReads) {
  makeFont();
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  const std::string text = page(FIRST, 3);
  failNextArraySize = (4096 + 2) * sizeof(uint32_t);
  const int result = font.buildAdvanceTable(text.c_str(), 1);
  failNextArraySize = 0;
  ASSERT_EQ(result, 0);
  for (uint32_t cp = FIRST; cp < FIRST + 3; ++cp) {
    uint16_t advance = 0;
    ASSERT_TRUE(font.getAdvance(cp, 0, advance));
    EXPECT_EQ(32 << 4, advance);
  }
  sdFontTestReads = 0;
  ASSERT_EQ(font.buildAdvanceTable(text.c_str(), 1), 0);
  EXPECT_EQ(sdFontTestReads, 0u);
}

TEST(SdCardFontTest, SmallCapsPrepareOriginalAndUppercasePackedMetricsWithoutBitmaps) {
  const struct {
    uint32_t first;
    uint32_t last;
    const char* words;
    size_t bytes;
  } cases[] = {
      {0x20, 0x7F, "ab\0z", 5}, {0x0391, 0x03CB, "\xCE\xB1\0\xCE\xB2", 6}, {0x0410, 0x044F, "\xD0\xB0\0\xD1\x8F", 6}};
  for (const auto& fixture : cases) {
    SCOPED_TRACE(fixture.first);
    makeFont(fixture.last - fixture.first + 2);
    put32(64, fixture.first);
    put32(68, fixture.last);
    for (uint32_t cp = fixture.first; cp <= fixture.last; ++cp) {
      put16(64 + 24 + (cp - fixture.first) * sizeof(EpdGlyph) + offsetof(EpdGlyph, advanceX), (10 + cp - fixture.first)
                                                                                                  << 4);
    }
    SdCardFont font;
    ASSERT_TRUE(font.load("fixture"));
    const char* segments[] = {fixture.words};
    const size_t lengths[] = {fixture.bytes};
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1), 0);
    auto* cursor = reinterpret_cast<const uint8_t*>(fixture.words);
    uint16_t advance = 0;
    EXPECT_FALSE(font.getAdvance(utf8SmallCapsUpper(utf8NextCodepoint(&cursor)), 0, advance));
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1, nullptr, false, true), 0);
    cursor = reinterpret_cast<const uint8_t*>(fixture.words);
    const auto* end = cursor + fixture.bytes;
    while (cursor < end) {
      if (*cursor == 0) {
        ++cursor;
        continue;
      }
      const uint32_t cp = utf8NextCodepoint(&cursor);
      ASSERT_TRUE(font.getAdvance(cp, 0, advance));
      EXPECT_EQ(advance, (10 + cp - fixture.first) << 4);
      const uint32_t upper = utf8SmallCapsUpper(cp);
      ASSERT_TRUE(font.getAdvance(upper, 0, advance));
      EXPECT_EQ(advance, (10 + upper - fixture.first) << 4);
    }
    EXPECT_EQ(font.getStats().bitmapBytes, 0u);
    sdFontTestReads = 0;
    ASSERT_EQ(font.buildAdvanceTablePacked(segments, lengths, 1, false, false, 1, nullptr, false, true), 0);
    EXPECT_EQ(sdFontTestReads, 0u);
  }
}

TEST(SdCardFontTest, FontsWithTheSameCoverageShareOneIntervalTable) {
  makeFont();
  size_t firstLoad = 0;
  size_t secondLoad = 0;
  auto first = std::make_unique<SdCardFont>();
  {
    const size_t before = nullableCalls;
    ASSERT_TRUE(first->load("fixture"));
    firstLoad = nullableCalls - before;
  }
  SdCardFont second;
  {
    const size_t before = nullableCalls;
    ASSERT_TRUE(second.load("fixture"));
    secondLoad = nullableCalls - before;
  }
  EXPECT_EQ(firstLoad - 1, secondLoad);

  // The table outlives the font that loaded it while another font still uses it.
  first.reset();
  EXPECT_TRUE(second.getEpdFont()->hasCodepoint(FIRST));
  EXPECT_TRUE(second.getEpdFont()->hasCodepoint(0xFFFD));
  EXPECT_FALSE(second.getEpdFont()->hasCodepoint(FIRST - 1));
}

TEST(SdCardFontTest, ACorruptGlyphEntryDrawsAsMissing) {
  makeFont();
  // Glyph 3 claims more bytes than its pixels need, as a corrupt .cpfont does.
  constexpr size_t GLYPH_OFFSET = 64 + 24;
  EpdGlyph glyph{};
  std::memcpy(&glyph, sdFontTestFile.data() + GLYPH_OFFSET + 3 * sizeof(glyph), sizeof(glyph));
  glyph.dataLength = 49335;
  std::memcpy(sdFontTestFile.data() + GLYPH_OFFSET + 3 * sizeof(glyph), &glyph, sizeof(glyph));
  SdCardFont font;
  ASSERT_TRUE(font.load("fixture"));
  ASSERT_EQ(0, font.prewarm(page(FIRST, 5).c_str(), 1, false, false, false));
  const auto* data = font.getEpdFont()->data;
  for (uint32_t cp = FIRST; cp < FIRST + 5; ++cp) {
    const EpdGlyph* found = nullptr;
    for (uint32_t i = 0; i < data->intervalCount; ++i) {
      const auto& interval = data->intervals[i];
      if (cp >= interval.first && cp <= interval.last) found = data->glyph + interval.offset + cp - interval.first;
    }
    ASSERT_NE(nullptr, found);
    if (cp == FIRST + 3) {
      EXPECT_EQ(0, found->dataLength);
      EXPECT_EQ(0, found->width);
    } else {
      EXPECT_EQ(BITMAP_BYTES, found->dataLength);
    }
  }
}
