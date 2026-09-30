#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ThaiDictionary.h"
#include "ThaiLineBreaker.h"

namespace thai {
struct Segment {
  size_t begin;
  size_t end;
  uint16_t codepoints;
  BreakKind before;
  BreakKind after;
  bool known;
  bool valid;
};

// Byte offsets into text; false leaves offset and result unchanged. Thai words
// wait for up to 126 scalars (24 repair + 70 dictionary + 32 cluster), or an
// actual run boundary. Greedy spans with unknown/single-scalar segments use
// bounded, allocation-free DP; clean spans keep longest matching.
// Unknown text emits one complete cluster with Emergency ranks, never an
// artificial Word boundary at the end of a window. A caller carries the previous
// segment's rank (so a known word ends at Word), with Prohibited on either side
// taking precedence. Punctuation is emitted as separately protected units.
//
// Generic text is returned in bounded prefixes with Prohibited/Prohibited ranks;
// it must go through the existing generic tokenizer, not the Thai token path.
// The caller owns generic/URL mode across discarded windows. A full http://,
// https:// or www. run is recognized here while its prefix remains visible.
// Whitespace and explicit-break semantics remain the parser's responsibility.
//
// valid describes source clusters, not dictionary availability. Empty/invalid
// dictionaries give cluster-safe fallback; dictionary.valid() exposes corruption
// to the caller without turning valid source text into an error.
bool nextSegment(std::string_view text, size_t& offset, Segment& result, bool endOfRun,
                 const ThaiDictionary& dictionary);

// Longest proper dictionary prefix at or before maxBytes whose remaining suffix
// is entirely covered by left-to-right longest dictionary matches. No backtracking
// or emergency cluster cuts are used to make a suffix appear dictionary-covered.
// All candidate ends are checked against the original token's cluster context.
size_t dictionarySplit(std::string_view token, size_t maxBytes, const ThaiDictionary& dictionary);
}  // namespace thai
