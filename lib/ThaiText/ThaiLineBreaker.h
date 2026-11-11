#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace thai {
enum class BreakKind : uint8_t { Space = 0, Word = 1, Punctuation = 2, Emergency = 3, Prohibited = 255 };

bool isOpeningPunctuation(uint32_t cp);
bool isClosingPunctuation(uint32_t cp);
// Pairwise protection only: widths, real whitespace and dictionary ranks belong
// to the caller. U+200B is an explicit opportunity handled by the parser.
bool prohibitsBreak(uint32_t left, uint32_t right);
// Recognizes one punctuation unit, including ... and ฯลฯ. Returns its byte
// length, or zero for non-punctuation / an undecidable soft-edge prefix.
size_t punctuationUnitBytes(std::string_view text, size_t offset, bool endOfRun);
}  // namespace thai
