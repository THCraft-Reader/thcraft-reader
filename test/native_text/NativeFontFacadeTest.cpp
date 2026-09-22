#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <NativeTextEngine.h>
#include <ReaderFontSizes.h>
#include <SdCardFontSystem.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
class NativeTextFontFacadeTest : public testing::Test {
 protected:
  NativeTextEngine engine;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  SdCardFontSystem fonts;
  std::filesystem::path root, previousRoot;

  void SetUp() override {
    ASSERT_EQ(Storage.openHandles(), 0u);
    previousRoot = Storage.root();
    root = std::filesystem::temp_directory_path() /
           ("native-font-facade-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Storage.setRoot(root);
    SETTINGS.fontFamily = CrossPointSettings::NOTOSANS;
    SETTINGS.fontPointSize = 14;
    SETTINGS.lineSpacing = CrossPointSettings::NORMAL;
    SETTINGS.sdFontFamilyName[0] = 0;
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
  }
  void TearDown() override {
    fonts.releaseNativeFonts();
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    SETTINGS.sdFontFamilyName[0] = 0;
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.endUsbDrive();
    Storage.resetFaults();
    Storage.setRoot(previousRoot);
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  void select(const char* name) {
    ASSERT_LT(std::strlen(name), sizeof(SETTINGS.sdFontFamilyName));
    std::strcpy(SETTINGS.sdFontFamilyName, name);
  }
  std::filesystem::path install(const char* family, const char* source = NATIVE_FONT_PATH,
                                const char* extension = ".ttf", bool hidden = true) {
    const auto directory = root / (hidden ? ".fonts" : "fonts") / family;
    std::filesystem::create_directories(directory);
    const auto file = directory / (std::string(family) + "-Regular" + extension);
    std::filesystem::copy_file(source, file, std::filesystem::copy_options::overwrite_existing);
    return directory;
  }
  void put(const std::filesystem::path& path, const char* bytes) {
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    file << bytes;
    ASSERT_TRUE(file);
  }
  std::vector<uint8_t> pixels(const char* text) {
    renderer.clearTextStatus();
    renderer.clearScreen();
    renderer.drawText(SETTINGS.getReaderFontId(), 20, 20, text);
    EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
    return {renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()};
  }
};

TEST_F(NativeTextFontFacadeTest, MissingCpfontSelectionFallsBackOnceAndRestoresAfterNativeInstall) {
  const auto directory = root / ".fonts" / "Legacy";
  std::filesystem::create_directories(directory);
  put(directory / "Legacy_14.cpfont", "old-device-font-bytes");
  select("Legacy");
  fonts.begin(renderer);
  EXPECT_EQ(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  EXPECT_FLOAT_EQ(SETTINGS.getReaderLineCompression(), 0.95f);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "Legacy");
  EXPECT_NE(fonts.takeNotice(), nullptr);
  fonts.ensureLoaded(renderer);
  EXPECT_EQ(fonts.takeNotice(), nullptr);
  EXPECT_FALSE(Storage.exists("/.crosspoint/settings.json"));
  install("Legacy");
  fonts.markRegistryDirty();
  fonts.ensureLoaded(renderer);
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  EXPECT_NE(SETTINGS.getReaderFontId(), 0);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "Legacy");
  EXPECT_EQ(fonts.takeNotice(), nullptr);
  std::ifstream old(directory / "Legacy_14.cpfont", std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(old), {}), "old-device-font-bytes");
}

TEST_F(NativeTextFontFacadeTest, HiddenRootWinsAndOneSourceSupportsAllReadingSizes) {
  const auto hidden = install("_Family", NATIVE_FONT_PATH, ".TTF");
  const auto visible = install("_Family", NATIVE_VARIABLE_FONT_PATH, ".ttf", false);
  put(hidden / "_Family-Bold.ttf.part", "incomplete-font");
  select("_Family");
  fonts.begin(renderer);
  const auto* family = fonts.registry().findFamily("_Family");
  ASSERT_NE(family, nullptr);
  ASSERT_EQ(family->files.size(), 1u);
  EXPECT_EQ(family->files.front().path, "/.fonts/_Family/_Family-Regular.TTF");
  EXPECT_EQ(family->availableSizes(), (std::vector<uint8_t>{12, 14, 16, 18}));
  int previousHeight = 0;
  for (uint8_t points : {12, 14, 16, 18}) {
    SETTINGS.fontPointSize = points;
    fonts.ensureLoaded(renderer);
    const int fontId = SETTINGS.getReaderFontId();
    EXPECT_NE(fontId, 0);
    const int height = renderer.getLineHeight(fontId);
    EXPECT_GT(height, previousHeight);
    previousHeight = height;
  }
  EXPECT_TRUE(std::filesystem::exists(visible / "_Family-Regular.ttf"));
}

TEST_F(NativeTextFontFacadeTest, InvalidOptionalStyleOrSidecarRejectsWholeFamily) {
  const auto directory = install("Broken");
  put(directory / "Broken-Bold.ttf", "not-a-font");
  select("Broken");
  fonts.begin(renderer);
  for (uint8_t size : {12, 14, 16, 18}) EXPECT_EQ(fonts.resolveFontId("Broken", size), 0);
  EXPECT_EQ(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  EXPECT_NE(fonts.takeNotice(), nullptr);
  std::filesystem::remove(directory / "Broken-Bold.ttf");
  put(directory / "native-font.json", "{\"version\":1,\"styles\":{\"regular\":{\"file\":\"../outside.ttf\"}}}");
  fonts.markRegistryDirty();
  fonts.ensureLoaded(renderer);
  EXPECT_EQ(fonts.resolveFontId("Broken", 14), 0);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "Broken");
}

TEST_F(NativeTextFontFacadeTest, SidecarVariationChangesPixelsAndLayoutIdentityWithoutRenamingFamily) {
  const auto directory = install("Variable", NATIVE_VARIABLE_FONT_PATH);
  const auto sidecar = directory / "native-font.json";
  put(sidecar, "{\"version\":1,\"styles\":{\"regular\":{\"file\":\"Variable-Regular.ttf\",\"axes\":{\"wght\":100}}}}");
  select("Variable");
  fonts.begin(renderer);
  const int id = SETTINGS.getReaderFontId();
  ASSERT_NE(id, NOTOSANS_14_FONT_ID);
  const auto light = pixels("abc");
  const auto oldIdentity = renderer.textLayoutFingerprint(id);
  fonts.releaseNativeFonts();
  put(sidecar, "{\"version\":1,\"styles\":{\"regular\":{\"file\":\"Variable-Regular.ttf\",\"axes\":{\"wght\":900}}}}");
  fonts.markRegistryDirty();
  fonts.ensureLoaded(renderer);
  EXPECT_EQ(SETTINGS.getReaderFontId(), id);
  EXPECT_NE(renderer.textLayoutFingerprint(id), oldIdentity);
  EXPECT_NE(pixels("abc"), light);
}

TEST_F(NativeTextFontFacadeTest, StorageHandoffClosesNativeStreamsAndLeavesBundledUiUsable) {
  install("Family");
  select("Family");
  fonts.begin(renderer);
  pixels("ภาษาไทย");
  EXPECT_GT(Storage.openHandles(), 0u);
  EXPECT_FALSE(Storage.beginUsbDrive());
  fonts.releaseNativeFonts();
  EXPECT_EQ(Storage.openHandles(), 0u);
  ASSERT_TRUE(Storage.beginUsbDrive());
  renderer.clearTextStatus();
  renderer.clearScreen();
  renderer.drawText(UI_12_FONT_ID, 10, 10, "ภาษาไทย");
  EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
  EXPECT_EQ(Storage.forbiddenAccesses(), 0u);
}
}  // namespace
