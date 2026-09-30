#include "ThaiLayoutId.h"

#include "ThaiConfig.h"
#if THAI_DICTIONARY
#include "ThaiDictionary.h"
#endif

namespace thai {
uint32_t layoutId() {
  uint32_t hash = 2166136261u;
  const auto byte = [&hash](uint8_t value) { hash = (hash ^ value) * 16777619u; };
  const auto littleEndianWord = [&byte](uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) byte(static_cast<uint8_t>(value >> shift));
  };
  littleEndianWord(3);  // Analyzer and line-breaking contract version.
#if THAI_DICTIONARY
  littleEndianWord(dictionaryDataId());
#else
  littleEndianWord(0);
#endif
  byte(THAI_WORD_BREAKING ? 1 : 0);
  byte(THAI_DICTIONARY ? 1 : 0);
  return hash & ~ANALYSIS_UNAVAILABLE_LAYOUT_BIT;
}
}  // namespace thai
