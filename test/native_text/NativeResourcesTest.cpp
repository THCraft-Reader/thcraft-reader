#include <NativeAllocator.h>
#include <datrie/trie.h>
#include <ft2build.h>
#include <gtest/gtest.h>
#include <thai/thbrk.h>
#include <thai/thwchar.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb-ft.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

namespace {
std::vector<uint8_t> readBytes(const char* path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const auto length = file.tellg();
  if (length <= 0) return {};
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) return {};
  return bytes;
}

class NativeTextResources : public testing::Test {
 protected:
  size_t initialUsed = 0;
  void SetUp() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    // HarfBuzz's immutable function tables and language tags live until exit.
    hb_language_from_string("th", -1);
    hb_unicode_funcs_get_default();
    const auto fontBytes = readBytes(NATIVE_FONT_PATH);
    ASSERT_FALSE(fontBytes.empty());
    FT_Library library = nullptr;
    ASSERT_EQ(FT_Init_FreeType(&library), 0);
    FT_Face face = nullptr;
    ASSERT_EQ(FT_New_Memory_Face(library, fontBytes.data(), static_cast<FT_Long>(fontBytes.size()), 0, &face), 0);
    hb_font_destroy(hb_ft_font_create_referenced(face));
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    initialUsed = native_text::allocationStats().used;
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    EXPECT_EQ(native_text::allocationStats().used, initialUsed);
  }
};

TEST_F(NativeTextResources, FailedGrowthPreservesTheOriginalBytes) {
  auto* memory = static_cast<uint8_t*>(native_text_malloc(128));
  ASSERT_NE(memory, nullptr);
  for (size_t i = 0; i < 128; ++i) memory[i] = static_cast<uint8_t>(i);
  const auto before = native_text::allocationStats();
  native_text::failAllocationsAfter(0);
  EXPECT_EQ(native_text_realloc(memory, 4096), nullptr);
  EXPECT_EQ(native_text::allocationStats().used, before.used);
  EXPECT_GT(native_text::allocationStats().failures, before.failures);
  for (size_t i = 0; i < 128; ++i) EXPECT_EQ(memory[i], static_cast<uint8_t>(i));
  native_text_free(memory);
}

TEST_F(NativeTextResources, CombinedBudgetAndOverflowFailWithoutAllocating) {
  const auto before = native_text::allocationStats();
  EXPECT_EQ(native_text_malloc(native_text::MEMORY_LIMIT), nullptr);
  EXPECT_EQ(native_text_calloc(std::numeric_limits<size_t>::max(), 2), nullptr);
  void* first = native_text_malloc(3 * 1024 * 1024);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(native_text_malloc(2 * 1024 * 1024), nullptr);
  EXPECT_LE(native_text::allocationStats().peak, native_text::MEMORY_LIMIT);
  native_text_free(first);
  EXPECT_EQ(native_text::allocationStats().used, before.used);
}

TEST_F(NativeTextResources, MemoryDictionaryMatchesUpstreamFileBreaker) {
  const auto bytes = readBytes(NATIVE_DICTIONARY_PATH);
  ASSERT_FALSE(bytes.empty());
  std::unique_ptr<ThBrk, decltype(&th_brk_delete)> memory(th_brk_new_from_memory(bytes.data(), bytes.size()),
                                                          th_brk_delete);
  std::unique_ptr<ThBrk, decltype(&th_brk_delete)> file(th_brk_new(NATIVE_DICTIONARY_PATH), th_brk_delete);
  ASSERT_NE(memory, nullptr);
  ASSERT_NE(file, nullptr);
  const char32_t* samples[] = {
      U"ภาษาไทย", U"ประเทศไทย",   U"ภาษาไทย EPUB 123 กี่", U"๑๒๓ภาษาไทย", U"กิ กี่ กึ กุ กู้ น้ำ กำ ปี่ ญู ฐู เก่ง",
      U"",        U"กขฃคฅฆงจฉชซฌ"};
  for (const auto* sample : samples) {
    std::array<thchar_t, 256> tis{};
    size_t length = 0;
    while (sample[length]) {
      ASSERT_LT(length, tis.size() - 1);
      tis[length] = th_uni2tis(sample[length]);
      ++length;
    }
    std::array<int, 256> a{}, b{};
    const int na = th_brk_find_breaks(memory.get(), tis.data(), a.data(), a.size());
    const int nb = th_brk_find_breaks(file.get(), tis.data(), b.data(), b.size());
    ASSERT_EQ(na, nb);
    for (int i = 0; i < na; ++i) EXPECT_EQ(a[i], b[i]);
  }
}

TEST_F(NativeTextResources, TruncatedDictionaryIsRejectedWithoutLeaks) {
  const auto bytes = readBytes(NATIVE_DICTIONARY_PATH);
  ASSERT_GT(bytes.size(), 128u);
  const size_t baseline = native_text::allocationStats().used;
  for (size_t length = 0; length < 64; ++length) {
    Trie* trie = trie_new_from_memory(bytes.data(), length);
    EXPECT_EQ(trie, nullptr) << length;
    if (trie) trie_free(trie);
    EXPECT_EQ(native_text::allocationStats().used, baseline);
  }
  for (const auto length : {bytes.size() / 2, bytes.size() - 1}) {
    Trie* trie = trie_new_from_memory(bytes.data(), length);
    EXPECT_EQ(trie, nullptr);
    if (trie) trie_free(trie);
    EXPECT_EQ(native_text::allocationStats().used, baseline);
  }
}

TEST_F(NativeTextResources, DictionaryAllocationFailureIsNotAnEmptySuccess) {
  const auto bytes = readBytes(NATIVE_DICTIONARY_PATH);
  ASSERT_FALSE(bytes.empty());
  for (size_t count = 0; count < 16; ++count) {
    const size_t baseline = native_text::allocationStats().used;
    native_text::failAllocationsAfter(count);
    const size_t failures = native_text::allocationStats().failures;
    ThBrk* breaker = th_brk_new_from_memory(bytes.data(), bytes.size());
    if (native_text::allocationStats().failures != failures) EXPECT_EQ(breaker, nullptr);
    if (breaker) th_brk_delete(breaker);
    EXPECT_EQ(native_text::allocationStats().used, baseline);
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  }
}

TEST_F(NativeTextResources, PinnedThaiFontShapesMarksWithoutMissingGlyphs) {
  const auto bytes = readBytes(NATIVE_FONT_PATH);
  ASSERT_FALSE(bytes.empty());
  FT_Library library = nullptr;
  ASSERT_EQ(FT_Init_FreeType(&library), 0);
  FT_Face face = nullptr;
  ASSERT_EQ(FT_New_Memory_Face(library, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &face), 0);
  ASSERT_EQ(FT_Set_Char_Size(face, 0, 14 * 64, 150, 150), 0);
  hb_font_t* font = hb_ft_font_create_referenced(face);
  hb_ft_font_set_load_flags(font, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL);
  hb_buffer_t* buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, "กิ กี่ กึ กุ กู้ น้ำ กำ ปี่ ญู ฐู เก่ง", -1, 0, -1);
  hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
  hb_buffer_set_script(buffer, HB_SCRIPT_THAI);
  hb_buffer_set_language(buffer, hb_language_from_string("th", -1));
  hb_shape(font, buffer, nullptr, 0);
  unsigned count = 0;
  const auto* glyphs = hb_buffer_get_glyph_infos(buffer, &count);
  ASSERT_GT(count, 0u);
  for (unsigned i = 0; i < count; ++i) EXPECT_NE(glyphs[i].codepoint, 0u);
  hb_buffer_destroy(buffer);
  hb_font_destroy(font);
  FT_Done_Face(face);
  FT_Done_FreeType(library);
}

TEST_F(NativeTextResources, NonFontBytesCannotOpenAsAnOutlineFace) {
  constexpr uint8_t invalid[] = {'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't'};
  FT_Library library = nullptr;
  ASSERT_EQ(FT_Init_FreeType(&library), 0);
  FT_Face face = nullptr;
  EXPECT_NE(FT_New_Memory_Face(library, invalid, sizeof(invalid), 0, &face), 0);
  EXPECT_EQ(face, nullptr);
  if (face) FT_Done_Face(face);
  FT_Done_FreeType(library);
}

TEST_F(NativeTextResources, ThaiSubsetRetainsShapingAndPositioning) {
  const auto original = readBytes(NATIVE_FONT_PATH);
  const auto subset = readBytes(NATIVE_SUBSET_FONT_PATH);
  ASSERT_FALSE(original.empty());
  ASSERT_FALSE(subset.empty());
  FT_Library library = nullptr;
  ASSERT_EQ(FT_Init_FreeType(&library), 0);
  FT_Face faces[2] = {nullptr, nullptr};
  const auto first = FT_New_Memory_Face(library, original.data(), static_cast<FT_Long>(original.size()), 0, &faces[0]);
  const auto second = FT_New_Memory_Face(library, subset.data(), static_cast<FT_Long>(subset.size()), 0, &faces[1]);
  EXPECT_EQ(first, 0);
  EXPECT_EQ(second, 0);
  if (!first && !second) {
    for (int pointSize : {12, 14, 16, 18}) {
      for (const char* text : {"กิ กี่ กึ กุ กู้ น้ำ กำ ปี่ ญู ฐู เก่ง", "น้ํา กํา"}) {
        hb_font_t* fonts[2];
        hb_buffer_t* buffers[2];
        for (int i = 0; i < 2; ++i) {
          EXPECT_EQ(FT_Set_Char_Size(faces[i], 0, pointSize * 64, 150, 150), 0);
          fonts[i] = hb_ft_font_create_referenced(faces[i]);
          hb_ft_font_set_load_flags(fonts[i], FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL);
          buffers[i] = hb_buffer_create();
          hb_buffer_add_utf8(buffers[i], text, -1, 0, -1);
          hb_buffer_set_direction(buffers[i], HB_DIRECTION_LTR);
          hb_buffer_set_script(buffers[i], HB_SCRIPT_THAI);
          hb_buffer_set_language(buffers[i], hb_language_from_string("th", -1));
          hb_shape(fonts[i], buffers[i], nullptr, 0);
        }
        const unsigned count = hb_buffer_get_length(buffers[0]);
        EXPECT_EQ(count, hb_buffer_get_length(buffers[1]));
        if (count == hb_buffer_get_length(buffers[1])) {
          const auto* left = hb_buffer_get_glyph_infos(buffers[0], nullptr);
          const auto* right = hb_buffer_get_glyph_infos(buffers[1], nullptr);
          const auto* lp = hb_buffer_get_glyph_positions(buffers[0], nullptr);
          const auto* rp = hb_buffer_get_glyph_positions(buffers[1], nullptr);
          for (unsigned i = 0; i < count; ++i) {
            char leftName[128]{}, rightName[128]{};
            EXPECT_EQ(FT_Get_Glyph_Name(faces[0], left[i].codepoint, leftName, sizeof(leftName)), 0);
            EXPECT_EQ(FT_Get_Glyph_Name(faces[1], right[i].codepoint, rightName, sizeof(rightName)), 0);
            EXPECT_STREQ(leftName, rightName);
            EXPECT_EQ(left[i].cluster, right[i].cluster);
            EXPECT_EQ(lp[i].x_advance, rp[i].x_advance);
            EXPECT_EQ(lp[i].y_advance, rp[i].y_advance);
            EXPECT_EQ(lp[i].x_offset, rp[i].x_offset);
            EXPECT_EQ(lp[i].y_offset, rp[i].y_offset);
          }
        }
        for (int i = 0; i < 2; ++i) {
          hb_buffer_destroy(buffers[i]);
          hb_font_destroy(fonts[i]);
        }
      }
    }
  }
  for (auto face : faces) {
    if (face) FT_Done_Face(face);
  }
  FT_Done_FreeType(library);
}

TEST_F(NativeTextResources, CorruptDictionaryCountsCannotDriveUnboundedAllocation) {
  const auto original = readBytes(NATIVE_DICTIONARY_PATH);
  ASSERT_GT(original.size(), 32u);
  const size_t baseline = native_text::allocationStats().used;
  for (size_t countOffset : {4u, 20u}) {
    auto corrupt = original;
    for (size_t i = 0; i < 4; ++i) corrupt[countOffset + i] = 0xff;
    Trie* trie = trie_new_from_memory(corrupt.data(), corrupt.size());
    EXPECT_EQ(trie, nullptr);
    if (trie) trie_free(trie);
    EXPECT_EQ(native_text::allocationStats().used, baseline);
  }
}

struct FreeTypeFace {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  ~FreeTypeFace() {
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
  }
  bool open(const std::vector<uint8_t>& bytes) {
    return FT_Init_FreeType(&library) == 0 &&
           FT_New_Memory_Face(library, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &face) == 0 &&
           FT_Set_Char_Size(face, 0, 14 * 64, 150, 150) == 0;
  }
};

struct GlyphPixels {
  unsigned width, rows;
  int left, top;
  std::vector<uint8_t> bytes;
  bool operator==(const GlyphPixels&) const = default;
};

GlyphPixels glyphPixels(FT_GlyphSlot slot) {
  const auto& bitmap = slot->bitmap;
  GlyphPixels result{bitmap.width, bitmap.rows, slot->bitmap_left, slot->bitmap_top, {}};
  result.bytes.reserve(static_cast<size_t>(bitmap.width) * bitmap.rows);
  for (unsigned row = 0; row < bitmap.rows; ++row) {
    const auto* data = bitmap.buffer + row * bitmap.pitch;
    result.bytes.insert(result.bytes.end(), data, data + bitmap.width);
  }
  return result;
}

TEST_F(NativeTextResources, FirstGlyphAllocationFailuresRecoverWithoutChangingPixelsOrLeaking) {
  const auto cffPath =
      std::filesystem::path(NATIVE_VARIABLE_FONT_PATH).parent_path() / "AdobeVFPrototype.abc.static.otf";
  const std::array<std::pair<std::string, FT_ULong>, 2> samples{{{NATIVE_FONT_PATH, 0x0e01}, {cffPath.string(), 'a'}}};
  for (size_t sample = 0; sample < samples.size(); ++sample) {
    const auto& [path, codepoint] = samples[sample];
    SCOPED_TRACE(path);
    const auto bytes = readBytes(path.c_str());
    ASSERT_FALSE(bytes.empty());
    const FT_Int32 flags =
        FT_LOAD_NO_BITMAP | FT_LOAD_RENDER | (sample == 0 ? FT_LOAD_FORCE_AUTOHINT : FT_LOAD_NO_AUTOHINT);
    GlyphPixels expected;
    {
      FreeTypeFace baseline;
      ASSERT_TRUE(baseline.open(bytes));
      ASSERT_EQ(FT_Load_Char(baseline.face, codepoint, flags), 0);
      expected = glyphPixels(baseline.face->glyph);
    }
    bool reachedSuccess = false;
    unsigned failuresChecked = 0;
    for (size_t failAt = 0; failAt < 256; ++failAt) {
      SCOPED_TRACE(failAt);
      const auto before = native_text::allocationStats().used;
      {
        FreeTypeFace attempt;
        ASSERT_TRUE(attempt.open(bytes));
        const auto failures = native_text::allocationStats().failures;
        native_text::failAllocationsAfter(failAt);
        const auto error = FT_Load_Char(attempt.face, codepoint, flags);
        native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
        if (native_text::allocationStats().failures != failures) {
          EXPECT_NE(error, 0);
          ++failuresChecked;
          ASSERT_EQ(FT_Load_Char(attempt.face, codepoint, flags), 0);
        } else {
          ASSERT_EQ(error, 0);
          reachedSuccess = true;
        }
        EXPECT_EQ(glyphPixels(attempt.face->glyph), expected);
      }
      EXPECT_EQ(native_text::allocationStats().used, before);
      if (reachedSuccess) break;
    }
    EXPECT_TRUE(reachedSuccess);
    EXPECT_GT(failuresChecked, 0u);
  }
}

struct SpanCapture {
  FT_Library library;
  std::vector<uint8_t> pixels = std::vector<uint8_t>(32 * 32);
  FT_Outline* nestedOutline = nullptr;
  SpanCapture* nestedCapture = nullptr;
  bool nestedCalled = false;
  FT_Error nestedError = 0;
};

void captureSpans(int y, int count, const FT_Span* spans, void* user);

FT_Error renderSpans(FT_Outline& outline, SpanCapture& capture) {
  FT_Raster_Params params{};
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = captureSpans;
  params.user = &capture;
  params.clip_box = {0, 0, 32, 32};
  return FT_Outline_Render(capture.library, &outline, &params);
}

void captureSpans(int y, int count, const FT_Span* spans, void* user) {
  auto& capture = *static_cast<SpanCapture*>(user);
  if (capture.nestedOutline && !capture.nestedCalled) {
    capture.nestedCalled = true;
    capture.nestedError = renderSpans(*capture.nestedOutline, *capture.nestedCapture);
  }
  for (int i = 0; i < count; ++i)
    for (unsigned x = spans[i].x; x < spans[i].x + spans[i].len; ++x) capture.pixels[y * 32 + x] = spans[i].coverage;
}

TEST_F(NativeTextResources, NestedDirectSpanRenderingPreservesBothOutlines) {
  const auto bytes = readBytes(NATIVE_FONT_PATH);
  FreeTypeFace font;
  ASSERT_TRUE(font.open(bytes));
  FT_Vector diamond[] = {{16 * 64, 64}, {31 * 64, 16 * 64}, {16 * 64, 31 * 64}, {64, 16 * 64}};
  FT_Vector square[] = {{2 * 64, 2 * 64}, {10 * 64, 2 * 64}, {10 * 64, 10 * 64}, {2 * 64, 10 * 64}};
  unsigned char tags[] = {FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON, FT_CURVE_TAG_ON};
  unsigned short contours[] = {3};
  FT_Outline outer{1, 4, diamond, tags, contours, 0};
  FT_Outline inner{1, 4, square, tags, contours, 0};
  SpanCapture expectedOuter{font.library}, expectedInner{font.library};
  ASSERT_EQ(renderSpans(outer, expectedOuter), 0);
  ASSERT_EQ(renderSpans(inner, expectedInner), 0);
  SpanCapture actualOuter{font.library}, actualInner{font.library};
  actualOuter.nestedOutline = &inner;
  actualOuter.nestedCapture = &actualInner;
  ASSERT_EQ(renderSpans(outer, actualOuter), 0);
  EXPECT_TRUE(actualOuter.nestedCalled);
  EXPECT_EQ(actualOuter.nestedError, 0);
  EXPECT_EQ(actualOuter.pixels, expectedOuter.pixels);
  EXPECT_EQ(actualInner.pixels, expectedInner.pixels);
}
}  // namespace
