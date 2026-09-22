#include <NativeAllocator.h>
#include <NativeFontRegistry.h>
#include <NativePlatform.h>
#include <fontIds.h>
#include <ft2build.h>
#include <gtest/gtest.h>
#include FT_MULTIPLE_MASTERS_H
#include <hb-ft.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr int CUSTOM_FONT = 71237;
constexpr int OTHER_CUSTOM_FONT = 71238;

class NativeTextFontRuntime : public testing::Test {
 protected:
  FT_Library library = nullptr;
  NativeFontRegistry registry;
  size_t initialUsed = 0;
  std::filesystem::path directory;

  static void SetUpTestSuite() {
    // HB's immutable function tables deliberately outlive all font registries.
    nativeTextLock();
    FT_Library warmLibrary = nullptr;
    ASSERT_EQ(FT_Init_FreeType(&warmLibrary), 0);
    NativeFontRegistry warm;
    ASSERT_EQ(warm.initialize(warmLibrary), TextStatus::Ok);
    ASSERT_EQ(warm.registerBuiltins(), TextStatus::Ok);
    warm.shutdown();
    FT_Done_FreeType(warmLibrary);
    nativeTextUnlock();
  }
  void SetUp() override {
    nativeTextLock();
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    initialUsed = native_text::allocationStats().used;
    ASSERT_EQ(FT_Init_FreeType(&library), 0);
    ASSERT_EQ(registry.initialize(library), TextStatus::Ok);
    ASSERT_EQ(registry.registerBuiltins(), TextStatus::Ok);
    directory = std::filesystem::temp_directory_path() /
                (std::string("native-font-runtime-") + testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(directory);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    registry.shutdown();
    if (library) FT_Done_FreeType(library);
    EXPECT_EQ(native_text::allocationStats().used, initialUsed);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    nativeTextUnlock();
  }
  NativeFaceChoice primary(int fontId, uint8_t style = 0, hb_script_t script = HB_SCRIPT_LATIN) {
    std::array<NativeFaceChoice, 8> choices;
    size_t count = 0;
    EXPECT_EQ(registry.candidates(fontId, style, script, choices, count), TextStatus::Ok);
    EXPECT_GT(count, 0u);
    return choices[0];
  }
  std::string copyFont(const char* name = "Family-Regular.ttf", const char* source = NATIVE_FONT_PATH) {
    const auto destination = directory / name;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    return destination.string();
  }
  TextStatus registerRegular(int id, const std::string& path, std::span<const NativeVariation> axes = {}) {
    const NativeFontFile file{path, 0, axes};
    return registry.registerCustomFont(id, 12, {&file, 1}, false);
  }
  uint64_t glyphPixels(NativeFaceHandle& handle, uint32_t cp) {
    const auto glyph = FT_Get_Char_Index(handle.face, cp);
    EXPECT_NE(glyph, 0u);
    EXPECT_EQ(FT_Load_Glyph(handle.face, glyph, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL), 0);
    EXPECT_EQ(FT_Render_Glyph(handle.face->glyph, FT_RENDER_MODE_NORMAL), 0);
    const auto& bitmap = handle.face->glyph->bitmap;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned y = 0; y < bitmap.rows; ++y) {
      const uint8_t* row = bitmap.buffer + static_cast<ptrdiff_t>(y) * bitmap.pitch;
      for (unsigned x = 0; x < bitmap.width; ++x) hash = (hash ^ row[x]) * UINT64_C(1099511628211);
    }
    return hash;
  }
};

TEST_F(NativeTextFontRuntime, OpensUtf8FontPathsWithoutUsingTheWindowsCodePage) {
  const auto destination = directory / std::filesystem::path(u8"ฟอนต์-Regular.ttf");
  std::filesystem::copy_file(NATIVE_FONT_PATH, destination);
  const auto encoded = destination.u8string();
  const std::string path(reinterpret_cast<const char*>(encoded.data()), encoded.size());
  ASSERT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::Ok);
  NativeFaceHandle face;
  ASSERT_EQ(registry.acquire(primary(CUSTOM_FONT), face), TextStatus::Ok);
  EXPECT_NE(FT_Get_Char_Index(face.face, 0x0e01), 0u);
  registry.release(face);
}

TEST_F(NativeTextFontRuntime, LogicalIdsPreserveFamiliesPointSizesAndThaiFallback) {
  const std::array<std::pair<int, unsigned>, 11> mappings{{{NOTOSANS_12_FONT_ID, 12},
                                                           {NOTOSANS_14_FONT_ID, 14},
                                                           {NOTOSANS_16_FONT_ID, 16},
                                                           {NOTOSANS_18_FONT_ID, 18},
                                                           {NOTOSERIF_12_FONT_ID, 12},
                                                           {NOTOSERIF_14_FONT_ID, 14},
                                                           {NOTOSERIF_16_FONT_ID, 16},
                                                           {NOTOSERIF_18_FONT_ID, 18},
                                                           {UI_10_FONT_ID, 10},
                                                           {UI_12_FONT_ID, 12},
                                                           {SMALL_FONT_ID, 8}}};
  for (const auto [id, points] : mappings) {
    const auto choice = primary(id);
    NativeFaceHandle handle;
    ASSERT_EQ(registry.acquire(choice, handle), TextStatus::Ok);
    EXPECT_EQ(handle.face->size->metrics.y_ppem, (points * 150 + 36) / 72);
    EXPECT_NE(FT_Get_Char_Index(handle.face, 'A'), 0u);
    registry.release(handle);
  }
  EXPECT_EQ(primary(NOTOSANS_12_FONT_ID).identity, primary(NOTOSANS_18_FONT_ID).identity);
  EXPECT_NE(primary(NOTOSANS_12_FONT_ID).identity, primary(NOTOSERIF_12_FONT_ID).identity);
  EXPECT_EQ(primary(NOTOSANS_12_FONT_ID, 16).rasterSize26, 6u * 64);
  EXPECT_EQ(primary(NOTOSANS_12_FONT_ID, 32).rasterSize26, 6u * 64);
  for (int id : {NOTOSANS_12_FONT_ID, NOTOSERIF_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID}) {
    std::array<NativeFaceChoice, 8> choices;
    size_t count = 0;
    ASSERT_EQ(registry.candidates(id, 3, HB_SCRIPT_THAI, choices, count), TextStatus::Ok);
    ASSERT_GE(count, 2u);
    NativeFaceHandle thai;
    ASSERT_EQ(registry.acquire(choices[1], thai), TextStatus::Ok);
    EXPECT_NE(FT_Get_Char_Index(thai.face, 0x0e01), 0u);
    EXPECT_NE(FT_Get_Char_Index(thai.face, 0x0e48), 0u);
    EXPECT_EQ(choices[1].syntheticStyle, 2);
    registry.release(thai);
    bool replacement = false;
    for (size_t i = 0; i < count; ++i) {
      NativeFaceHandle face;
      ASSERT_EQ(registry.acquire(choices[i], face), TextStatus::Ok);
      replacement |= FT_Get_Char_Index(face.face, 0xfffd) != 0;
      registry.release(face);
    }
    EXPECT_TRUE(replacement);
  }
  EXPECT_FALSE(registry.hasFont(0));
}

TEST_F(NativeTextFontRuntime, PinnedFacesSurviveEvictionAndCapacityRecoversAfterRelease) {
  auto choice = primary(NOTOSANS_12_FONT_ID);
  std::array<NativeFaceHandle, 24> handles;
  for (size_t i = 0; i < handles.size(); ++i) {
    choice.rasterSize26 = static_cast<uint32_t>(i + 8) * 64;
    ASSERT_EQ(registry.acquire(choice, handles[i]), TextStatus::Ok);
  }
  const auto lastPpem = handles.back().face->size->metrics.y_ppem;
  choice.rasterSize26 = 40 * 64;
  NativeFaceHandle extra;
  EXPECT_EQ(registry.acquire(choice, extra), TextStatus::CapacityExceeded);
  const auto pinnedPixels = glyphPixels(handles.back(), 'a');
  registry.evictUnusedFaces();
  EXPECT_EQ(glyphPixels(handles.back(), 'a'), pinnedPixels);
  registry.release(handles.front());
  ASSERT_EQ(registry.acquire(choice, extra), TextStatus::Ok);
  EXPECT_EQ(handles.back().face->size->metrics.y_ppem, lastPpem);
  EXPECT_EQ(registry.faceStatus(handles.back()), TextStatus::Ok);
  registry.release(extra);
  for (size_t i = 1; i < handles.size(); ++i) registry.release(handles[i]);
}

TEST_F(NativeTextFontRuntime, StreamedFontRemainsUsableUntilLastPinAndReopensAfterEviction) {
  const auto path = copyFont();
  ASSERT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::Ok);
  auto choice = primary(CUSTOM_FONT, 3, HB_SCRIPT_THAI);
  EXPECT_EQ(choice.syntheticStyle, 3);
  NativeFaceHandle first, second;
  ASSERT_EQ(registry.acquire(choice, first), TextStatus::Ok);
  ASSERT_EQ(registry.acquire(choice, second), TextStatus::Ok);
  const auto pixels = glyphPixels(first, 0x0e01);
  registry.releaseSdFaces();
  registry.release(first);
  EXPECT_EQ(glyphPixels(second, 0x0e01), pixels);
  EXPECT_EQ(registry.faceStatus(second), TextStatus::Ok);
  registry.release(second);
  NativeFaceHandle reopened;
  ASSERT_EQ(registry.acquire(choice, reopened), TextStatus::Ok);
  EXPECT_EQ(glyphPixels(reopened, 0x0e01), pixels);
  registry.release(reopened);
  registry.releaseSdFaces();
  ASSERT_TRUE(std::filesystem::remove(path));
  EXPECT_EQ(registry.acquire(choice, reopened), TextStatus::StorageError);
}

TEST_F(NativeTextFontRuntime, InvalidOptionalStyleCannotReplaceTheExistingFamily) {
  const auto path = copyFont();
  ASSERT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::Ok);
  const auto oldFingerprint = registry.fingerprint(CUSTOM_FONT);
  const auto oldChoice = primary(CUSTOM_FONT);
  const auto invalid = (directory / "Family-Bold.otf").string();
  {
    std::ofstream file(invalid, std::ios::binary);
    file << "OTTOthis is not a font";
  }
  const NativeFontFile files[] = {{path, 0, {}}, {invalid, 1, {}}};
  EXPECT_EQ(registry.registerCustomFont(CUSTOM_FONT, 18, files, true), TextStatus::InvalidFont);
  EXPECT_EQ(registry.fingerprint(CUSTOM_FONT), oldFingerprint);
  const auto retained = primary(CUSTOM_FONT);
  NativeFaceHandle handle;
  ASSERT_EQ(registry.acquire(retained, handle), TextStatus::Ok);
  EXPECT_EQ(retained.identity, oldChoice.identity);
  EXPECT_EQ(handle.face->size->metrics.y_ppem, 25);
  EXPECT_NE(FT_Get_Char_Index(handle.face, 0x0e01), 0u);
  registry.release(handle);
}

TEST_F(NativeTextFontRuntime, ForgedTableRangesAndDisallowedContainersAreRejected) {
  std::ifstream input(NATIVE_FONT_PATH, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  ASSERT_GT(bytes.size(), 28u);
  const auto path = (directory / "Bad-Regular.ttf").string();
  for (const std::array<char, 4> signature :
       {std::array<char, 4>{'t', 't', 'c', 'f'}, std::array<char, 4>{'w', 'O', 'F', 'F'},
        std::array<char, 4>{'w', 'O', 'F', '2'}}) {
    std::copy(signature.begin(), signature.end(), bytes.begin());
    {
      std::ofstream file(path, std::ios::binary);
      file.write(bytes.data(), bytes.size());
    }
    EXPECT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::InvalidFont);
    EXPECT_FALSE(registry.hasFont(CUSTOM_FONT));
  }
  bytes[0] = 0;
  bytes[1] = 1;
  bytes[2] = 0;
  bytes[3] = 0;
  for (size_t i = 20; i < 24; ++i) bytes[i] = static_cast<char>(0xff);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
  }
  EXPECT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::InvalidFont);
  EXPECT_FALSE(registry.hasFont(CUSTOM_FONT));
}

TEST_F(NativeTextFontRuntime, FontLargerThanMemoryBudgetUsesBoundedStreamStorage) {
  const auto path = copyFont();
  {
    std::ofstream append(path, std::ios::binary | std::ios::app);
    const std::array<char, 4096> zeros{};
    for (size_t i = 0; i < 2048; ++i) append.write(zeros.data(), zeros.size());
  }
  const size_t before = native_text::allocationStats().used;
  ASSERT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::Ok);
  NativeFaceHandle handle;
  ASSERT_EQ(registry.acquire(primary(CUSTOM_FONT), handle), TextStatus::Ok);
  EXPECT_LT(native_text::allocationStats().used - before, 256u * 1024);
  EXPECT_NE(FT_Get_Char_Index(handle.face, 0x0e01), 0u);
  registry.release(handle);
}

TEST_F(NativeTextFontRuntime, AllocationFailuresLeaveRegistrationAndOpenFilesUnchanged) {
  const auto path = copyFont();
  ASSERT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::Ok);
  const auto fingerprint = registry.fingerprint(CUSTOM_FONT);
  for (size_t failAt = 0; failAt < 4; ++failAt) {
    const size_t before = native_text::allocationStats().used;
    native_text::failAllocationsAfter(failAt);
    EXPECT_EQ(registerRegular(CUSTOM_FONT, path), TextStatus::OutOfMemory);
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    EXPECT_EQ(native_text::allocationStats().used, before);
    EXPECT_EQ(registry.fingerprint(CUSTOM_FONT), fingerprint);
  }
  registry.releaseSdFaces();
  ASSERT_TRUE(std::filesystem::remove(path));
}

TEST_F(NativeTextFontRuntime, VariableCoordinatesClampAndAffectRealGlyphsAndFingerprint) {
  const auto path = copyFont("Variable-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  FT_Face probe = nullptr;
  std::ifstream input(path, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(bytes.empty());
  ASSERT_EQ(FT_New_Memory_Face(library, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &probe), 0);
  FT_MM_Var* variation = nullptr;
  ASSERT_EQ(FT_Get_MM_Var(probe, &variation), 0);
  size_t weight = 0;
  while (weight < variation->num_axis && variation->axis[weight].tag != HB_TAG('w', 'g', 'h', 't')) ++weight;
  ASSERT_LT(weight, variation->num_axis);
  const float low = static_cast<float>(variation->axis[weight].minimum) / 65536.0f;
  const float high = static_cast<float>(variation->axis[weight].maximum) / 65536.0f;
  FT_Done_MM_Var(library, variation);
  FT_Done_Face(probe);
  const NativeVariation lowAxis{HB_TAG('w', 'g', 'h', 't'), low};
  const NativeVariation highAxis{HB_TAG('w', 'g', 'h', 't'), high};
  ASSERT_EQ(registerRegular(CUSTOM_FONT, path, {&lowAxis, 1}), TextStatus::Ok);
  ASSERT_EQ(registerRegular(OTHER_CUSTOM_FONT, path, {&highAxis, 1}), TextStatus::Ok);
  NativeFaceHandle light, heavy;
  ASSERT_EQ(registry.acquire(primary(CUSTOM_FONT), light), TextStatus::Ok);
  ASSERT_EQ(registry.acquire(primary(OTHER_CUSTOM_FONT), heavy), TextStatus::Ok);
  EXPECT_NE(glyphPixels(light, 'a'), glyphPixels(heavy, 'a'));
  EXPECT_NE(registry.fingerprint(CUSTOM_FONT), registry.fingerprint(OTHER_CUSTOM_FONT));
  registry.release(light);
  registry.release(heavy);
  const auto highIdentity = primary(OTHER_CUSTOM_FONT).identity;
  const auto highFingerprint = registry.fingerprint(OTHER_CUSTOM_FONT);
  const NativeVariation clamped{HB_TAG('w', 'g', 'h', 't'), high + 10000};
  ASSERT_EQ(registerRegular(OTHER_CUSTOM_FONT, path, {&clamped, 1}), TextStatus::Ok);
  EXPECT_EQ(primary(OTHER_CUSTOM_FONT).identity, highIdentity);
  EXPECT_EQ(registry.fingerprint(OTHER_CUSTOM_FONT), highFingerprint);
  const NativeVariation unknown{HB_TAG('n', 'o', 'p', 'e'), 1};
  EXPECT_EQ(registerRegular(OTHER_CUSTOM_FONT, path, {&unknown, 1}), TextStatus::InvalidFont);
  EXPECT_EQ(registry.fingerprint(OTHER_CUSTOM_FONT), highFingerprint);
}
}  // namespace
