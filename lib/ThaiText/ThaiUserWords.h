#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ThaiDictionary.h"

namespace thai {
// Bounds for the reader's own word list. Words are stored in the dictionary's
// one-byte symbol form (codepoint - U+0E00) with a length byte each, so the byte
// cap holds several hundred typical names.
inline constexpr size_t MAX_USER_WORDS = 300;
inline constexpr size_t MAX_USER_WORD_BYTES = 4096;
// Shortest useful word, in codepoints; one letter adds nothing to segmentation.
inline constexpr size_t MIN_USER_WORD_CODEPOINTS = 2;
// Symbols are below this value (U+0E01..U+0E5B); one bucket per first symbol.
inline constexpr uint8_t USER_WORD_SYMBOL_END = 0x5C;

// Read-only index over caller-owned storage. `words` holds length-prefixed symbol
// strings in list order. `sorted` holds their offsets grouped by first symbol,
// longest first; buckets[s] .. buckets[s + 1] spans the words starting with s.
struct UserWordsView {
  const uint8_t* words = nullptr;
  size_t wordBytes = 0;
  const uint16_t* sorted = nullptr;
  uint16_t count = 0;
  uint16_t buckets[USER_WORD_SYMBOL_END + 1] = {};
  // Identity of the word set, independent of list order; nonzero when count > 0.
  uint32_t id = 0;
};

// Thai letters and signs a user word may contain: no digits, Thai punctuation,
// repetition or abbreviation marks.
bool isUserWordScalar(uint32_t cp);

// Converts UTF-8 to symbols. Returns the symbol count, or 0 unless the whole input
// is valid Thai letter clusters of MIN_USER_WORD_CODEPOINTS up to the dictionary's
// word length, using only user word scalars.
size_t encodeUserWord(std::string_view utf8, uint8_t* symbols, size_t capacity);
// Writes symbols back as UTF-8 (3 bytes each, no terminator); returns the bytes.
size_t decodeUserWord(const uint8_t* symbols, size_t length, char* utf8);

// The part of text left after dropping scalars that cannot be in a user word from
// both ends (spaces, quotes, punctuation). It may still be invalid inside.
std::string_view trimToUserWord(std::string_view text);

// Byte end of the first cluster and byte start of the last cluster of text, or 0
// when text has fewer than two clusters (nothing could be trimmed).
size_t firstClusterEnd(std::string_view text);
size_t lastClusterStart(std::string_view text);

// Fills sorted, buckets and id over view.words and view.wordBytes. sortedStorage
// needs room for every word. False when the words are malformed or over the bounds.
bool indexUserWords(UserWordsView& view, uint16_t* sortedStorage);

// The list that default ThaiDictionary instances consult (nullptr for none). The
// caller keeps the view alive while installed and must not swap it while text is
// being laid out; the firmware holds the render lock for both.
void setUserWords(const UserWordsView* view);
uint32_t userWordsId();

// Longest installed user word that starts text, fits maxBytes and ends on a
// cluster boundary, in bytes; 0 when none.
size_t longestUserWord(std::string_view text, size_t maxBytes);
}  // namespace thai
