#include "ThaiUserWords.h"

#include <algorithm>
#include <cstring>

#include "ThaiCharClass.h"
#include "ThaiCluster.h"
#include "ThaiDictionary.h"

namespace thai {
namespace {
const UserWordsView* activeWords = nullptr;
uint32_t activeId = 0;

// The dictionary's query alphabet (three-byte U+0E01..U+0E5B), as in
// ThaiDictionary.cpp, but checked: text here is not pre-validated.
bool querySymbol(std::string_view text, size_t index, uint8_t& symbol) {
  const auto first = static_cast<uint8_t>(text[index * 3]);
  const auto middle = static_cast<uint8_t>(text[index * 3 + 1]);
  const auto last = static_cast<uint8_t>(text[index * 3 + 2]);
  if (first != 0xE0 ||
      !((middle == 0xB8 && last >= 0x81 && last <= 0xBF) || (middle == 0xB9 && last >= 0x80 && last <= 0x9B))) {
    return false;
  }
  symbol = static_cast<uint8_t>(((middle & 0x3Fu) << 6u | (last & 0x3Fu)) - 0xE00u);
  return true;
}
}  // namespace

bool isUserWordScalar(const uint32_t cp) {
  return cp >= 0x0E01 && cp <= 0x0E4E && cp != 0x0E2F && cp != 0x0E46 && cp != 0x0E3F &&
         !(cp >= 0x0E3B && cp <= 0x0E3E);
}

size_t encodeUserWord(std::string_view utf8, uint8_t* symbols, const size_t capacity) {
  size_t count = 0;
  for (size_t offset = 0; offset < utf8.size();) {
    const auto scalar = detail::decode(utf8, offset, true);
    if (!scalar.bytes || !scalar.valid || !isUserWordScalar(scalar.value)) return 0;
    // A leading mark belongs to the previous word; lookups start at a cluster.
    if (count == 0 && isDependentSign(scalar.value)) return 0;
    if (count == capacity || count == MAX_DICTIONARY_WORD_CODEPOINTS) return 0;
    symbols[count++] = static_cast<uint8_t>(scalar.value - 0x0E00);
    offset += scalar.bytes;
  }
  if (count < MIN_USER_WORD_CODEPOINTS || !isClusterBoundary(utf8, utf8.size())) return 0;
  return count;
}

size_t decodeUserWord(const uint8_t* symbols, const size_t length, char* utf8) {
  size_t bytes = 0;
  for (size_t i = 0; i < length; ++i) {
    const uint32_t cp = 0x0E00u + symbols[i];
    utf8[bytes++] = static_cast<char>(0xE0);
    utf8[bytes++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    utf8[bytes++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  return bytes;
}

std::string_view trimToUserWord(std::string_view text) {
  size_t begin = text.size();
  size_t end = 0;
  for (size_t offset = 0; offset < text.size();) {
    const auto scalar = detail::decode(text, offset, true);
    if (!scalar.bytes) break;
    if (scalar.valid && isUserWordScalar(scalar.value)) {
      begin = std::min(begin, offset);
      end = offset + scalar.bytes;
    }
    offset += scalar.bytes;
  }
  return begin < end ? text.substr(begin, end - begin) : std::string_view{};
}

size_t firstClusterEnd(std::string_view text) {
  size_t offset = 0;
  Cluster cluster{};
  if (!nextCluster(text, offset, cluster, true) || offset >= text.size()) return 0;
  return offset;
}

size_t lastClusterStart(std::string_view text) {
  size_t offset = 0;
  size_t start = 0;
  Cluster cluster{};
  while (offset < text.size()) {
    start = offset;
    if (!nextCluster(text, offset, cluster, true)) return 0;
  }
  return start;
}

bool indexUserWords(UserWordsView& view, uint16_t* sortedStorage) {
  view.sorted = sortedStorage;
  view.count = 0;
  view.id = 0;
  std::fill(std::begin(view.buckets), std::end(view.buckets), uint16_t{0});
  if (view.wordBytes > MAX_USER_WORD_BYTES || (view.wordBytes && (!view.words || !sortedStorage))) return false;

  const uint8_t* words = view.words;
  uint16_t count = 0;
  for (size_t position = 0; position < view.wordBytes;) {
    const uint8_t length = words[position];
    if (length < MIN_USER_WORD_CODEPOINTS || length > MAX_DICTIONARY_WORD_CODEPOINTS ||
        position + 1 + length > view.wordBytes || count == MAX_USER_WORDS) {
      return false;
    }
    for (size_t i = 1; i <= length; ++i) {
      if (words[position + i] == 0 || words[position + i] >= USER_WORD_SYMBOL_END) return false;
    }
    sortedStorage[count++] = static_cast<uint16_t>(position);
    position += 1 + length;
  }

  // First symbol ascending, then longest first so a lookup stops at its first hit.
  std::sort(sortedStorage, sortedStorage + count, [words](const uint16_t a, const uint16_t b) {
    const uint8_t* left = words + a;
    const uint8_t* right = words + b;
    if (left[1] != right[1]) return left[1] < right[1];
    if (left[0] != right[0]) return left[0] > right[0];
    return std::memcmp(left + 1, right + 1, left[0]) < 0;
  });
  uint16_t index = 0;
  for (size_t symbol = 0; symbol <= USER_WORD_SYMBOL_END; ++symbol) {
    while (index < count && words[sortedStorage[index] + 1] < symbol) ++index;
    view.buckets[symbol] = index;
  }

  uint32_t hash = 2166136261u;
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* word = words + sortedStorage[i];
    for (size_t j = 0; j <= word[0]; ++j) hash = (hash ^ word[j]) * 16777619u;
  }
  view.count = count;
  view.id = count == 0 ? 0 : (hash ? hash : 1);
  return true;
}

void setUserWords(const UserWordsView* view) {
  activeWords = view && view->count ? view : nullptr;
  activeId = activeWords ? activeWords->id : 0;
}

uint32_t userWordsId() { return activeId; }

size_t longestUserWord(std::string_view text, const size_t maxBytes) {
  const UserWordsView* view = activeWords;
  if (!view || maxBytes < 3) return 0;
  uint8_t query[MAX_DICTIONARY_WORD_CODEPOINTS];
  if (text.size() < 3 || !querySymbol(text, 0, query[0])) return 0;
  const uint8_t first = query[0];
  const uint16_t begin = view->buckets[first];
  const uint16_t end = view->buckets[first + 1];
  if (begin == end) return 0;  // the common case: no word starts with this letter

  // The bucket's first word is its longest; decode no further than that.
  const size_t longest = view->words[view->sorted[begin]];
  const size_t limit = std::min({longest, text.size() / 3, maxBytes / 3});
  size_t length = 1;
  while (length < limit && querySymbol(text, length, query[length])) ++length;
  for (uint16_t i = begin; i < end; ++i) {
    const uint8_t* word = view->words + view->sorted[i];
    if (word[0] > length || std::memcmp(word + 1, query, word[0]) != 0) continue;
    if (isClusterBoundary(text, word[0] * size_t{3})) return word[0] * size_t{3};
  }
  return 0;
}
}  // namespace thai
