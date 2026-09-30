#include <ThaiConfig.h>
#include <ThaiDictionary.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Fixture {
  std::vector<uint8_t> data;
  std::vector<uint32_t> offsets;
  size_t wordCount;

  explicit Fixture(std::vector<std::string> words) {
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    wordCount = words.size();
    std::vector<uint8_t> previous;
    for (size_t index = 0; index < words.size(); ++index) {
      std::vector<uint8_t> symbols;
      for (size_t byte = 0; byte < words[index].size(); byte += 3) {
        symbols.push_back(static_cast<uint8_t>(((static_cast<uint8_t>(words[index][byte + 1]) & 0x3F) << 6 |
                                               (static_cast<uint8_t>(words[index][byte + 2]) & 0x3F)) - 0xE00));
      }
      size_t prefix = 0;
      if (index % thai::DICTIONARY_BLOCK_WORDS == 0) {
        offsets.push_back(static_cast<uint32_t>(data.size()));
      } else {
        while (prefix < std::min(previous.size(), symbols.size()) && previous[prefix] == symbols[prefix]) {
          ++prefix;
        }
      }
      data.push_back(static_cast<uint8_t>(prefix));
      data.push_back(static_cast<uint8_t>(symbols.size() - prefix));
      data.insert(data.end(), symbols.begin() + prefix, symbols.end());
      previous = std::move(symbols);
    }
    offsets.push_back(static_cast<uint32_t>(data.size()));
  }

  thai::DictionaryView view() const { return {data.data(), data.size(), offsets.data(), offsets.size(), wordCount, 123}; }
};

TEST(ThaiDictionary, TerminalPrefixAndMaximumByteLimit) {
  Fixture fixture({"ก", "กข", "กขค", "ข"});
  thai::ThaiDictionary dictionary(fixture.view());
  ASSERT_TRUE(dictionary.valid());
  EXPECT_EQ(dictionary.longestMatch("กขคง"), std::string_view("กขค").size());
  EXPECT_EQ(dictionary.longestMatch("กขคง", 8), std::string_view("กข").size());
  EXPECT_EQ(dictionary.longestMatch("กขคง", 5), std::string_view("ก").size());
  EXPECT_EQ(dictionary.longestMatch("กขคง", 2), 0u);
  EXPECT_EQ(dictionary.longestMatch("ง"), 0u);
}

TEST(ThaiDictionary, FullLookaheadRejectsArtificialClusterEnd) {
  Fixture fixture({"ก", "กข", "เก", "เรื"});
  thai::ThaiDictionary dictionary(fixture.view());
  EXPECT_EQ(dictionary.longestMatch("ก่"), 0u);
  EXPECT_EQ(dictionary.longestMatch("ก่", 3), 0u);
  EXPECT_EQ(dictionary.longestMatch("กข่", 6), std::string_view("ก").size());
  EXPECT_EQ(dictionary.longestMatch("เก่ง", 6), 0u);
  EXPECT_EQ(dictionary.longestMatch("เรื่อง", 9), 0u);
}

TEST(ThaiDictionary, LongestWordCrossesOldParserBuffer) {
  std::string word;
  for (size_t i = 0; i < 70; ++i) word += "ก";
  Fixture fixture({"ก", word});
  thai::ThaiDictionary dictionary(fixture.view());
  EXPECT_EQ(dictionary.longestMatch(word + "ข"), 210u);
  EXPECT_EQ(dictionary.longestMatch(word + "ข", 209), 3u);
}

TEST(ThaiDictionary, CommonPrefixRetryFindsEarlierWordAcrossBlocks) {
  std::vector<std::string> words{"ก"};
  for (uint8_t symbol = 2; symbol <= 40; ++symbol) {
    std::string word = "ก";
    word += static_cast<char>(0xE0);
    word += static_cast<char>(0xB8);
    word += static_cast<char>(0x80 + symbol);
    word += "ข";
    words.push_back(word);
  }
  Fixture fixture(words);
  thai::ThaiDictionary dictionary(fixture.view());
  // A later predecessor shares กข but is not a prefix: retry must find ก.
  EXPECT_EQ(dictionary.longestMatch("กขค"), 3u);
  for (const auto& word : words) {
    // Some fixture entries end inside orthographic clusters only when followed
    // by marks; exact dictionary words without extra signs remain terminals.
    EXPECT_EQ(dictionary.longestMatch(word), word.size()) << word;
  }
}

TEST(ThaiDictionary, EmptyMissingAndMalformedViewsAreSafe) {
  thai::ThaiDictionary empty(thai::DictionaryView{});
  EXPECT_TRUE(empty.valid());
  EXPECT_FALSE(empty.available());
  EXPECT_EQ(empty.dataId(), 0u);
  EXPECT_EQ(empty.longestMatch("ภาษาไทย"), 0u);
  Fixture fixture({"ก", "ข"});
  auto view = fixture.view();
  view.data = nullptr;
  thai::ThaiDictionary missing(view);
  EXPECT_FALSE(missing.valid());
  EXPECT_EQ(missing.longestMatch("ก"), 0u);
  view = fixture.view();
  view.offsetCount = 1;
  EXPECT_FALSE(thai::ThaiDictionary(view).valid());
  view = fixture.view();
  view.wordCount = SIZE_MAX;
  EXPECT_FALSE(thai::ThaiDictionary(view).valid());
  fixture.offsets.back() = static_cast<uint32_t>(fixture.data.size() + 1);
  EXPECT_FALSE(thai::ThaiDictionary(fixture.view()).valid());
}

TEST(ThaiDictionary, CorruptEncodingNeverPublishesPartialMatches) {
  for (const std::vector<uint8_t>& bytes : std::vector<std::vector<uint8_t>>{
           {1, 1, 1}, {0, 71, 1}, {0, 2, 1}, {0, 1, 0}, {0, 1, 0x5C},
           {0, 1, 1, 2, 1, 2}, {0, 1, 2, 0, 1, 1}, {0, 1, 1, 1, 0}}) {
    const uint32_t offsets[] = {0, static_cast<uint32_t>(bytes.size())};
    thai::ThaiDictionary dictionary({bytes.data(), bytes.size(), offsets, 2, bytes.size() > 4 ? 2u : 1u, 0});
    EXPECT_FALSE(dictionary.valid());
    EXPECT_FALSE(dictionary.available());
    EXPECT_EQ(dictionary.longestMatch("กข"), 0u);
  }
}

TEST(ThaiDictionary, InvalidUtf8AndNonThaiRemainOutsideDictionary) {
  Fixture fixture({"ก"});
  thai::ThaiDictionary dictionary(fixture.view());
  EXPECT_EQ(dictionary.longestMatch("ASCIIก"), 0u);
  EXPECT_EQ(dictionary.longestMatch(std::string("\xE0\xB8", 2)), 0u);
  EXPECT_EQ(dictionary.longestMatch(std::string("\xE0\xB8\xFF", 3)), 0u);
  EXPECT_EQ(dictionary.longestMatch("กASCII"), 3u);
}

TEST(ThaiDictionary, ProductionSupplementAndCompoundRemainLexicalData) {
  thai::ThaiDictionary dictionary;
#if THAI_DICTIONARY
  ASSERT_TRUE(dictionary.available());
  EXPECT_EQ(dictionary.dataId(), thai::dictionaryDataId());
  const std::string_view input = "ประเทศไทยมีประชากรจำนวนมาก";
  const std::vector<std::string_view> expected = {"ประเทศไทย", "มี", "ประชากร", "จำนวนมาก"};
  size_t offset = 0;
  for (const auto word : expected) {
    const size_t matched = dictionary.longestMatch(input.substr(offset));
    ASSERT_EQ(matched, word.size()) << word;
    EXPECT_EQ(input.substr(offset, matched), word);
    offset += matched;
  }
  EXPECT_EQ(offset, input.size());
  EXPECT_EQ(dictionary.longestMatch("จำนวนมาก", std::string_view("จำนวนมาก").size() - 1),
            std::string_view("จำนวน").size());
  EXPECT_EQ(dictionary.longestMatch("มาก"), std::string_view("มาก").size());
#else
  EXPECT_TRUE(dictionary.valid());
  EXPECT_FALSE(dictionary.available());
  EXPECT_EQ(dictionary.longestMatch("ประเทศไทย"), 0u);
  EXPECT_EQ(thai::dictionaryDataId(), 0u);
#endif
}
}  // namespace
