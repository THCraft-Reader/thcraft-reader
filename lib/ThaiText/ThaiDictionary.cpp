#include "ThaiDictionary.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "ThaiCluster.h"
#include "ThaiConfig.h"

#if THAI_DICTIONARY
#include "generated/ThaiDictionaryData.h"
#endif

namespace thai {
namespace {
uint32_t readOffset(const DictionaryView& view, size_t index) {
  uint32_t result;
  // memcpy also permits host fixtures whose offset storage is not aligned.
  std::memcpy(&result, reinterpret_cast<const uint8_t*>(view.offsets) + index * sizeof(result), sizeof(result));
  return result;
}

uint8_t querySymbol(std::string_view text, size_t index) {
  const auto middle = static_cast<uint8_t>(text[index * 3 + 1]);
  const auto last = static_cast<uint8_t>(text[index * 3 + 2]);
  return static_cast<uint8_t>(((middle & 0x3Fu) << 6u | (last & 0x3Fu)) - 0xE00u);
}

// Returns word versus query ordering, retaining the shared-prefix length.
int compare(const uint8_t* word, size_t length, std::string_view text, size_t queryLength, size_t& common) {
  common = 0;
  while (common < length && common < queryLength && word[common] == querySymbol(text, common)) {
    ++common;
  }
  if (common == length || common == queryLength) {
    return length < queryLength ? -1 : length > queryLength ? 1 : 0;
  }
  return word[common] < querySymbol(text, common) ? -1 : 1;
}

bool clusterBoundary(std::string_view text, size_t target) {
  size_t offset = 0;
  Cluster cluster{};
  while (offset < target) {
    if (!nextCluster(text, offset, cluster, true) || !cluster.valid) {
      return false;
    }
  }
  return offset == target;
}
}  // namespace

ThaiDictionary::ThaiDictionary() {
#if THAI_DICTIONARY
  static_assert(generated::MAX_WORD_CODEPOINTS <= MAX_DICTIONARY_WORD_CODEPOINTS);
  static_assert(generated::BLOCK_WORDS == DICTIONARY_BLOCK_WORDS);
  static_assert(sizeof(generated::FIRST_SYMBOL_BLOCKS) / sizeof(uint16_t) == 93);
  static_assert(generated::OFFSET_COUNT - 1 <= std::numeric_limits<uint16_t>::max());
  view_ = {generated::DATA,         generated::DATA_BYTES, generated::OFFSETS,
           generated::OFFSET_COUNT, generated::WORD_COUNT, generated::DATA_ID};
#endif
}

ThaiDictionary::ThaiDictionary(DictionaryView view) : view_(view) { invalid_ = !validate(); }

bool ThaiDictionary::checkHeader() const {
  if (view_.offsetCount > std::numeric_limits<size_t>::max() / sizeof(uint32_t) ||
      view_.dataBytes > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  if (view_.wordCount == 0) {
    return view_.dataBytes == 0 &&
           (view_.offsetCount == 0 || (view_.offsetCount == 1 && view_.offsets && readOffset(view_, 0) == 0));
  }
  const size_t blocks = view_.wordCount / DICTIONARY_BLOCK_WORDS + (view_.wordCount % DICTIONARY_BLOCK_WORDS != 0);
  return view_.data && view_.offsets && view_.dataBytes && view_.offsetCount == blocks + 1 &&
         readOffset(view_, 0) == 0 && readOffset(view_, blocks) == view_.dataBytes;
}

bool ThaiDictionary::blockBounds(size_t block, size_t& position, size_t& end) const {
  if (!view_.offsets || view_.offsetCount < 2 || block >= view_.offsetCount - 1) {
    return false;
  }
  position = readOffset(view_, block);
  end = readOffset(view_, block + 1);
  return position < end && end <= view_.dataBytes;
}

bool ThaiDictionary::decodeEntry(size_t& position, size_t end, bool leader, uint8_t* word, size_t& length) const {
  if (!view_.data || end > view_.dataBytes || position > end || end - position < 2) {
    return false;
  }
  const size_t prefix = view_.data[position++];
  const size_t suffix = view_.data[position++];
  if ((leader && prefix != 0) || prefix > length || suffix == 0 || prefix + suffix > MAX_DICTIONARY_WORD_CODEPOINTS ||
      suffix > end - position) {
    return false;
  }
  for (size_t index = 0; index < suffix; ++index) {
    const uint8_t symbol = view_.data[position++];
    if (symbol == 0 || symbol > 0x5B) {
      return false;
    }
    word[prefix + index] = symbol;
  }
  length = prefix + suffix;
  word[length] = 0;
  return true;
}

bool ThaiDictionary::validate() const {
  if (!checkHeader()) {
    return false;
  }
  uint8_t word[MAX_DICTIONARY_WORD_CODEPOINTS + 1]{};
  uint8_t previous[MAX_DICTIONARY_WORD_CODEPOINTS + 1]{};
  size_t previousLength = 0;
  for (size_t block = 0;
       block < view_.wordCount / DICTIONARY_BLOCK_WORDS + (view_.wordCount % DICTIONARY_BLOCK_WORDS != 0); ++block) {
    size_t position, end;
    if (!blockBounds(block, position, end)) {
      return false;
    }
    size_t length = 0;
    const size_t count = std::min(DICTIONARY_BLOCK_WORDS, view_.wordCount - block * DICTIONARY_BLOCK_WORDS);
    for (size_t index = 0; index < count; ++index) {
      if (!decodeEntry(position, end, index == 0, word, length)) {
        return false;
      }
      if (previousLength) {
        const int order = std::memcmp(previous, word, std::min(previousLength, length));
        if (order > 0 || (order == 0 && previousLength >= length)) {
          return false;
        }
      }
      std::memcpy(previous, word, length);
      previousLength = length;
    }
    if (position != end) {
      return false;
    }
  }
  return true;
}

bool ThaiDictionary::predecessor(std::string_view text, size_t queryLength, size_t& length, size_t& common) const {
  uint8_t word[MAX_DICTIONARY_WORD_CODEPOINTS + 1];
  size_t low = 0;
  size_t high = view_.offsetCount - 1;
#if THAI_DICTIONARY
  if (view_.data == generated::DATA && view_.offsets == generated::OFFSETS) {
    const auto symbol = querySymbol(text, 0);
    const size_t first = generated::FIRST_SYMBOL_BLOCKS[symbol];
    // The preceding block can contain words beginning with this symbol.
    low = first ? first - 1 : 0;
    high = generated::FIRST_SYMBOL_BLOCKS[symbol + 1];
  }
#endif
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    size_t position, end, wordLength = 0, shared;
    if (!blockBounds(middle, position, end) || !decodeEntry(position, end, true, word, wordLength)) {
      return false;
    }
    if (compare(word, wordLength, text, queryLength, shared) <= 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  length = common = 0;
  if (low == 0) {
    return true;
  }
  const size_t block = low - 1;
  size_t position, end, wordLength = 0;
  if (!blockBounds(block, position, end)) {
    return false;
  }
  const size_t count = std::min(DICTIONARY_BLOCK_WORDS, view_.wordCount - block * DICTIONARY_BLOCK_WORDS);
  for (size_t index = 0; index < count; ++index) {
    if (!decodeEntry(position, end, index == 0, word, wordLength)) {
      return false;
    }
    size_t shared;
    if (compare(word, wordLength, text, queryLength, shared) > 0) {
      return true;
    }
    length = wordLength;
    common = shared;
  }
  return position == end;
}

size_t ThaiDictionary::longestMatch(std::string_view text, size_t maxBytes) const {
  if (!available() || maxBytes < 3) {
    return 0;
  }
  if (!checkHeader()) {
    invalid_ = true;
    return 0;
  }
  const size_t limit = std::min({MAX_DICTIONARY_WORD_CODEPOINTS, text.size() / 3, maxBytes / 3});
  size_t queryLength = 0;
  while (queryLength < limit) {
    const size_t offset = queryLength * 3;
    const auto first = static_cast<uint8_t>(text[offset]);
    const auto middle = static_cast<uint8_t>(text[offset + 1]);
    const auto last = static_cast<uint8_t>(text[offset + 2]);
    if (first != 0xE0 ||
        !((middle == 0xB8 && last >= 0x81 && last <= 0xBF) || (middle == 0xB9 && last >= 0x80 && last <= 0x9B))) {
      break;
    }
    ++queryLength;
  }
  while (queryLength) {
    size_t length, common;
    if (!predecessor(text, queryLength, length, common)) {
      invalid_ = true;
      return 0;
    }
    if (length == 0 || common == 0) {
      return 0;
    }
    if (length == common) {
      if (clusterBoundary(text, length * 3)) {
        return length * 3;
      }
      queryLength = length - 1;
    } else {
      queryLength = common;
    }
  }
  return 0;
}

uint32_t dictionaryDataId() {
#if THAI_DICTIONARY
  return generated::DATA_ID;
#else
  return 0;
#endif
}
}  // namespace thai
