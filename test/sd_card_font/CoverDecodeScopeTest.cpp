#include <CrossPointSettings.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <ReaderFontSizes.h>
#include <SdCardFont.h>
#include <SdCardFontSystem.h>
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_regular.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "HostAllocation.h"

// Persistence is the only application boundary adapted here. Mirror the normal
// clear operation so accidental calls really destroy the selected family/size.
void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  fontPointSize =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  saveToFile();
}

namespace {
namespace fs = std::filesystem;
fs::path smokeOutput;
constexpr char kFamily[] = "CoverLifecycle";
constexpr char kThai[] = "ภาษาไทย กำลังอ่าน ก่ี่ข";

TEST(CoverDecodeScopeCacheTest, ReleasesDecompressorPageAndHotBuffersWithoutSelectedFont) {
  // One stored DEFLATE block containing one eight-pixel glyph.
  static constexpr uint8_t compressed[] = {0x01, 0x01, 0x00, 0xFE, 0xFF, 0xFF};
  static constexpr EpdGlyph glyph{8, 1, 128, 0, 1, 1, 0};
  static constexpr EpdUnicodeInterval interval{'A', 'A', 0};
  static constexpr EpdFontGroup group{0, sizeof(compressed), 1, 1, 0};
  EpdFontData font{};
  font.bitmap = compressed;
  font.glyph = &glyph;
  font.intervals = &interval;
  font.intervalCount = 1;
  font.groups = &group;
  font.groupCount = 1;
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  FontDecompressor decompressor;
  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
  cache.setFontDecompressor(&decompressor);
  renderer.setFontCacheManager(&cache);
  SdCardFontSystem system;
  SETTINGS.sdFontFamilyName[0] = '\0';
  const size_t baseline = probe::allocationLive();
  const auto* bitmap = decompressor.getBitmap(&font, &glyph, 0);
  ASSERT_NE(bitmap, nullptr);
  EXPECT_EQ(bitmap[0], 0xFF);
  ASSERT_EQ(decompressor.prewarmCache(&font, "A"), 0);
  EXPECT_GT(probe::allocationLive(), baseline);
  size_t released = 0;
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    released = probe::allocationLive();
  }
  EXPECT_EQ(released, baseline);
  // Reusing the decompressor must fault the real bitmap back in, not access a
  // dangling page/hot-buffer pointer left behind by font suspension.
  bitmap = decompressor.getBitmap(&font, &glyph, 0);
  ASSERT_NE(bitmap, nullptr);
  EXPECT_EQ(bitmap[0], 0xFF);
}
constexpr int kUiIds[] = {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID};
static_assert(!std::is_copy_constructible_v<SdCardFontSystem::CoverDecodeScope>);
static_assert(!std::is_move_constructible_v<SdCardFontSystem::CoverDecodeScope>);

class CoverDecodeScopeTest : public testing::TestWithParam<bool> {
 protected:
  HalDisplay display{800, 480};
  GfxRenderer renderer{display};
  FontDecompressor decompressor;
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts()};
  SdCardFontSystem system;
  EpdFont small{&notosans_8_regular}, ui10{&ubuntu_10_regular}, ui12{&ubuntu_12_regular};
  fs::path root;

  fs::path fontPath(unsigned size) const {
    return root / "fonts" / kFamily / (std::string(kFamily) + "_" + std::to_string(size) + ".cpfont");
  }

  void SetUp() override {
    const char* asset = std::getenv("THAI_SHAPE_TEST_FONT");
    if (!asset || !*asset) {
      GTEST_SKIP() << "Run THAI_SHAPE_TEST_FONT=/path/to/paired.cpfont CoverDecodeScopeTest";
    }
    const fs::path font(asset);
    fs::path shape = font;
    shape.replace_extension(".cpshape");
    if (!fs::exists(font) || !fs::exists(shape)) {
      GTEST_SKIP()
          << "Optional paired asset absent; run THAI_SHAPE_TEST_FONT=/path/to/paired.cpfont CoverDecodeScopeTest";
    }
    root = fs::temp_directory_path() /
           ("cover-font-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "fonts" / kFamily);
    for (unsigned size : {8, 10, 12, 16}) {
      const auto path = fontPath(size);
      fs::copy_file(font, path);
      auto companion = path;
      companion.replace_extension(".cpshape");
      fs::copy_file(shape, companion);
    }
    probe::sdRoot = root;
    // Both real admission paths are exercised. This is simulated headroom,
    // not a host or device heap measurement, and does not replace allocation.
    probe::internalHeadroomBytes = GetParam() ? 32 * 1024 : 256 * 1024 * 1024;
    probe::resetInternalHeapSamples();
    probe::resetShapeFaults();
    renderer.begin();
    renderer.insertFont(SMALL_FONT_ID, EpdFontFamily(&small));
    renderer.insertFont(UI_10_FONT_ID, EpdFontFamily(&ui10));
    renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(&ui12));
    decompressor.init();
    cache.setFontDecompressor(&decompressor);
    renderer.setFontCacheManager(&cache);
    std::strcpy(SETTINGS.sdFontFamilyName, kFamily);
    SETTINGS.fontPointSize = 16;
    system.begin(renderer);
    ASSERT_EQ(renderer.getSdCardFonts().size(), 4u);
    for (const auto& entry : renderer.getFontMap()) {
      if (renderer.isSdCardFont(entry.first)) ASSERT_NE(entry.second.getThaiShape(), nullptr);
    }
    probe::settingsWrites = 0;
  }

  void TearDown() override {
    SETTINGS.sdFontFamilyName[0] = '\0';
    system.ensureLoaded(renderer);
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    renderer.setFontCacheManager(nullptr);
    probe::sdRoot.clear();
    probe::internalHeadroomBytes = 256 * 1024 * 1024;
    probe::resetInternalHeapSamples();
    probe::setNullableAllocationFailure();
    if (!root.empty()) fs::remove_all(root);
  }

  std::vector<uint8_t> thaiFrame() {
    renderer.clearScreen();
    for (unsigned i = 0; i < std::size(kUiIds); ++i) renderer.drawText(kUiIds[i], 24, 24 + i * 100, kThai);
    const auto* bytes = renderer.getFrameBuffer();
    return {bytes, bytes + 800 * 480 / 8};
  }

  void expectSelected() const {
    EXPECT_STREQ(SETTINGS.sdFontFamilyName, kFamily);
    EXPECT_EQ(SETTINGS.fontPointSize, 16);
    EXPECT_EQ(probe::settingsWrites, 0u);
  }

  void expectReleased() const {
    EXPECT_TRUE(renderer.getSdCardFonts().empty());
    EXPECT_TRUE(renderer.getTtfFonts().empty());
    EXPECT_EQ(renderer.getFontMap().size(), 3u);
    EXPECT_EQ(system.resolveFontId(kFamily, 16), 0);
  }

  void writeFrame(const char* name, const std::vector<uint8_t>& frame) {
    if (smokeOutput.empty()) return;
    fs::create_directories(smokeOutput);
    const std::string mode = GetParam() ? "cached-" : "resident-";
    std::ofstream output(smokeOutput / (mode + name), std::ios::binary);
    output << "P4\n800 480\n";
    // The display buffer uses white=1, PBM uses black=1.
    for (uint8_t byte : frame) output.put(static_cast<char>(~byte));
    ASSERT_TRUE(output.good());
  }
};

TEST_P(CoverDecodeScopeTest, UnusedScopeDoesNotAllocateReadReloadOrClearCaches) {
  const auto frame = thaiFrame();
  const int id = system.resolveFontId(kFamily, 16);
  const auto* font = renderer.getSdCardFonts().at(id);
  const auto allocations = probe::allocationCalls();
  const auto live = probe::allocationLive();
  const auto reads = probe::readCalls;
  {
    SdCardFontSystem::CoverDecodeScope unused(system, renderer);
  }
  const auto allocationsAfter = probe::allocationCalls();
  const auto liveAfter = probe::allocationLive();
  EXPECT_EQ(allocationsAfter, allocations);
  EXPECT_EQ(liveAfter, live);
  EXPECT_EQ(probe::readCalls, reads);
  EXPECT_EQ(renderer.getSdCardFonts().at(id), font);
  EXPECT_EQ(thaiFrame(), frame);
  expectSelected();
}

TEST_P(CoverDecodeScopeTest, ReleasesAllFontsIdempotentlyAndRestoresIdentityAndThaiFallbackPixels) {
  const auto before = thaiFrame();
  const int id = system.resolveFontId(kFamily, 16);
  const size_t loadedBytes = probe::allocationLive();
  size_t releasedBytes = 0;
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    releasedBytes = probe::allocationLive();
    expectReleased();
    const auto calls = probe::allocationCalls();
    const auto reads = probe::readCalls;
    scope.releaseFonts();
    const auto callsAfter = probe::allocationCalls();
    EXPECT_EQ(callsAfter, calls);
    EXPECT_EQ(probe::readCalls, reads);
    expectSelected();
  }
  EXPECT_LT(releasedBytes + 4 * sizeof(SdCardFont), loadedBytes);
  EXPECT_EQ(renderer.getSdCardFonts().size(), 4u);
  EXPECT_EQ(system.resolveFontId(kFamily, 16), id);
  const auto after = thaiFrame();
  EXPECT_EQ(after, before);
  expectSelected();
  writeFrame("before.pbm", before);
  writeFrame("after.pbm", after);
  if (!smokeOutput.empty()) {
    std::printf("cover-font mode=%s id_before=%d id_after=%d live_loaded=%zu live_released=%zu live_restored=%zu\n",
                GetParam() ? "cached" : "resident", id, system.resolveFontId(kFamily, 16), loadedBytes, releasedBytes,
                probe::allocationLive());
  }
  // Repeat with no assertions or rendering between the measurements: cached
  // companions share storage, and no cache/index may leak across unload cycles.
  size_t first = 0, second = 0;
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    first = probe::allocationLive();
  }
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    second = probe::allocationLive();
  }
  EXPECT_EQ(second, first);
}

TEST_P(CoverDecodeScopeTest, RestoreAllocationFailureKeepsSelectionThenNormalLoadRestoresFallbacks) {
  const auto before = thaiFrame();
  const int id = system.resolveFontId(kFamily, 16);
  {
    probe::ScopedNullableAllocationFailure fail(sizeof(SdCardFont), 1, probe::NullableAllocationKind::Scalar);
    {
      SdCardFontSystem::CoverDecodeScope scope(system, renderer);
      scope.releaseFonts();
    }
    EXPECT_EQ(probe::nullableAllocationFailure().failures, 1u);
  }
  expectReleased();
  expectSelected();
  // Built-in text remains safe after the failed restore; no UI/cache pointer
  // may refer to the deleted fonts. Then a normal entry restores Thai again.
  renderer.drawText(UI_10_FONT_ID, 24, 24, "Reader");
  cache.clearCache();
  system.ensureLoaded(renderer);
  EXPECT_EQ(system.resolveFontId(kFamily, 16), id);
  EXPECT_EQ(thaiFrame(), before);
  expectSelected();
}

TEST_P(CoverDecodeScopeTest, MissingFileDuringRestoreKeepsSelectionAndSize) {
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    fs::rename(fontPath(16), fontPath(16).string() + ".offline");
  }
  expectReleased();
  expectSelected();
  fs::rename(fontPath(16).string() + ".offline", fontPath(16));
  system.ensureLoaded(renderer);
  EXPECT_EQ(renderer.getSdCardFonts().size(), 4u);
}

TEST_P(CoverDecodeScopeTest, DirtyRegistryMissingFamilyDoesNotClearTemporarySelection) {
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    fs::rename(root / "fonts", root / "offline");
    system.markRegistryDirty();
  }
  expectReleased();
  expectSelected();
  // The ordinary loader retains its established missing-family policy.
  system.ensureLoaded(renderer);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "");
  EXPECT_GT(probe::settingsWrites, 0u);
}

TEST_P(CoverDecodeScopeTest, TemporaryRestoreDoesNotSnapRequestedPointSize) {
  SETTINGS.fontPointSize = 15;
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
  }
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, kFamily);
  EXPECT_EQ(SETTINGS.fontPointSize, 15);
  EXPECT_EQ(probe::settingsWrites, 0u);
  EXPECT_NE(system.resolveFontId(kFamily, 15), 0);
  system.ensureLoaded(renderer);
  EXPECT_EQ(SETTINGS.fontPointSize, 16);
  EXPECT_EQ(probe::settingsWrites, 1u);
}

TEST_P(CoverDecodeScopeTest, NoSelectedFamilyReleasesStaleFontsWithoutReloadOrSnap) {
  SETTINGS.sdFontFamilyName[0] = '\0';
  SETTINGS.fontPointSize = 15;
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    expectReleased();
  }
  expectReleased();
  EXPECT_EQ(SETTINGS.fontPointSize, 15);
  EXPECT_EQ(probe::settingsWrites, 0u);
}

TEST_P(CoverDecodeScopeTest, EarlyReturnRestoresFontsAndUiFallbacks) {
  const auto before = thaiFrame();
  const auto decodeFailure = [&]() {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
    std::vector<uint8_t> parserBuffer(4096);
    return false;
  };
  EXPECT_FALSE(decodeFailure());
  EXPECT_EQ(thaiFrame(), before);
  expectSelected();
}

TEST_P(CoverDecodeScopeTest, DropResidentThaiShapeReloadsSdBackedWithSameIdentityAndPixels) {
  const auto before = thaiFrame();
  const int id = system.resolveFontId(kFamily, 16);
  const size_t loadedBytes = probe::allocationLive();
  EXPECT_EQ(renderer.getSdCardFonts().at(id)->hasResidentThaiShape(), !GetParam());
  const auto reads = probe::readCalls;

  system.dropResidentThaiShape(renderer);

  ASSERT_EQ(renderer.getSdCardFonts().size(), 4u);
  EXPECT_EQ(system.resolveFontId(kFamily, 16), id);
  for (const auto& entry : renderer.getSdCardFonts()) EXPECT_FALSE(entry.second->hasResidentThaiShape());
  if (GetParam()) {
    // Nothing resident to shed: the loaded fonts are left alone.
    EXPECT_EQ(probe::readCalls, reads);
  } else {
    EXPECT_LT(probe::allocationLive(), loadedBytes);
  }
  EXPECT_EQ(thaiFrame(), before);
  expectSelected();

  // The policy outlives the reload: a later load must not go resident again.
  {
    SdCardFontSystem::CoverDecodeScope scope(system, renderer);
    scope.releaseFonts();
  }
  ASSERT_EQ(renderer.getSdCardFonts().size(), 4u);
  for (const auto& entry : renderer.getSdCardFonts()) EXPECT_FALSE(entry.second->hasResidentThaiShape());
  EXPECT_EQ(thaiFrame(), before);
}

INSTANTIATE_TEST_SUITE_P(CompanionStorage, CoverDecodeScopeTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) { return info.param ? "Cached" : "Resident"; });
}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--smoke-output") != 0) continue;
    if (i + 1 >= argc) {
      std::fputs("--smoke-output requires a directory\n", stderr);
      return 2;
    }
    smokeOutput = argv[i + 1];
    for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
    argc -= 2;
    --i;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
