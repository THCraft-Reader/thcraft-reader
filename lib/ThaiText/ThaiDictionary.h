#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace thai {
inline constexpr size_t MAX_DICTIONARY_WORD_CODEPOINTS = 70;
inline constexpr size_t DICTIONARY_BLOCK_WORDS = 16;

struct DictionaryView {
  const uint8_t* data = nullptr;
  size_t dataBytes = 0;
  const uint32_t* offsets = nullptr;
  size_t offsetCount = 0;
  size_t wordCount = 0;
  uint32_t crc = 0;
};

// Views must remain alive and immutable for the lifetime of this accessor.
// Only the generated default view is trusted at construction; injected views
// are validated without copying. Accessor reads remain checked in both cases.
class ThaiDictionary {
 public:
  ThaiDictionary();
  explicit ThaiDictionary(DictionaryView view);
  size_t longestMatch(std::string_view text, size_t maxBytes = SIZE_MAX) const;
  bool valid() const { return !invalid_; }
  bool available() const { return valid() && view_.wordCount != 0; }
  uint32_t dataId() const { return available() ? view_.crc : 0; }

 private:
  DictionaryView view_;
  mutable bool invalid_ = false;

  bool checkHeader() const;
  bool validate() const;
  bool blockBounds(size_t block, size_t& position, size_t& end) const;
  bool decodeEntry(size_t& position, size_t end, bool leader, uint8_t* word, size_t& length) const;
  bool predecessor(std::string_view text, size_t queryLength, size_t& length, size_t& common) const;
};

// Kept out of line so the generated arrays have exactly one owning TU.
uint32_t dictionaryDataId();
}  // namespace thai
