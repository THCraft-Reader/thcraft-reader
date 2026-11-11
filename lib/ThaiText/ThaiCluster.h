#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ThaiCharClass.h"
#include "ThaiConfig.h"
#include "ThaiLineBreaker.h"

namespace thai {
struct Cluster {
  size_t begin;
  size_t end;
  uint16_t codepoints;
  bool valid;
};

bool containsThai(std::string_view text);
// Byte offsets into text. False leaves offset/result unchanged when the trailing
// cluster needs more input. True always advances; invalid UTF-8 and pathological
// sign runs are returned unchanged in bounded fragments with valid=false.
bool nextCluster(std::string_view text, size_t& offset, Cluster& result, bool endOfRun);
// Last complete cluster end <= limit, evaluated against the full input (never a
// sliced prefix that could manufacture a boundary). Returns zero if none fits.
size_t lastSafeBoundary(std::string_view text, size_t limit, bool endOfRun);

// Only well-formed Thai letter clusters participate in internal justification.
bool isJustifiableLetterCluster(std::string_view text, const Cluster& cluster);

// Allocation-free, source-preserving spacing traversal, independent of breaks.
// Offsets separate visible letter units; base/mark and Sara Am recipes stay rigid.
class JustificationBoundaryCursor {
 public:
  explicit JustificationBoundaryCursor(std::string_view text) : text_(text) {}
  bool next(size_t& byteOffset);

 private:
  std::string_view text_;
  size_t offset_ = 0;
  size_t clusterEnd_ = 0;
  size_t unitOffset_ = 0;
  bool previousEligible_ = false;
};
}  // namespace thai
