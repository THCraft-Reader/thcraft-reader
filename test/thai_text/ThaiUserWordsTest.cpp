#include <ThaiDictionary.h>
#include <ThaiLayoutId.h>
#include <ThaiSegmenter.h>
#include <ThaiUserWords.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
// Packs UTF-8 words into the length-prefixed symbol form and installs them for
// the lifetime of the object.
class InstalledWords {
 public:
  explicit InstalledWords(const std::vector<std::string>& words) {
    for (const auto& word : words) {
      uint8_t symbols[thai::MAX_DICTIONARY_WORD_CODEPOINTS];
      const size_t count = thai::encodeUserWord(word, symbols, sizeof(symbols));
      EXPECT_NE(count, 0u) << word;
      packed_.push_back(static_cast<uint8_t>(count));
      packed_.insert(packed_.end(), symbols, symbols + count);
    }
    sorted_.resize(words.size());
    view_.words = packed_.data();
    view_.wordBytes = packed_.size();
    indexed_ = thai::indexUserWords(view_, sorted_.data());
    thai::setUserWords(&view_);
  }
  ~InstalledWords() { thai::setUserWords(nullptr); }
  InstalledWords(const InstalledWords&) = delete;
  InstalledWords& operator=(const InstalledWords&) = delete;

  bool indexed() const { return indexed_; }
  const thai::UserWordsView& view() const { return view_; }

 private:
  std::vector<uint8_t> packed_;
  std::vector<uint16_t> sorted_;
  thai::UserWordsView view_;
  bool indexed_ = false;
};

size_t encode(std::string_view word) {
  uint8_t symbols[thai::MAX_DICTIONARY_WORD_CODEPOINTS];
  return thai::encodeUserWord(word, symbols, sizeof(symbols));
}
}  // namespace

TEST(ThaiUserWords, EncodeAcceptsOnlyWholeThaiWords) {
  EXPECT_EQ(encode("สมชาย"), 5u);
  EXPECT_EQ(encode("กรรณิการ์"), 9u);
  EXPECT_EQ(encode("ก"), 0u);        // one letter
  EXPECT_EQ(encode("สม ชาย"), 0u);   // space inside
  EXPECT_EQ(encode("Somchai"), 0u);  // not Thai
  EXPECT_EQ(encode("ปี๒๕"), 0u);      // Thai digits
  EXPECT_EQ(encode("ไปๆ"), 0u);      // repetition mark
  EXPECT_EQ(encode("ิสม"), 0u);       // starts with a mark
  EXPECT_EQ(encode(""), 0u);
  std::string longest;
  for (size_t i = 0; i < thai::MAX_DICTIONARY_WORD_CODEPOINTS; ++i) longest += "ก";
  EXPECT_EQ(encode(longest), thai::MAX_DICTIONARY_WORD_CODEPOINTS);
  EXPECT_EQ(encode(longest + "ก"), 0u);
}

TEST(ThaiUserWords, DecodeRoundTripsEncode) {
  const std::string word = "กรรณิการ์";
  uint8_t symbols[thai::MAX_DICTIONARY_WORD_CODEPOINTS];
  const size_t count = thai::encodeUserWord(word, symbols, sizeof(symbols));
  ASSERT_NE(count, 0u);
  char utf8[thai::MAX_DICTIONARY_WORD_CODEPOINTS * 3];
  EXPECT_EQ(std::string(utf8, thai::decodeUserWord(symbols, count, utf8)), word);
}

TEST(ThaiUserWords, TrimDropsSurroundingPunctuation) {
  EXPECT_EQ(thai::trimToUserWord(" “สมชาย”, "), "สมชาย");
  EXPECT_EQ(thai::trimToUserWord("(สมชาย)!"), "สมชาย");
  EXPECT_EQ(thai::trimToUserWord("สม ชาย"), "สม ชาย");  // inner space stays; encode rejects it
  EXPECT_EQ(thai::trimToUserWord("..."), "");
}

TEST(ThaiUserWords, ClusterTrimPoints) {
  const std::string_view word = "สมชาย";
  EXPECT_EQ(word.substr(thai::firstClusterEnd(word)), "มชาย");
  EXPECT_EQ(word.substr(0, thai::lastClusterStart(word)), "สมชา");
  EXPECT_EQ(thai::firstClusterEnd("กี่"), 0u);  // a single cluster cannot be trimmed
  EXPECT_EQ(thai::lastClusterStart("กี่"), 0u);
}

TEST(ThaiUserWords, IndexRejectsMalformedStorage) {
  const uint8_t truncated[] = {5, 1, 2};
  uint16_t sorted[4];
  thai::UserWordsView view;
  view.words = truncated;
  view.wordBytes = sizeof(truncated);
  EXPECT_FALSE(thai::indexUserWords(view, sorted));

  const uint8_t badSymbol[] = {2, 1, thai::USER_WORD_SYMBOL_END};
  view.words = badSymbol;
  view.wordBytes = sizeof(badSymbol);
  EXPECT_FALSE(thai::indexUserWords(view, sorted));
}

TEST(ThaiUserWords, LongestMatchPrefersLongestWordOnClusterBoundary) {
  InstalledWords words({"ฆฬ", "ฆฬฎฐ", "ฬฎ"});
  ASSERT_TRUE(words.indexed());
  EXPECT_EQ(thai::longestUserWord("ฆฬฎฐก", SIZE_MAX), std::string_view("ฆฬฎฐ").size());
  EXPECT_EQ(thai::longestUserWord("ฆฬฎฐก", 9), std::string_view("ฆฬ").size());
  EXPECT_EQ(thai::longestUserWord("ฬฎฐ", SIZE_MAX), std::string_view("ฬฎ").size());
  EXPECT_EQ(thai::longestUserWord("ฐฆฬ", SIZE_MAX), 0u);
  // "ฆฬ" followed by a vowel sign does not end on a cluster boundary.
  EXPECT_EQ(thai::longestUserWord("ฆฬิ", SIZE_MAX), 0u);
}

TEST(ThaiUserWords, IdIgnoresListOrderAndClearsWithList) {
  uint32_t first = 0;
  {
    InstalledWords words({"ฆฬฎฐ", "ฬฎ"});
    first = thai::userWordsId();
    EXPECT_NE(first, 0u);
  }
  EXPECT_EQ(thai::userWordsId(), 0u);
  InstalledWords reordered({"ฬฎ", "ฆฬฎฐ"});
  EXPECT_EQ(thai::userWordsId(), first);
}

TEST(ThaiUserWords, DefaultDictionaryAndSegmenterUseInstalledWords) {
  const std::string name = "ฆฬฎฐ";
  const thai::ThaiDictionary dictionary;
  const size_t builtin = dictionary.longestBuiltinMatch(name);
  ASSERT_LT(builtin, name.size());
  const uint32_t baseLayout = thai::layoutId();

  {
    InstalledWords words({name});
    EXPECT_EQ(dictionary.longestMatch(name), name.size());
    EXPECT_EQ(dictionary.longestBuiltinMatch(name), builtin);
    EXPECT_NE(thai::layoutId(), baseLayout);

    size_t offset = 0;
    thai::Segment segment{};
    ASSERT_TRUE(thai::nextSegment(name, offset, segment, true, dictionary));
    EXPECT_EQ(segment.end, name.size());
    EXPECT_TRUE(segment.known);
  }
  EXPECT_EQ(thai::layoutId(), baseLayout);
}
