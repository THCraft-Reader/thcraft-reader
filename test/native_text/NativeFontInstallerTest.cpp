#include <CrossPointSettings.h>
#include <FontInstaller.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <NativeAllocator.h>
#include <NativeTextEngine.h>
#include <SdCardFontSystem.h>
#include <fontIds.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
using Error = FontInstaller::Error;
using StyleFile = FontInstaller::NativeStyleFile;
namespace fs = std::filesystem;

class NativeTextFontInstaller : public testing::Test {
 protected:
  NativeTextEngine engine;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontInstaller installer{sdFontSystem.registry()};
  fs::path root, previousRoot;

  void SetUp() override {
    ASSERT_EQ(Storage.openHandles(), 0u);
    previousRoot = Storage.root();
    root = fs::temp_directory_path() /
           ("native-font-installer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Storage.setRoot(root);
    Storage.resetFaults();
    SETTINGS.fontFamily = CrossPointSettings::NOTOSANS;
    SETTINGS.fontPointSize = 14;
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
    sdFontSystem.begin(renderer);
  }
  void TearDown() override {
    native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
    Storage.resetFaults();
    sdFontSystem.releaseNativeFonts();
    renderer.setNativeTextEngine(nullptr);
    engine.shutdown();
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.setRoot(previousRoot);
    std::error_code error;
    fs::remove_all(root, error);
  }
  static std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file) << path;
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  void put(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(file);
  }
  std::map<std::string, std::string> disk() {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (entry.is_regular_file())
        result.emplace(entry.path().lexically_relative(root).generic_string(), bytes(entry.path()));
    }
    return result;
  }
  StyleFile stage(const char* name, const fs::path& source = NATIVE_FONT_PATH,
                  std::span<const NativeVariation> axes = {}) {
    char family[32], canonical[64], path[112];
    uint8_t style = 0;
    if (!FontInstaller::parseNativeFilename(name, family, sizeof(family), style, canonical, sizeof(canonical))) {
      ADD_FAILURE() << "Invalid fixture filename " << name;
      return {name, style, axes};
    }
    EXPECT_TRUE(installer.ensureFamilyDir(family));
    EXPECT_TRUE(FontInstaller::buildStagingFontPath(family, canonical, path, sizeof(path)));
    const auto content = bytes(source);
    HalFile output;
    EXPECT_TRUE(Storage.openFileForWrite("TEST", path, output));
    // Exercise a header split across separate writes, then the remaining body.
    size_t offset = 0;
    for (size_t count : {size_t(1), size_t(2), size_t(1)}) {
      EXPECT_EQ(output.write(content.data() + offset, count), count);
      offset += count;
    }
    while (offset < content.size()) {
      const size_t count = std::min<size_t>(997, content.size() - offset);
      EXPECT_EQ(output.write(content.data() + offset, count), count);
      offset += count;
    }
    EXPECT_TRUE(output.close());
    return {name, style, axes, static_cast<uint32_t>(content.size())};
  }
  void installOldFamily() {
    const StyleFile file = stage("Family-Regular.ttf");
    ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::OK);
    std::strcpy(SETTINGS.sdFontFamilyName, "Family");
    sdFontSystem.ensureLoaded(renderer);
    ASSERT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  }
  std::vector<uint8_t> pixels(EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    sdFontSystem.ensureLoaded(renderer);
    renderer.clearTextStatus();
    renderer.clearScreen();
    renderer.drawText(SETTINGS.getReaderFontId(), 20, 20, "abc ภาษาไทย", true, style);
    EXPECT_EQ(renderer.lastTextStatus(), TextStatus::Ok);
    return {renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()};
  }
};

TEST_F(NativeTextFontInstaller, SplitStagedTrueTypeAndCffOpenTypePublishCanonicalSelectableFamilies) {
  auto file = stage("Family-Regular.TtF");
  // CRC32 of the pinned 37,780-byte source, independently obtained with Python zlib.
  file.verifyChecksum = true;
  file.crc32 = 0x2216bc8f;
  EXPECT_TRUE(installer.validateFontFile("/.fonts/Family/Family-Regular.ttf.part"));
  ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, false), Error::OK);
  EXPECT_EQ(bytes(root / ".fonts/Family/Family-Regular.ttf"), bytes(NATIVE_FONT_PATH));
  EXPECT_FALSE(fs::exists(root / ".fonts/Family/Family-Regular.ttf.part"));
  std::strcpy(SETTINGS.sdFontFamilyName, "Family");
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  pixels();

  const auto source = fs::path(NATIVE_VARIABLE_FONT_PATH).parent_path() / "AdobeVFPrototype.abc.static.otf";
  file = stage("OpenType-Regular.OTF", source);
  ASSERT_EQ(installer.commitNativeFamily("OpenType", {&file, 1}, true), Error::OK);
  EXPECT_EQ(bytes(root / ".fonts/OpenType/OpenType-Regular.otf"), bytes(source));
  std::strcpy(SETTINGS.sdFontFamilyName, "OpenType");
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  pixels();
}

TEST_F(NativeTextFontInstaller, RejectsTraversalCpfontContainersAndOversizedSavedNames) {
  char family[32], canonical[64], path[112];
  uint8_t style;
  for (const char* name :
       {"../Family-Regular.ttf", "Family/Family-Regular.ttf", "Family\\Family-Regular.ttf", "Family-Regular.ttf.part",
        "Family_14.cpfont", "Family-Regular.ttc", "Family-Regular.woff", "Family-Regular.exe.ttf"}) {
    EXPECT_FALSE(FontInstaller::parseNativeFilename(name, family, sizeof(family), style, canonical, sizeof(canonical)));
    EXPECT_FALSE(FontInstaller::isValidFontFilename(name));
  }
  EXPECT_FALSE(installer.ensureFamilyDir("../escape"));
  EXPECT_EQ(installer.lastError(), Error::INVALID_FAMILY_NAME);
  const std::string tooLong(32, 'A');
  EXPECT_FALSE(installer.ensureFamilyDir(tooLong.c_str()));
  EXPECT_FALSE(FontInstaller::buildStagingFontPath("Family", "Other-Regular.ttf", path, sizeof(path)));
  EXPECT_FALSE(installer.validateFontFile("/.fonts/Family/../Other-Regular.ttf"));
  put(root / ".fonts/Family/Family-Regular.ttf.part", std::string("CPFONT\0\0", 8));
  EXPECT_FALSE(installer.validateFontFile("/.fonts/Family/Family-Regular.ttf.part"));
  StyleFile forged{"Family-Regular.ttf", 0, {}, 8};
  EXPECT_EQ(installer.commitNativeFamily("Family", {&forged, 1}, true), Error::INVALID_FILE);
  EXPECT_FALSE(fs::exists(root / ".fonts/Family/Family-Regular.ttf"));
  EXPECT_FALSE(fs::exists(root.parent_path() / "escape"));
}

TEST_F(NativeTextFontInstaller, MixedFamilyDuplicateStyleAndMissingRegularRequestsNeverPublish) {
  auto first = stage("Family-Regular.ttf");
  auto other = stage("Other-Bold.ttf");
  StyleFile mixed[] = {first, other};
  EXPECT_EQ(installer.commitNativeFamily("Family", mixed, true), Error::INVALID_FILE);
  EXPECT_FALSE(fs::exists(root / ".fonts/Family/Family-Regular.ttf"));
  installer.discardNativeStaging("Family", {&first, 1});
  installer.discardNativeStaging("Other", {&other, 1});
  first = stage("Family-Regular.ttf");
  other = stage("Family-Regular.otf");
  StyleFile duplicate[] = {first, other};
  EXPECT_EQ(installer.commitNativeFamily("Family", duplicate, false), Error::INVALID_FILE);
  installer.discardNativeStaging("Family", duplicate);
  other = stage("Family-Bold.ttf");
  EXPECT_EQ(installer.commitNativeFamily("Family", {&other, 1}, false), Error::INVALID_FILE);
  EXPECT_TRUE(disk().empty());
}

TEST_F(NativeTextFontInstaller, InvalidOptionalStyleCannotReplaceAnyExistingSourceOrSelection) {
  installOldFamily();
  const auto before = disk();
  const auto drawn = pixels();
  const int id = SETTINGS.getReaderFontId();
  const auto identity = renderer.textLayoutFingerprint(id);
  auto regular = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  put(root / ".fonts/Family/Family-Bold.ttf.part", "OTTOinvalid optional font");
  StyleFile files[] = {regular, {"Family-Bold.ttf", 1, {}, 25}};
  files[1].size = static_cast<uint32_t>(fs::file_size(root / ".fonts/Family/Family-Bold.ttf.part"));
  EXPECT_EQ(installer.commitNativeFamily("Family", files, true), Error::INVALID_FILE);
  EXPECT_EQ(disk(), before);
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "Family");
  EXPECT_EQ(SETTINGS.getReaderFontId(), id);
  EXPECT_EQ(renderer.textLayoutFingerprint(id), identity);
  EXPECT_EQ(pixels(), drawn);
}

TEST_F(NativeTextFontInstaller, DeclaredSizeAndChecksumMustMatchActualReadableBytes) {
  installOldFamily();
  const auto before = disk();
  auto file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  ++file.size;
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::INVALID_FILE);
  EXPECT_EQ(disk(), before);
  file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  file.verifyChecksum = true;
  file.crc32 = 0;  // This pinned SFNT has a nonzero CRC32.
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::INVALID_FILE);
  EXPECT_EQ(disk(), before);
  file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  Storage.faults().readPath = "Family-Regular.ttf.part";
  Storage.faults().readBytes = 7;
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::SD_WRITE_ERROR);
  Storage.resetFaults();
  EXPECT_EQ(disk(), before);
}

TEST_F(NativeTextFontInstaller, TruncatedUploadAndCancellationPreservePreviousInstallation) {
  installOldFamily();
  const auto before = disk();
  ASSERT_TRUE(installer.ensureFamilyDir("Family"));
  const auto content = bytes(NATIVE_VARIABLE_FONT_PATH);
  Storage.faults().writePath = "Family-Regular.ttf.part";
  Storage.faults().writeBytes = 3;
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/.fonts/Family/Family-Regular.ttf.part", file));
  EXPECT_EQ(file.write(content.data(), content.size()), 3u);
  ASSERT_TRUE(file.close());
  Storage.resetFaults();
  StyleFile request{"Family-Regular.ttf", 0, {}, static_cast<uint32_t>(content.size())};
  EXPECT_EQ(installer.commitNativeFamily("Family", {&request, 1}, false), Error::INVALID_FILE);
  EXPECT_EQ(disk(), before);

  request = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  auto bold = stage("Family-Bold.ttf");
  StyleFile cancelled[] = {request, bold};
  installer.discardNativeStaging("Family", cancelled);
  EXPECT_EQ(disk(), before);
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
}

TEST_F(NativeTextFontInstaller, LaterFontRenameFailureRollsBackAlreadyPublishedFiles) {
  installOldFamily();
  auto oldBold = stage("Family-Bold.ttf");
  ASSERT_EQ(installer.commitNativeFamily("Family", {&oldBold, 1}, false), Error::OK);
  const auto before = disk();
  StyleFile replacement[] = {stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH),
                             stage("Family-Bold.ttf", NATIVE_VARIABLE_FONT_PATH)};
  Storage.faults().renamePath = "Family-Bold.ttf.part";
  Storage.faults().renameFailures = 1;
  EXPECT_EQ(installer.commitNativeFamily("Family", replacement, true), Error::SD_WRITE_ERROR);
  Storage.resetFaults();
  EXPECT_EQ(disk(), before);
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
}

TEST_F(NativeTextFontInstaller, BackupRenameFailureRestoresEarlierBackups) {
  installOldFamily();
  auto bold = stage("Family-Bold.ttf");
  ASSERT_EQ(installer.commitNativeFamily("Family", {&bold, 1}, false), Error::OK);
  const auto before = disk();
  auto replacement = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  Storage.faults().renamePath = "Family-Bold.ttf";
  Storage.faults().renameFailures = 1;
  EXPECT_EQ(installer.commitNativeFamily("Family", {&replacement, 1}, true), Error::SD_WRITE_ERROR);
  Storage.resetFaults();
  EXPECT_EQ(disk(), before);
}

TEST_F(NativeTextFontInstaller, SidecarShortWriteAndPublishFailureLeaveOldRecipeAndFontsIntact) {
  const NativeVariation light[] = {{0x77676874, 100}};
  const NativeVariation heavy[] = {{0x77676874, 900}};
  auto file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, light);
  ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::OK);
  const auto before = disk();
  file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, heavy);
  Storage.faults().writePath = "native-font.json.part";
  Storage.faults().writeBytes = 10;
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::SD_WRITE_ERROR);
  Storage.resetFaults();
  EXPECT_EQ(disk(), before);

  file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, heavy);
  Storage.faults().renamePath = "native-font.json.part";
  Storage.faults().renameFailures = 1;
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::SD_WRITE_ERROR);
  Storage.resetFaults();
  EXPECT_EQ(disk(), before);
}

TEST_F(NativeTextFontInstaller, ManualStyleReplacementKeepsUntouchedAxesAndRemovesChangedStyleRecipe) {
  const NativeVariation light[] = {{0x77676874, 100}};
  const NativeVariation heavy[] = {{0x77676874, 900}};
  StyleFile initial[] = {stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, light),
                         stage("Family-Bold.ttf", NATIVE_VARIABLE_FONT_PATH, heavy)};
  ASSERT_EQ(installer.commitNativeFamily("Family", initial, true), Error::OK);
  std::strcpy(SETTINGS.sdFontFamilyName, "Family");
  const auto regularPixels = pixels();
  const auto boldPixels = pixels(EpdFontFamily::BOLD);
  auto file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, false), Error::OK);
  EXPECT_NE(pixels(), regularPixels);
  EXPECT_EQ(pixels(EpdFontFamily::BOLD), boldPixels);
  const auto sidecar = bytes(root / ".fonts/Family/native-font.json");
  EXPECT_EQ(sidecar.find("Family-Regular.ttf"), std::string::npos);
  EXPECT_NE(sidecar.find("Family-Bold.ttf"), std::string::npos);
  file = stage("Family-Bold.ttf", NATIVE_VARIABLE_FONT_PATH);
  ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, false), Error::OK);
  EXPECT_FALSE(fs::exists(root / ".fonts/Family/native-font.json"));
  EXPECT_NE(pixels(EpdFontFamily::BOLD), boldPixels);
}

TEST_F(NativeTextFontInstaller, UnknownAxesRejectBeforePublicationAndSupportedCoordinatesClamp) {
  installOldFamily();
  const auto before = disk();
  const NativeVariation invalid[] = {{0x6e6f7065, 1}};
  auto file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, invalid);
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::INVALID_FILE);
  EXPECT_EQ(disk(), before);
  const NativeVariation beyondMaximum[] = {{0x77676874, 1000000}};
  file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, beyondMaximum);
  ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::OK);
  const auto clampedPixels = pixels();
  const NativeVariation maximum[] = {{0x77676874, 900}};
  file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH, maximum);
  ASSERT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::OK);
  EXPECT_EQ(pixels(), clampedPixels);
}

TEST_F(NativeTextFontInstaller, CompleteReplacementAndDeletionPreserveCpfontChildrenAndSavedSelection) {
  installOldFamily();
  auto bold = stage("Family-Bold.ttf");
  ASSERT_EQ(installer.commitNativeFamily("Family", {&bold, 1}, false), Error::OK);
  put(root / ".fonts/Family/Family_14.cpfont", "legacy hidden font");
  put(root / ".fonts/Family/notes.txt", "user notes");
  put(root / ".fonts/Family/child/Family-Regular.ttf", "unrelated child");
  put(root / "fonts/Family/Family_14.cpfont", "legacy visible font");
  put(root / "fonts/Family/Family-Regular.ttf", bytes(NATIVE_FONT_PATH));
  auto replacement = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  ASSERT_EQ(installer.commitNativeFamily("Family", {&replacement, 1}, true), Error::OK);
  EXPECT_FALSE(fs::exists(root / ".fonts/Family/Family-Bold.ttf"));
  pixels();
  ASSERT_GT(Storage.openHandles(), 0u);
  ASSERT_EQ(installer.deleteFamily("Family"), Error::OK);
  EXPECT_EQ(Storage.openHandles(), 0u);
  EXPECT_FALSE(fs::exists(root / ".fonts/Family/Family-Regular.ttf"));
  EXPECT_FALSE(fs::exists(root / "fonts/Family/Family-Regular.ttf"));
  EXPECT_EQ(bytes(root / ".fonts/Family/Family_14.cpfont"), "legacy hidden font");
  EXPECT_EQ(bytes(root / "fonts/Family/Family_14.cpfont"), "legacy visible font");
  EXPECT_EQ(bytes(root / ".fonts/Family/notes.txt"), "user notes");
  EXPECT_EQ(bytes(root / ".fonts/Family/child/Family-Regular.ttf"), "unrelated child");
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "Family");
  EXPECT_EQ(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
  EXPECT_FALSE(fs::exists(root / ".crosspoint/settings.json"));
  replacement = stage("Family-Regular.ttf");
  ASSERT_EQ(installer.commitNativeFamily("Family", {&replacement, 1}, true), Error::OK);
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "Family");
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
}

TEST_F(NativeTextFontInstaller, NativeBudgetFailureCannotDamageAnInstalledFamily) {
  installOldFamily();
  const auto before = disk();
  auto file = stage("Family-Regular.ttf", NATIVE_VARIABLE_FONT_PATH);
  native_text::failAllocationsAfter(0);
  EXPECT_EQ(installer.commitNativeFamily("Family", {&file, 1}, true), Error::OUT_OF_MEMORY);
  native_text::failAllocationsAfter(std::numeric_limits<size_t>::max());
  installer.discardNativeStaging("Family", {&file, 1});
  EXPECT_EQ(disk(), before);
  sdFontSystem.ensureLoaded(renderer);
  EXPECT_NE(SETTINGS.getReaderFontId(), NOTOSANS_14_FONT_ID);
}
}  // namespace
