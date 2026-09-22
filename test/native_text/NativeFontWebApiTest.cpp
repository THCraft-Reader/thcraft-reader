#include <ArduinoJson.h>
#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <NativeTextEngine.h>
#include <SdCardFontSystem.h>
#include <gtest/gtest.h>
#include <network/FontWebApi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
class NativeTextFontWebApiTest : public testing::Test {
 protected:
  NativeTextEngine engine;
  HalDisplay panel;
  GfxRenderer renderer{panel};
  std::unique_ptr<FontWebApi> api;
  std::filesystem::path root, previousRoot;
  std::vector<uint8_t> font, replacement;

  static std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  void SetUp() override {
    ASSERT_EQ(Storage.openHandles(), 0u);
    previousRoot = Storage.root();
    root = std::filesystem::temp_directory_path() /
           ("native-font-web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Storage.setRoot(root);
    SETTINGS.sdFontFamilyName[0] = 0;
    SETTINGS.fontFamily = CrossPointSettings::NOTOSANS;
    ASSERT_EQ(engine.initialize(), TextStatus::Ok);
    renderer.begin();
    renderer.setNativeTextEngine(&engine);
    sdFontSystem.begin(renderer);
    api = std::make_unique<FontWebApi>(sdFontSystem);
    font = readBytes(NATIVE_FONT_PATH);
    replacement = readBytes(NATIVE_VARIABLE_FONT_PATH);
    ASSERT_GT(font.size(), 8192u);
    ASSERT_GT(replacement.size(), 8192u);
  }
  void TearDown() override {
    api.reset();
    sdFontSystem.releaseNativeFonts();
    SETTINGS.sdFontFamilyName[0] = 0;
    renderer.setNativeTextEngine(nullptr);
    // The global facade must not retain this fixture's engine pointer.
    sdFontSystem.begin(renderer);
    SETTINGS.sdFontIdResolver = nullptr;
    SETTINGS.sdFontResolverCtx = nullptr;
    engine.shutdown();
    EXPECT_EQ(Storage.openHandles(), 0u);
    Storage.resetFaults();
    Storage.setRoot(previousRoot);
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  std::filesystem::path path(const char* name) const { return root / ".fonts" / "Web" / name; }
  void put(const char* name, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path(name).parent_path());
    std::ofstream output(path(name), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ASSERT_TRUE(output);
  }
  void existingFamily() {
    put("Web-Regular.ttf", font);
    put("Web_14.cpfont", {'l', 'e', 'g', 'a', 'c', 'y'});
    put("keep.txt", {'k', 'e', 'e', 'p'});
    sdFontSystem.markRegistryDirty();
  }
  static std::string manifest(std::initializer_list<std::pair<const char*, size_t>> entries,
                              const char* family = "Web") {
    JsonDocument doc;
    doc["family"] = family;
    auto files = doc["files"].to<JsonArray>();
    for (const auto& [name, size] : entries) {
      auto file = files.add<JsonObject>();
      file["name"] = name;
      file["size"] = static_cast<uint32_t>(size);
    }
    std::string result;
    serializeJson(doc, result);
    return result;
  }
  bool upload(const char* name, const std::vector<uint8_t>& bytes) {
    if (!api->beginFile(name)) return false;
    // Split both native signature and the rest of the SFNT header between
    // callbacks, then exercise buffered SD writes and the final short write.
    size_t offset = 0;
    for (size_t count : {1u, 2u, 1u, 3u, 5u}) {
      if (!api->writeChunk(bytes.data() + offset, count)) return false;
      offset += count;
    }
    while (offset < bytes.size()) {
      const size_t count = std::min<size_t>(1733, bytes.size() - offset);
      if (!api->writeChunk(bytes.data() + offset, count)) return false;
      offset += count;
    }
    return api->endFile();
  }
};

TEST_F(NativeTextFontWebApiTest, MultipleStylesPublishTogetherAndDeletionPreservesLegacyFiles) {
  existingFamily();
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.TTF", replacement.size()}, {"Web-Bold.otf", font.size()}})));
  ASSERT_TRUE(upload("Web-Regular.TTF", replacement));
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Bold.otf")));
  ASSERT_TRUE(upload("Web-Bold.otf", font));
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  ASSERT_EQ(api->finishUpload().status, 200);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), replacement);
  EXPECT_EQ(readBytes(path("Web-Bold.otf")), font);
  const auto response = api->list();
  ASSERT_EQ(response.status, 200);
  JsonDocument listing;
  ASSERT_FALSE(deserializeJson(listing, response.body));
  EXPECT_EQ(listing["format"].as<std::string>(), "opentype");
  ASSERT_EQ(listing["families"].size(), 1u);
  EXPECT_EQ(listing["families"][0]["files"].size(), 2u);
  ASSERT_EQ(api->remove("{\"family\":\"Web\"}").status, 200);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Regular.ttf")));
  EXPECT_FALSE(std::filesystem::exists(path("Web-Bold.otf")));
  EXPECT_EQ(readBytes(path("Web_14.cpfont")), (std::vector<uint8_t>{'l', 'e', 'g', 'a', 'c', 'y'}));
  EXPECT_EQ(readBytes(path("keep.txt")), (std::vector<uint8_t>{'k', 'e', 'e', 'p'}));
  ASSERT_EQ(api->list().status, 200);
  EXPECT_EQ(sdFontSystem.registry().findFamily("Web"), nullptr);
}

TEST_F(NativeTextFontWebApiTest, InvalidSfntInSecondStyleNeverPublishesTheFirstStyle) {
  existingFamily();
  std::vector<uint8_t> invalid(32, 0);
  invalid[1] = 1;  // a genuine SFNT signature is not sufficient validation
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size()}, {"Web-Bold.ttf", invalid.size()}})));
  ASSERT_TRUE(upload("Web-Regular.ttf", replacement));
  ASSERT_TRUE(upload("Web-Bold.ttf", invalid));
  EXPECT_EQ(api->finishUpload().status, 400);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Bold.ttf")));
  EXPECT_FALSE(std::filesystem::exists(path("Web-Regular.ttf.part")));
  EXPECT_FALSE(std::filesystem::exists(path("Web-Bold.ttf.part")));
}

TEST_F(NativeTextFontWebApiTest, TruncatedAndCancelledRequestsKeepTheOldInstallation) {
  existingFamily();
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size() + 1}})));
  EXPECT_FALSE(upload("Web-Regular.ttf", replacement));
  EXPECT_EQ(api->finishUpload().status, 400);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Regular.ttf.part")));
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size()}, {"Web-Bold.ttf", font.size()}})));
  ASSERT_TRUE(upload("Web-Regular.ttf", replacement));
  ASSERT_TRUE(api->beginFile("Web-Bold.ttf"));
  ASSERT_TRUE(api->writeChunk(font.data(), 17));
  api->abortUpload();
  EXPECT_EQ(api->finishUpload().status, 400);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Regular.ttf.part")));
  EXPECT_FALSE(std::filesystem::exists(path("Web-Bold.ttf.part")));
  EXPECT_EQ(Storage.openHandles(), 0u);
}

TEST_F(NativeTextFontWebApiTest, ShortSdWritesCannotMasqueradeAsSuccessfulUpload) {
  existingFamily();
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size()}})));
  Storage.faults().writePath = "/.fonts/Web/Web-Regular.ttf.part";
  Storage.faults().writeBytes = 1024;
  EXPECT_FALSE(upload("Web-Regular.ttf", replacement));
  EXPECT_EQ(api->finishUpload().status, 500);
  Storage.resetFaults();
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Regular.ttf.part")));
  EXPECT_EQ(Storage.openHandles(), 0u);
}

TEST_F(NativeTextFontWebApiTest, WholeRequestRejectsTraversalMixedFamiliesDuplicateStylesAndOversizedNames) {
  existingFamily();
  const std::string requests[] = {
      manifest({{"../Web-Regular.ttf", font.size()}}),
      manifest({{"Web-Regular.ttf", font.size()}, {"Other-Bold.ttf", font.size()}}),
      manifest({{"Web-Regular.ttf", font.size()}, {"Web-Regular.otf", font.size()}}),
      manifest({{"Web-Regular.ttf", font.size()}}, "12345678901234567890123456789012"),
      manifest({{"Web_14.cpfont", font.size()}}),
  };
  for (const auto& request : requests) {
    SCOPED_TRACE(request);
    EXPECT_FALSE(api->beginUpload(request));
    EXPECT_EQ(api->finishUpload().status, 400);
    EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
    EXPECT_FALSE(std::filesystem::exists(path("Web-Regular.ttf.part")));
  }
  EXPECT_EQ(api->remove("{\"family\":\"../Web\"}").status, 400);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
}

TEST_F(NativeTextFontWebApiTest, MissingUndeclaredAndRepeatedMultipartFilesCannotCommit) {
  existingFamily();
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size()}, {"Web-Bold.ttf", font.size()}})));
  ASSERT_TRUE(upload("Web-Regular.ttf", replacement));
  EXPECT_EQ(api->finishUpload().status, 400);  // declared second style never arrived
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size()}})));
  EXPECT_FALSE(api->beginFile("../Web-Regular.ttf"));
  EXPECT_EQ(api->finishUpload().status, 400);
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Regular.ttf", replacement.size()}})));
  ASSERT_TRUE(upload("Web-Regular.ttf", replacement));
  EXPECT_FALSE(api->beginFile("Web-Regular.ttf"));
  EXPECT_EQ(api->finishUpload().status, 400);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
}

TEST_F(NativeTextFontWebApiTest, OptionalStyleRequiresRegularButCanExtendAnInstalledFamily) {
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Bold.ttf", font.size()}})));
  ASSERT_TRUE(upload("Web-Bold.ttf", font));
  EXPECT_EQ(api->finishUpload().status, 400);
  EXPECT_FALSE(std::filesystem::exists(path("Web-Bold.ttf")));
  existingFamily();
  ASSERT_TRUE(api->beginUpload(manifest({{"Web-Bold.ttf", replacement.size()}})));
  ASSERT_TRUE(upload("Web-Bold.ttf", replacement));
  ASSERT_EQ(api->finishUpload().status, 200);
  EXPECT_EQ(readBytes(path("Web-Regular.ttf")), font);
  EXPECT_EQ(readBytes(path("Web-Bold.ttf")), replacement);
}
}  // namespace
