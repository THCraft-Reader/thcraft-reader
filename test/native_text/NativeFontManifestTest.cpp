#include <FontManifest.h>
#include <HalStorage.h>
#include <NativeFontCatalogue.generated.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
constexpr const char* VALID_MANIFEST = R"json({
  "version":1,"format":"opentype",
  "scriptGroups":[{"tag":"latin","label":"Latin"},{"tag":"thai","label":"Thai"}],
  "families":[{"name":"Example","description":"Variable example","scripts":["latin","thai"],"files":[
    {"name":"Example-Regular.ttf","size":1024,"crc32":4294967295,
     "url":"https://raw.githubusercontent.com/fonts/repo/abc/Example.ttf","axes":{"wght":400,"opsz":14}},
    {"name":"Example-Bold.otf","size":2048,"crc32":0,
     "url":"https://raw.githubusercontent.com/fonts/repo/abc/Example-Bold.otf"}
  ]}]
})json";

std::string encoded(const JsonDocument& document) {
  std::string result;
  serializeJson(document, result);
  return result;
}

class NativeTextFontManifestTest : public testing::Test {
 protected:
  FontManifest manifest;
  JsonDocument document;
  void SetUp() override { ASSERT_FALSE(deserializeJson(document, VALID_MANIFEST)); }
  FontManifest::Error load() {
    const auto json = encoded(document);
    return manifest.load(json.data(), json.size());
  }
  void expectRejectedWithoutReplacingCatalogue() {
    ASSERT_EQ(manifest.load(VALID_MANIFEST, std::strlen(VALID_MANIFEST)), FontManifest::Error::Ok);
    EXPECT_EQ(load(), FontManifest::Error::Invalid);
    ASSERT_EQ(manifest.families().size(), 1u);
    EXPECT_STREQ(manifest.str(manifest.families()[0].name), "Example");
    EXPECT_EQ(manifest.families()[0].fileCount, 2u);
    EXPECT_EQ(manifest.files()[0].size, 1024u);
  }
};

TEST_F(NativeTextFontManifestTest, NativeRecipeExposesCompleteDownloadRequest) {
  ASSERT_EQ(load(), FontManifest::Error::Ok);
  ASSERT_EQ(manifest.families().size(), 1u);
  const auto& family = manifest.families()[0];
  EXPECT_EQ(family.totalSize, 3072u);
  EXPECT_EQ(family.scriptMask, 3u);
  ASSERT_EQ(family.fileCount, 2u);
  const auto& regular = manifest.files()[family.fileStart];
  EXPECT_STREQ(manifest.str(regular.name), "Example-Regular.ttf");
  EXPECT_STREQ(manifest.str(regular.url), "https://raw.githubusercontent.com/fonts/repo/abc/Example.ttf");
  EXPECT_EQ(regular.crc32, UINT32_MAX);
  EXPECT_EQ(regular.style, 0u);
  ASSERT_EQ(regular.axisCount, 2u);
  EXPECT_EQ(regular.axes[0].tag, 0x77676874u);
  EXPECT_FLOAT_EQ(regular.axes[0].value, 400);
  EXPECT_EQ(regular.axes[1].tag, 0x6f70737au);
  EXPECT_FLOAT_EQ(regular.axes[1].value, 14);
  const auto& bold = manifest.files()[family.fileStart + 1];
  EXPECT_EQ(bold.style, 1u);
  EXPECT_EQ(bold.axisCount, 0u);
  EXPECT_EQ(bold.crc32, 0u);  // Zero is a valid CRC, not a missing checksum.
}

TEST_F(NativeTextFontManifestTest, CompiledCatalogueRetainsStaticAndVariableRecipes) {
  ASSERT_EQ(manifest.load(native_text::assets::fontCatalogue, native_text::assets::fontCatalogueSize),
            FontManifest::Error::Ok);
  const FontManifest::Family* inter = nullptr;
  bool foundStatic = false;
  for (const auto& family : manifest.families()) {
    if (!std::strcmp(manifest.str(family.name), "Inter")) inter = &family;
    for (uint32_t index = 0; index < family.fileCount; ++index) {
      const auto& file = manifest.files()[family.fileStart + index];
      if (!file.axisCount) foundStatic = true;
    }
  }
  ASSERT_NE(inter, nullptr);
  EXPECT_TRUE(foundStatic);
  bool regular = false, bold = false;
  for (uint32_t index = 0; index < inter->fileCount; ++index) {
    const auto& file = manifest.files()[inter->fileStart + index];
    bool optical = false, weight = false;
    for (const auto axis : std::span(file.axes, file.axisCount)) {
      if (axis.tag == 0x6f70737au && axis.value == 14) optical = true;
      if (axis.tag == 0x77676874u && axis.value == (file.style & 1 ? 700 : 400)) weight = true;
    }
    EXPECT_TRUE(optical);
    EXPECT_TRUE(weight);
    regular |= file.style == 0;
    bold |= file.style == 1;
  }
  EXPECT_TRUE(regular);
  EXPECT_TRUE(bold);
}

TEST_F(NativeTextFontManifestTest, MalformedLaterFamilyCannotPublishPartialCatalogue) {
  auto invalid = document["families"].as<JsonArray>().add<JsonObject>();
  invalid["name"] = "Bad";
  invalid["files"].to<JsonArray>();
  expectRejectedWithoutReplacingCatalogue();
}

TEST_F(NativeTextFontManifestTest, FamilyAndStyleConfinementPrecedesPublication) {
  document["families"][0]["files"][0]["name"] = "../Example-Regular.ttf";
  expectRejectedWithoutReplacingCatalogue();
  document["families"][0]["files"][0]["name"] = "Other-Regular.ttf";
  expectRejectedWithoutReplacingCatalogue();
  document["families"][0]["files"][0]["name"] = "Example-Bold.ttf";
  expectRejectedWithoutReplacingCatalogue();  // Duplicate style across extensions; no regular.
  document["families"][0]["files"].as<JsonArray>().remove(1);
  expectRejectedWithoutReplacingCatalogue();  // A unique bold file still cannot install alone.
}

TEST_F(NativeTextFontManifestTest, SizeAndChecksumBoundsRejectUnverifiableDownloads) {
  document["families"][0]["files"][0]["size"] = -1;
  expectRejectedWithoutReplacingCatalogue();
  document["families"][0]["files"][0]["size"] = UINT32_MAX;
  expectRejectedWithoutReplacingCatalogue();  // Family byte sum would wrap.
  document["families"][0]["files"][0]["size"] = 1024;
  document["families"][0]["files"][0].remove("crc32");
  expectRejectedWithoutReplacingCatalogue();
}

TEST_F(NativeTextFontManifestTest, RejectsNonHttpsAndAmbiguousSourceAuthorities) {
  document["families"][0]["files"][0]["url"] = "http://example.org/font.ttf";
  expectRejectedWithoutReplacingCatalogue();
  document["families"][0]["files"][0]["url"] = "https://example.org@attacker.test/font.ttf";
  expectRejectedWithoutReplacingCatalogue();
  document["families"][0]["files"][0]["url"] = "https://example.org/\nfont.ttf";
  expectRejectedWithoutReplacingCatalogue();
}

TEST_F(NativeTextFontManifestTest, AxisCountTagsAndFiniteValuesAreBounded) {
  auto axes = document["families"][0]["files"][0]["axes"].to<JsonObject>();
  for (int index = 0; index < 9; ++index) axes["AXI" + std::to_string(index)] = index;
  expectRejectedWithoutReplacingCatalogue();
  axes.clear();
  axes["weight"] = 400;
  expectRejectedWithoutReplacingCatalogue();
  axes.clear();
  axes["wght"] = "400";
  expectRejectedWithoutReplacingCatalogue();
  axes["wght"] = 1e100;
  expectRejectedWithoutReplacingCatalogue();
}

TEST_F(NativeTextFontManifestTest, GroupOverflowOrUnknownMembershipNeverDropsData) {
  document["families"][0]["scripts"][0] = "unknown";
  expectRejectedWithoutReplacingCatalogue();
  document["families"][0]["scripts"][0] = "latin";
  auto groups = document["scriptGroups"].as<JsonArray>();
  for (size_t index = groups.size(); index <= FontManifest::MAX_SCRIPT_GROUPS; ++index) {
    auto group = groups.add<JsonObject>();
    group["tag"] = "group" + std::to_string(index);
    group["label"] = "Group";
  }
  expectRejectedWithoutReplacingCatalogue();
}

TEST_F(NativeTextFontManifestTest, RejectsWrongBackendVersionAndEmbeddedStringTerminator) {
  document["format"] = "cpfont";
  expectRejectedWithoutReplacingCatalogue();
  document["format"] = "opentype";
  document["version"] = 2;
  expectRejectedWithoutReplacingCatalogue();
  document["version"] = 1;
  document["families"][0]["name"] = std::string("Example\0ignored", 15);
  expectRejectedWithoutReplacingCatalogue();
}

TEST_F(NativeTextFontManifestTest, CompleteDocumentRequiredBeforePublishing) {
  ASSERT_EQ(load(), FontManifest::Error::Ok);
  const std::string trailing = std::string(VALID_MANIFEST) + "garbage";
  EXPECT_EQ(manifest.load(trailing.data(), trailing.size()), FontManifest::Error::Invalid);
  const std::string whitespace = std::string(VALID_MANIFEST) + " \r\n\t";
  EXPECT_EQ(manifest.load(whitespace.data(), whitespace.size()), FontManifest::Error::Ok);
  const std::string oversized(FontManifest::MAX_DOCUMENT_BYTES + 1, ' ');
  EXPECT_EQ(manifest.load(oversized.data(), oversized.size()), FontManifest::Error::Invalid);
  EXPECT_EQ(manifest.families()[0].fileCount, 2u);
}

TEST_F(NativeTextFontManifestTest, FileReaderHandlesShortChunksAndRejectsStorageFailure) {
  const auto previous = Storage.root();
  const auto root = std::filesystem::temp_directory_path() /
                    ("native-manifest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Restore {
    std::filesystem::path previous, root;
    ~Restore() {
      Storage.resetFaults();
      Storage.setRoot(previous);
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  } restore{previous, root};
  Storage.setRoot(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream output(root / "fonts.json", std::ios::binary);
    output << VALID_MANIFEST;
    ASSERT_TRUE(output.good());
  }
  Storage.faults().maximumRead = 7;
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/fonts.json", file));
  ASSERT_EQ(manifest.load(file), FontManifest::Error::Ok);
  ASSERT_EQ(manifest.files().size(), 2u);
  EXPECT_EQ(manifest.files()[0].axisCount, 2u);
  file.close();
  Storage.faults().readPath = "/fonts.json";
  Storage.faults().readBytes = 50;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/fonts.json", file));
  EXPECT_EQ(manifest.load(file), FontManifest::Error::StorageError);
  EXPECT_EQ(manifest.files()[0].axisCount, 2u);
}
}  // namespace
