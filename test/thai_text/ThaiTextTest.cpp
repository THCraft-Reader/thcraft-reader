#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"

namespace {

struct Token {
  std::string text;
  EpdFontFamily::Style style;
  bool operator==(const Token& other) const { return text == other.text && style == other.style; }
};
using Lines = std::vector<std::vector<Token>>;

class ThaiTextTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;
  std::filesystem::path path;

  void SetUp() override {
    const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("thcraft-thai-" + std::to_string(serial) + ".xhtml");
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  Lines parse(const std::string& body, uint16_t width = 480, size_t chunkBytes = 0,
              bool hyphenation = false) {
    const std::string xml = "<html><head/><body>" + body + "</body></html>";
    {
      std::ofstream output(path, std::ios::binary);
      output.write(xml.data(), static_cast<std::streamsize>(xml.size()));
      EXPECT_TRUE(output.good());
    }
    Lines result;
    const std::string filename = path.string();
    CssParser cssParser{path.parent_path().string()};
    ChapterHtmlSlimParser parser{
        nullptr, filename, renderer, 0, 1.0f, false, static_cast<uint8_t>(CssTextAlign::Left),
        width, 800, hyphenation, false,
        [&](std::unique_ptr<Page> page, auto, auto, auto) {
          if (!page) return;
          for (const auto& element : page->elements) {
            if (element->getTag() != TAG_PageLine) continue;
            const auto* block = static_cast<const PageLine&>(*element).getBlock();
            EXPECT_TRUE(block->valid());
            auto& line = result.emplace_back();
            for (uint16_t i = 0; i < block->wordCount(); ++i) {
              line.push_back({block->wordText(i), block->wordStyle(i)});
            }
          }
        },
        true, "", "", 0, {}, nullptr, &cssParser};
    if (chunkBytes == 0) {
      EXPECT_TRUE(parser.parseAndBuildPages());
    } else {
      if (!parser.beginParse()) {
        ADD_FAILURE() << "Could not initialize production parser";
        return result;
      }
      for (size_t offset = 0; offset < xml.size(); offset += chunkBytes) {
        const size_t count = std::min(chunkBytes, xml.size() - offset);
        if (XML_Parse(parser.xmlParser_, xml.data() + offset, static_cast<int>(count),
                      offset + count == xml.size()) != XML_STATUS_OK) {
          ADD_FAILURE() << XML_ErrorString(XML_GetErrorCode(parser.xmlParser_));
          parser.abortParse();
          return result;
        }
      }
      EXPECT_TRUE(parser.finishParse());
    }
    return result;
  }

  static std::string joined(const Lines& lines) {
    std::string result;
    for (const auto& line : lines) {
      for (const auto& token : line) result += token.text;
    }
    return result;
  }

  static std::vector<std::string> lineText(const Lines& lines) {
    std::vector<std::string> result;
    for (const auto& line : lines) {
      auto& text = result.emplace_back();
      for (const auto& token : line) text += token.text;
    }
    return result;
  }
};

// These assert consumer-visible preservation, not legacy token-count details.
// Fixed host metrics are suitable for semantics, never Thai visual acceptance.
TEST_F(ThaiTextTest, PreservesComposedAndDecomposedThaiSourceBytes) {
  for (const char* text : {"กี่", "กุ่", "น้ำ", "นํ้า", "เรื่อง", "ผู้หญิง"}) {
    SCOPED_TRACE(text);
    EXPECT_EQ(joined(parse(std::string("<p>") + text + "</p>")), text);
  }
}

TEST_F(ThaiTextTest, ExpatInputSplitsPreserveShortThaiParagraph) {
  const std::string text = "เก่งน้ำเรื่องประเทศไทย";
  const std::string body = "<p>" + text + "</p>";
  const auto whole = parse(body);
  EXPECT_EQ(joined(whole), text);
  for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{199}, size_t{200}, size_t{201}}) {
    SCOPED_TRACE(chunk);
    EXPECT_EQ(parse(body, 480, chunk), whole);
  }
}

TEST_F(ThaiTextTest, ExplicitBreakKeepsBothThaiRuns) {
  EXPECT_EQ(lineText(parse("<p>ภาษา<br/>ไทย</p>")), (std::vector<std::string>{"ภาษา", "ไทย"}));
}

TEST_F(ThaiTextTest, MixedVersionTimeAndUrlRetainBytes) {
  const std::string input = "EpubCraft เป็น EPUB Editor Version 4.2.0 บทที่ 12 เวลา 02:40 น. "
                            "https://example.org/ภาษาไทย?q=12";
  std::string expected = input;
  expected.erase(std::remove(expected.begin(), expected.end(), ' '), expected.end());
  EXPECT_EQ(joined(parse("<p>" + input + "</p>")), expected);
}

TEST_F(ThaiTextTest, VietnameseNfcControlUsesExistingNormalization) {
  EXPECT_EQ(joined(parse("<p>Tie\xCC\x82\xCC\x81ng a\xCC\x82\xCC\x81 a\xCC\xA3\xCC\x82</p>")),
            "Tiếngấậ");
}

TEST_F(ThaiTextTest, DictionaryWrapMovesHospitalWordIntact) {
  const std::string prefix = "วันนี้ผมเดินทางไป";
  const std::string text = prefix + "โรงพยาบาล";
  const auto width = renderer.getTextAdvanceX(0, prefix.c_str(), EpdFontFamily::REGULAR);
  for (const bool hyphenation : {false, true}) {
    const auto lines = lineText(parse("<p style=\"text-indent:0\">" + text + "</p>", width, 0, hyphenation));
    EXPECT_EQ(lines, (std::vector<std::string>{prefix, "โรงพยาบาล"}));
  }
}

TEST_F(ThaiTextTest, ClusterAcrossBoldBoundaryTakesBaseStyle) {
  const auto lines = parse("<p><b>ก</b>ี่</p>");
  ASSERT_EQ(joined(lines), "กี่");
  for (const auto& line : lines) {
    for (const auto& token : line) EXPECT_EQ(token.style, EpdFontFamily::BOLD);
  }
}

TEST_F(ThaiTextTest, OverwideClustersRemainIntactWithoutInsertedHyphens) {
  for (const char* cluster : {"กี่", "กุ่", "น้ำ", "นํ้า", "เรื่อ"}) {
    for (bool hyphenation : {false, true}) {
      for (uint16_t width = 1; width <= 40; ++width) {
        SCOPED_TRACE(cluster);
        SCOPED_TRACE(width);
        const auto lines = lineText(parse(std::string("<p>") + cluster + "ก</p>", width, 0, hyphenation));
        std::string rendered;
        for (const auto& line : lines) rendered += line;
        EXPECT_EQ(rendered, std::string(cluster) + "ก");
        ASSERT_FALSE(lines.empty());
        EXPECT_EQ(lines.front().find(cluster), 0u);
      }
    }
  }
}

TEST_F(ThaiTextTest, BufferBoundaryDoesNotSplitMarkedCluster) {
  // 198 bytes before the 9-byte cluster: the existing generic raw buffer is 200.
  const std::string prefix(198, 'x');
  const auto lines = lineText(parse("<p>" + prefix + "กี่</p>", 536));
  ASSERT_FALSE(lines.empty());
  std::string rendered;
  for (const auto& line : lines) rendered += line;
  EXPECT_EQ(rendered, prefix + "กี่");
  bool completeCluster = false;
  for (const auto& line : lines) completeCluster |= line.find("กี่") != std::string::npos;
  EXPECT_TRUE(completeCluster);
}

}  // namespace
