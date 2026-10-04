// SPDX-FileCopyrightText: 2016-2026 PyThaiNLP Project
// SPDX-License-Identifier: Apache-2.0
// Modified: bounded C++ predicate port, streaming tri-state matching, vowel
// correction and dependent-sign protection. See TCC-NOTICE and LICENSE-TCC.
#include "ThaiCluster.h"

namespace thai {
namespace {
constexpr uint16_t INVALID = 0xFFFF;
enum class MatchState : uint8_t { No, Yes, More };

// Thai predicates need only BMP values. Other valid scalars use a nonmatching
// sentinel; byte ends still retain their exact original width. No source copy.
struct Window {
  uint16_t cp[MAX_CLUSTER_CODEPOINTS];
  uint8_t ends[MAX_CLUSTER_CODEPOINTS];
  uint8_t count = 0;
  bool final = false;

  void load(std::string_view text, size_t offset, bool endOfRun) {
    size_t bytes = 0;
    while (count < MAX_CLUSTER_CODEPOINTS && offset + bytes < text.size()) {
      const auto scalar = detail::decode(text, offset + bytes, endOfRun);
      if (!scalar.bytes) break;
      bytes += scalar.bytes;
      cp[count] = !scalar.valid ? INVALID : scalar.value >= 0xFFFF ? 0xFFFE : scalar.value;
      ends[count++] = static_cast<uint8_t>(bytes);
    }
    final = endOfRun && offset + bytes == text.size();
  }
};

struct Match {
  const Window& window;
  uint8_t pos;
  MatchState state = MatchState::Yes;

  Match& range(uint16_t low, uint16_t high, bool optional = false) {
    if (state != MatchState::Yes) return *this;
    if (pos == window.count) {
      if (!window.final)
        state = MatchState::More;
      else if (!optional)
        state = MatchState::No;
    } else if (window.cp[pos] >= low && window.cp[pos] <= high) {
      ++pos;
    } else if (!optional) {
      state = MatchState::No;
    }
    return *this;
  }
  Match& literal(uint16_t cp, bool optional = false) { return range(cp, cp, optional); }
  Match& base(bool optional = false) { return range(0x0E01, 0x0E2E, optional); }
  Match& tone(bool optional = true) { return range(0x0E48, 0x0E4B, optional); }

  void acceptOptional(const Match& branch) {
    if (branch.state == MatchState::Yes)
      pos = branch.pos;
    else if (branch.state == MatchState::More)
      state = MatchState::More;
  }
  Match& toneBase() {
    if (state != MatchState::Yes) return *this;
    Match branch{window, pos};
    branch.tone(false).base();
    acceptOptional(branch);
    return *this;
  }
  Match& suffix() {
    if (state != MatchState::Yes) return *this;
    Match branch{window, pos};
    branch.base().base(true);
    if (branch.state == MatchState::Yes && branch.pos < window.count) {
      const auto cp = window.cp[branch.pos];
      // Upstream [d|ิ] expands to [ูุ|ิ], including the literal '|'.
      if (cp == 0x0E39 || cp == 0x0E38 || cp == '|' || cp == 0x0E34) ++branch.pos;
    }
    branch.literal(0x0E4C);
    acceptOptional(branch);
    return *this;
  }
  Match& followingBaseOrVowel() {
    if (state != MatchState::Yes) return *this;
    if (pos == window.count) {
      if (!window.final) state = MatchState::More;
    } else if (!isBase(window.cp[pos]) && !isLeadingVowel(window.cp[pos])) {
      state = MatchState::No;
    }
    return *this;
  }
  Match& vowelIUU() {
    if (state != MatchState::Yes) return *this;
    if (pos == window.count)
      state = window.final ? MatchState::No : MatchState::More;
    else if (window.cp[pos] == 0x0E34 || window.cp[pos] == 0x0E38 || window.cp[pos] == 0x0E39)
      ++pos;
    else
      state = MatchState::No;
    return *this;
  }
  Match& vowelIIUU() {
    if (state == MatchState::Yes && pos < window.count && window.cp[pos] == 0x0E35) {
      ++pos;
      return *this;
    }
    return vowelIUU();
  }
};

// Exactly the pinned alternatives, in order. The first cั alternative shadows
// the following suffixed alternative; deliberately do not "optimize" it away.
MatchState grammar(const Window& window, uint8_t start, uint8_t& end) {
  for (uint8_t rule = 0; rule <= 30; ++rule) {
    Match m{window, start};
    switch (rule) {
      // Explicit correction before upstream alternatives: เ c ื t อ ะ?
      case 0:
        m.literal(0x0E40).base().literal(0x0E37).tone().literal(0x0E2D).literal(0x0E30, true);
        break;
      case 1:
        m.base().literal(0x0E31).toneBase();
        break;
      case 2:
        m.base().literal(0x0E31).toneBase().suffix();
        break;
      case 3:
        m.literal(0x0E40).base().literal(0x0E47).base().suffix();
        break;
      case 4:
        m.literal(0x0E40).base().base().tone().literal(0x0E32).literal(0x0E30).suffix();
        break;
      case 5:
        m.literal(0x0E40).base().base().literal(0x0E35).tone().literal(0x0E22).literal(0x0E30).suffix();
        break;
      case 6:
        m.literal(0x0E40).base().base().literal(0x0E35).tone().literal(0x0E22).followingBaseOrVowel().suffix();
        break;
      case 7:
        m.literal(0x0E40).base().vowelIIUU().tone().literal(0x0E22).followingBaseOrVowel().suffix();
        break;
      case 8:
        m.literal(0x0E40).base().base().literal(0x0E47).base().suffix();
        break;
      case 9:
        m.literal(0x0E40).base().literal(0x0E34).base().literal(0x0E4C).base().suffix();
        break;
      case 10:
        m.literal(0x0E40).base().literal(0x0E34).tone().base().suffix();
        break;
      case 11:
        m.literal(0x0E40).base().literal(0x0E35).tone().literal(0x0E22).literal(0x0E30, true).suffix();
        break;
      case 12:
        m.literal(0x0E40).base().literal(0x0E37).tone().literal(0x0E2D).literal(0x0E30).suffix();
        break;
      case 13:
        m.literal(0x0E40).base().literal(0x0E37);
        break;
      case 14:
        m.literal(0x0E40).base().tone().literal(0x0E32, true).literal(0x0E30, true).suffix();
        break;
      case 15:
        m.base().range(0x0E36, 0x0E37).tone().base().suffix();
        break;
      case 16:
        m.base().range(0x0E30, 0x0E39).tone().suffix();
        break;
      case 17:
        m.base().vowelIUU().literal(0x0E4C);
        break;
      case 18:
        m.base().literal(0x0E23).literal(0x0E23).base().literal(0x0E4C);
        break;
      case 19:
        m.base().literal(0x0E47);
        break;
      case 20:
        m.base().tone();
        if (m.state == MatchState::Yes && m.pos < window.count &&
            (window.cp[m.pos] == 0x0E30 || window.cp[m.pos] == 0x0E32 || window.cp[m.pos] == 0x0E33))
          ++m.pos;
        m.suffix();
        break;
      case 21:
        m.literal(0x0E41).base().literal(0x0E47).base().suffix();
        break;
      case 22:
        m.literal(0x0E41).base().base().literal(0x0E4C).suffix();
        break;
      case 23:
        m.literal(0x0E41).base().tone().literal(0x0E30).suffix();
        break;
      case 24:
        m.literal(0x0E41).base().base().literal(0x0E47).base().suffix();
        break;
      case 25:
        m.literal(0x0E41).base().base().base().literal(0x0E4C).suffix();
        break;
      case 26:
        m.literal(0x0E42).base().tone().literal(0x0E30).suffix();
        break;
      case 27:
        m.range(0x0E40, 0x0E44).base().tone().suffix();
        break;
      case 28:
        m.literal(0x0E01).literal(0x0E47);
        break;
      case 29:
        m.literal(0x0E2D).literal(0x0E36);
        break;
      case 30:
        m.literal(0x0E2B).literal(0x0E36);
        break;
    }
    if (m.state != MatchState::No) {
      end = m.pos;
      return m.state;
    }
  }
  end = start + 1;
  return MatchState::Yes;
}

bool emit(const Window& window, size_t& offset, Cluster& result, uint8_t count, bool valid) {
  result = {offset, offset + window.ends[count - 1], count, valid};
  offset = result.end;
  return true;
}
}  // namespace

bool containsThai(std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    const auto scalar = detail::decode(text, offset, true);
    if (scalar.valid && isThai(scalar.value)) return true;
    offset += scalar.bytes;
  }
  return false;
}

bool nextCluster(std::string_view text, size_t& offset, Cluster& result, bool endOfRun) {
  const auto first = detail::decode(text, offset, endOfRun);
  if (!first.bytes) return false;
  if (!first.valid || !isThai(first.value)) {
    result = {offset, offset + first.bytes, 1, first.valid};
    offset = result.end;
    return true;
  }
  Window window;
  window.load(text, offset, endOfRun);
  uint8_t start = 0;
  // Orphan marks are source errors, not new paragraph boundaries. Keep them
  // with the following Thai cluster when possible, without unbounded retention.
  while (start < window.count && isDependentSign(window.cp[start])) ++start;
  uint8_t end = start;
  if (start < window.count && window.cp[start] != INVALID) {
    if (grammar(window, start, end) == MatchState::More) {
      if (window.count == MAX_CLUSTER_CODEPOINTS) return emit(window, offset, result, window.count, false);
      return false;
    }
  } else if (!start) {
    return emit(window, offset, result, 1, false);
  }
  while (end < window.count && isDependentSign(window.cp[end])) ++end;
  if (end == window.count && !window.final) {
    if (end < MAX_CLUSTER_CODEPOINTS) return false;
    // The grammar stores at most 32 scalars. One bounded boundary probe tells
    // whether a 32-scalar span is complete or belongs to a pathological run;
    // it is not retained or offered to the finite grammar.
    const auto next = detail::decode(text, offset + window.ends[end - 1], endOfRun);
    if (!next.bytes) return false;
    if (isDependentSign(next.value) || start == window.count) return emit(window, offset, result, end, false);
  }
  return emit(window, offset, result, end, true);
}

size_t lastSafeBoundary(std::string_view text, size_t limit, bool endOfRun) {
  size_t offset = 0;
  size_t safe = 0;
  Cluster cluster{};
  while (offset < text.size() && offset < limit && nextCluster(text, offset, cluster, endOfRun)) {
    if (!cluster.valid || cluster.end > limit) break;
    safe = cluster.end;
  }
  return safe;
}

namespace {
// Spacing units are deliberately finer than TCCs. Only the conventional
// base/mark stacks are admitted; malformed stacks exclude their whole TCC.
bool spacingUnit(std::string_view text, size_t& offset) {
  const auto first = detail::decode(text, offset, true);
  if (!first.valid || !first.bytes) return false;
  offset += first.bytes;
  if (!isBase(first.value)) {
    return isLeadingVowel(first.value) || (isSpacingVowel(first.value) && first.value != 0xE33);
  }
  auto next = detail::decode(text, offset, true);
  // Decomposed Sara Am, including its reordered tone, stays with the base.
  if (next.value == 0xE4D) {
    size_t end = offset + next.bytes;
    auto tone = detail::decode(text, end, true);
    if (isTone(tone.value)) end += tone.bytes;
    const auto aa = detail::decode(text, end, true);
    if (aa.value == 0xE32) {
      offset = end + aa.bytes;
      return true;
    }
  }
  size_t end = offset;
  if (isTone(next.value)) end += next.bytes;
  const auto am = detail::decode(text, end, true);
  if (am.value == 0xE33) {
    offset = end + am.bytes;
    return true;
  }
  if (isAboveVowel(next.value) || isBelowVowel(next.value) || next.value == 0xE47 || next.value == 0xE4D) {
    offset += next.bytes;
    next = detail::decode(text, offset, true);
  }
  if (isTone(next.value) || next.value == 0xE4C || next.value == 0xE4E) offset += next.bytes;
  return true;
}
}  // namespace

bool isJustifiableLetterCluster(std::string_view text, const Cluster& cluster) {
  if (!cluster.valid || cluster.begin >= cluster.end || cluster.end > text.size()) return false;
  const auto first = detail::decode(text, cluster.begin, true);
  if (!first.valid || (!isBase(first.value) && !isLeadingVowel(first.value))) return false;
  bool hasBase = false;
  // Restrict lookahead to this TCC; spacing never changes its admission or end.
  const auto letters = text.substr(0, cluster.end);
  for (size_t offset = cluster.begin; offset < cluster.end;) {
    const auto scalar = detail::decode(letters, offset, true);
    hasBase |= isBase(scalar.value);
    if (!spacingUnit(letters, offset)) return false;
  }
  return hasBase;
}

bool JustificationBoundaryCursor::next(size_t& byteOffset) {
  for (;;) {
    if (unitOffset_ < clusterEnd_) {
      byteOffset = unitOffset_;
      spacingUnit(text_.substr(0, clusterEnd_), unitOffset_);
      return true;
    }
    Cluster cluster{};
    if (!nextCluster(text_, offset_, cluster, true)) return false;
    const bool eligible = isJustifiableLetterCluster(text_, cluster);
    const bool boundary = previousEligible_ && eligible;
    previousEligible_ = eligible;
    if (!eligible) continue;
    clusterEnd_ = cluster.end;
    unitOffset_ = cluster.begin;
    spacingUnit(text_.substr(0, clusterEnd_), unitOffset_);
    if (boundary) {
      byteOffset = cluster.begin;
      return true;
    }
  }
}
}  // namespace thai
