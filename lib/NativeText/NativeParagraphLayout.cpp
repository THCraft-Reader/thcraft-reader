#include "NativeParagraphLayout.h"

#include <hb.h>

#include "../Epub/Epub/CjkBreakPolicy.h"
#include "../Epub/Epub/hyphenation/Hyphenator.h"
#include "NativePlatform.h"
#include "NativeTextEngine.h"
#include "NativeUtf8.h"
extern "C" {
#include <minibidi.h>
}

#include <algorithm>
#include <climits>
#include <new>

namespace {
constexpr size_t MAX_SCALARS = 4096, MAX_BYTES = 16384, RETAIN_SCALARS = 512;
constexpr uint16_t SAFE = 1, BREAK = 2, EMERGENCY = 4, HYPHEN = 8, WORD = 16, STRETCH = 32, HARD = 64;
constexpr uint8_t BOLD = 1, UNDERLINE = 4, SUP = 16, SUB = 32;
constexpr uint64_t INF = UINT64_MAX / 4;
struct Lock {
  NativeTextEngine& engine;
  explicit Lock(NativeTextEngine& value) : engine(value) { engine.lock(); }
  ~Lock() { engine.unlock(); }
};
template <class T>
bool append(NativeBuffer<T>& buffer, const T& value) {
  const size_t index = buffer.size();
  if (index == buffer.capacity() && !buffer.reserve(std::max<size_t>(8, index * 2))) return false;
  if (!buffer.resize(index + 1)) return false;
  buffer[index] = value;
  return true;
}
int32_t floor26(int32_t value) { return value >= 0 ? value / 64 : -static_cast<int32_t>((-int64_t(value) + 63) / 64); }
int32_t ceil26(int32_t value) {
  return value >= 0 ? static_cast<int32_t>((int64_t(value) + 63) / 64) : -(-int64_t(value) / 64);
}
bool noBreakSpace(uint32_t cp) { return cp == 0x00a0 || cp == 0x202f || cp == 0x2060 || cp == 0xfeff; }
bool space(uint32_t cp) {
  return cp == ' ' || (cp >= 9 && cp <= 13) || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x205f || cp == 0x3000;
}
bool selectable(uint32_t cp) {
  const auto category = hb_unicode_general_category(hb_unicode_funcs_get_default(), cp);
  return category == HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER ||
         category == HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER ||
         category == HB_UNICODE_GENERAL_CATEGORY_LETTER_NUMBER || category == HB_UNICODE_GENERAL_CATEGORY_OTHER_NUMBER;
}
bool ideograph(uint32_t cp) {
  return (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0xf900 && cp <= 0xfaff) ||
         (cp >= 0x20000 && cp <= 0x3ffff);
}
struct Point {
  uint32_t byte = 0, cp = 0, source = 0;
  uint16_t flags = 0;
  uint8_t style = 0, level = 0;
};
struct WordRange {
  uint16_t first, last;
};
struct RubyMetrics {
  uint32_t first, last;
  int32_t width, top, bottom;
};
struct WidthEntry {
  uint16_t first = UINT16_MAX, last = 0;
  int32_t width = 0;
};
struct Node {
  uint64_t cost = INF;
  uint16_t point = 0, next = 0;
};
struct Box {
  int32_t left = INT32_MAX, right = INT32_MIN, top = 0, bottom = 0;
  bool found = false;
};
Box rangeBox(const NativeGlyphRun& run, uint32_t first, uint32_t last, std::span<const NativeGap> gaps = {}) {
  Box box;
  for (const auto& cluster : run.clusters.span()) {
    if (cluster.startByte >= last || cluster.endByte <= first) continue;
    box.left = std::min(box.left, cluster.x26);
    int32_t advance = cluster.advance26;
    const auto gap = std::lower_bound(gaps.begin(), gaps.end(), cluster.endByte,
                                      [](const NativeGap& value, uint32_t byte) { return value.byteOffset < byte; });
    if (gap != gaps.end() && gap->byteOffset == cluster.endByte) advance -= gap->extraAdvance26;
    box.right = std::max(box.right, cluster.x26 + advance);
    box.top = box.found ? std::min(box.top, cluster.top26) : cluster.top26;
    box.bottom = box.found ? std::max(box.bottom, cluster.bottom26) : cluster.bottom26;
    box.found = true;
  }
  return box;
}
}  // namespace

struct NativeParagraphLayout::State {
  NativeBuffer<Point> points;
  NativeBuffer<bidi_char> bidi;
  NativeBuffer<uint8_t> levels, bidiScratch;
  NativeBuffer<uint32_t> thai;
  NativeBuffer<CodepointInfo> hyphenPoints;
  NativeBuffer<Hyphenator::BreakInfo> hyphens;
  NativeBuffer<WordRange> words;
  NativeBuffer<RubyMetrics> ruby;
  NativeBuffer<NativeStyleSpan> spans;
  NativeBuffer<NativeGap> gaps;
  NativeBuffer<Node> nodes;
  // Fixed-size associative cache; the remaining scratch grows only with this window.
  WidthEntry widths[2048];
  NativeGlyphRun run;
  const NativeParagraphView* paragraph = nullptr;
  const NativeLayoutOptions* options = nullptr;
  std::string_view text;
  int8_t level = 0;
  bool final = true;
  bool callbackAttempted = false;
  int32_t leftPad = 0;

  size_t atByte(uint32_t byte) const {
    const auto* found = std::lower_bound(points.data(), points.data() + points.size(), byte,
                                         [](const Point& point, uint32_t value) { return point.byte < value; });
    return static_cast<size_t>(found - points.data());
  }
  bool boundary(uint32_t byte) const {
    const size_t index = atByte(byte);
    return index < points.size() && points[index].byte == byte && (points[index].flags & SAFE);
  }
  bool inRuby(uint32_t byte) const {
    for (const auto& group : ruby.span())
      if (byte > group.first && byte < group.last) return true;
    return false;
  }
  TextStatus makeSpans(size_t first, size_t last) {
    spans.clear();
    const uint32_t origin = points[first].byte;
    for (size_t i = first; i < last; ++i) {
      const NativeStyleSpan next{points[i].byte - origin, points[i + 1].byte - origin, points[i].style,
                                 points[i].level};
      if (!spans.empty() && spans[spans.size() - 1].style == next.style &&
          spans[spans.size() - 1].bidiLevel == next.bidiLevel)
        spans[spans.size() - 1].endByte = next.endByte;
      else if (!append(spans, next))
        return TextStatus::OutOfMemory;
    }
    return TextStatus::Ok;
  }
  NativeLineInput input(size_t first, size_t last, bool useGaps) const {
    NativeLineInput value;
    value.text = text.substr(points[first].byte, points[last].byte - points[first].byte);
    value.fontId = options->fontId;
    value.spans = spans.span();
    value.paragraphLevel = level;
    value.resolvedLevels = true;
    value.readerFeatures = options->readerFeatures;
    value.syntheticSuffixCp = (points[last].flags & HYPHEN) ? '-' : 0;
    if (useGaps) value.gaps = gaps.span();
    return value;
  }
  TextStatus addGap(uint32_t byte, int32_t extra) {
    if (extra <= 0) return TextStatus::Ok;
    for (auto& gap : gaps.span()) {
      if (gap.byteOffset != byte) continue;
      if (gap.extraAdvance26 > INT32_MAX - extra) return TextStatus::CapacityExceeded;
      gap.extraAdvance26 += extra;
      return TextStatus::Ok;
    }
    return append(gaps, NativeGap{byte, extra}) ? TextStatus::Ok : TextStatus::OutOfMemory;
  }
  void sortGaps() {
    if (gaps.size() > 1)
      std::sort(gaps.data(), gaps.data() + gaps.size(),
                [](const NativeGap& a, const NativeGap& b) { return a.byteOffset < b.byteOffset; });
  }
  TextStatus shapeCandidate(NativeTextEngine& engine, size_t first, size_t last, int32_t& width) {
    auto status = makeSpans(first, last);
    if (status != TextStatus::Ok) return status;
    gaps.clear();
    leftPad = 0;
    status = engine.shapeLine(input(first, last, false), run);
    if (status != TextStatus::Ok) return status;
    const uint32_t origin = points[first].byte, end = points[last].byte;
    // Reserve ruby overhang using final visual clusters, never token-width sums.
    for (size_t r = 0; r < ruby.size(); ++r) {
      const auto& group = ruby[r];
      if (group.first < origin || group.last > end) continue;
      const Box box = rangeBox(run, group.first - origin, group.last - origin);
      if (!box.found) return TextStatus::InvalidText;
      const int32_t excess = std::max(0, group.width - (box.right - box.left));
      if (!excess) continue;
      const NativeCluster* before = nullptr;
      const NativeCluster* after = nullptr;
      const NativeCluster* right = nullptr;
      for (const auto& cluster : run.clusters.span()) {
        if (cluster.startByte == cluster.endByte) continue;
        if (cluster.x26 + cluster.advance26 <= box.left) before = &cluster;
        if (!after && cluster.x26 >= box.right) after = &cluster;
        if (cluster.startByte < group.last - origin && cluster.endByte > group.first - origin &&
            (!right || cluster.x26 > right->x26))
          right = &cluster;
      }
      auto allowance = [&](const NativeCluster* cluster) -> int32_t {
        if (!cluster) return 0;
        const uint32_t byte = origin + cluster->startByte;
        for (const auto& other : ruby.span())
          if (byte >= other.first && byte < other.last) return 0;
        const size_t index = atByte(byte);
        return index < points.size() && !ideograph(points[index].cp) ? cluster->advance26 / 2 : 0;
      };
      const int32_t left = std::max(0, excess / 2 - allowance(before));
      const int32_t rightExtra = std::max(0, excess - excess / 2 - allowance(after));
      if (before)
        status = addGap(before->endByte, left);
      else
        leftPad = std::max(leftPad, left);
      if (status == TextStatus::Ok && right) status = addGap(right->endByte, rightExtra);
      if (status != TextStatus::Ok) return status;
    }
    if (!gaps.empty()) {
      sortGaps();
      status = engine.shapeLine(input(first, last, true), run);
      if (status != TextStatus::Ok) return status;
    }
    if (run.advance26 > INT32_MAX - leftPad) return TextStatus::CapacityExceeded;
    width = run.advance26 + leftPad;
    nativeTextYield();
    return TextStatus::Ok;
  }
  TextStatus measure(NativeTextEngine& engine, size_t first, size_t last, int32_t& width) {
    const size_t slot = ((first * 4099u) ^ (last * 131u)) & 2047u;
    auto& cached = widths[slot];
    if (cached.first == first && cached.last == last) {
      width = cached.width;
      return TextStatus::Ok;
    }
    const auto status = shapeCandidate(engine, first, last, width);
    if (status == TextStatus::Ok) cached = {static_cast<uint16_t>(first), static_cast<uint16_t>(last), width};
    return status;
  }
  int32_t available(size_t point) const {
    const int32_t indent = point == 0 && options->firstLine ? options->firstLineIndent * 64 : 0;
    return std::max<int32_t>(0, options->width * 64 - indent);
  }

  TextStatus prepare(NativeTextEngine& engine) {
    for (auto& entry : widths) entry.first = UINT16_MAX;
    points.clear();
    words.clear();
    ruby.clear();
    if (!points.reserve(std::min(text.size() + 1, MAX_SCALARS + 1))) return TextStatus::OutOfMemory;
    size_t offset = 0, sourceAnchor = 0, style = 0;
    uint64_t source = paragraph->sourceStart;
    uint32_t previousByte = 0, previousSource = 0;
    for (size_t i = 0; i < paragraph->sourceAnchors.size(); ++i) {
      const auto& anchor = paragraph->sourceAnchors[i];
      if ((i && (anchor.byteOffset <= previousByte || anchor.sourceOffset < previousSource)) ||
          !native_text::utf8Boundary(paragraph->text, anchor.byteOffset))
        return TextStatus::InvalidText;
      previousByte = anchor.byteOffset;
      previousSource = anchor.sourceOffset;
    }
    previousByte = 0;
    for (const auto& span : paragraph->spans) {
      if (span.startByte < previousByte || span.endByte <= span.startByte || span.bidiLevel > 125 ||
          !native_text::utf8Boundary(paragraph->text, span.startByte) ||
          !native_text::utf8Boundary(paragraph->text, span.endByte))
        return TextStatus::InvalidText;
      previousByte = span.endByte;
    }
    while (true) {
      if (sourceAnchor < paragraph->sourceAnchors.size() && paragraph->sourceAnchors[sourceAnchor].byteOffset == offset)
        source = paragraph->sourceAnchors[sourceAnchor++].sourceOffset;
      if (source > UINT32_MAX) return TextStatus::CapacityExceeded;
      if (!points.empty() && source < points[points.size() - 1].source) return TextStatus::InvalidText;
      Point point;
      point.byte = static_cast<uint32_t>(offset);
      point.source = static_cast<uint32_t>(source);
      while (style < paragraph->spans.size() && paragraph->spans[style].endByte <= offset) ++style;
      if (style < paragraph->spans.size() && paragraph->spans[style].startByte <= offset) {
        point.style = paragraph->spans[style].style;
        point.level = paragraph->spans[style].bidiLevel;
      } else
        point.level = paragraph->paragraphLevel < 0 ? 0 : paragraph->paragraphLevel;
      const size_t start = offset;
      if (offset < text.size() && !native_text::nextUtf8(text, offset, point.cp)) return TextStatus::InvalidText;
      if (!append(points, point)) return TextStatus::OutOfMemory;
      if (start == text.size()) break;
      source += paragraph->sourceUnit == NativeSourceUnit::Byte ? offset - start : 1;
    }
    const size_t count = points.size() - 1;
    level = paragraph->paragraphLevel < 0 ? 0 : paragraph->paragraphLevel;
    if (!paragraph->resolvedLevels) {
      const size_t scratchBytes = bidi_scratch_size(count);
      if (!bidi.resize(count) || !levels.resize(count) || !bidiScratch.resize(scratchBytes))
        return TextStatus::OutOfMemory;
      for (size_t i = 0; i < count; ++i) bidi[i] = {points[i].cp, points[i].cp, static_cast<uint16_t>(i), 0};
      const int resolved =
          resolve_bidi_levels(paragraph->paragraphLevel < 0, level, bidi.data(), static_cast<int>(count), levels.data(),
                              bidiScratch.data(), scratchBytes);
      if (resolved < 0) return TextStatus::CapacityExceeded;
      level = static_cast<int8_t>(resolved);
      for (size_t i = 0; i < count; ++i) points[i].level = levels[i];
    }
    auto status = makeSpans(0, count);
    if (status != TextStatus::Ok) return status;
    status = engine.shapeLine(input(0, count, false), run);
    if (status != TextStatus::Ok) return status;
    for (const auto& cluster : run.clusters.span()) {
      const size_t first = atByte(cluster.startByte), last = atByte(cluster.endByte);
      if (first >= count || last > count) return TextStatus::InvalidText;
      if (!cluster.unsafeToBreak) points[first].flags |= SAFE;
      // A base owns markup that happened inside its shaping cluster. Keep the
      // pre-L1 paragraph levels, rather than persisting line-reset whitespace.
      for (size_t i = first; i < last; ++i) {
        points[i].style = cluster.style;
        points[i].level = points[first].level;
      }
    }
    points[0].flags |= SAFE | BREAK | WORD;
    points[count].flags |= SAFE | BREAK | WORD;
    if (!thai.resize(count + 1)) return TextStatus::OutOfMemory;
    size_t found = 0;
    status = engine.findThaiBreaks(text, thai.span(), found);
    if (status != TextStatus::Ok) return status;
    for (size_t i = 0; i < found; ++i) {
      const size_t at = atByte(thai[i]);
      if (at < points.size() && points[at].byte == thai[i] && (points[at].flags & SAFE))
        points[at].flags |= BREAK | WORD;
    }
    status = engine.findThaiBreaks(text, thai.span(), found, true);
    if (status != TextStatus::Ok) return status;
    for (size_t i = 0; i < found; ++i) {
      const size_t at = atByte(thai[i]);
      if (at < points.size() && points[at].byte == thai[i] && (points[at].flags & SAFE)) points[at].flags |= EMERGENCY;
    }
    for (size_t i = 1; i <= count; ++i) {
      const uint32_t left = points[i - 1].cp, right = i < count ? points[i].cp : 0;
      if (space(left) || noBreakSpace(left) || left == 0x200b || space(right) || noBreakSpace(right) || right == 0x200b)
        points[i].flags |= WORD;
      if (!(points[i].flags & SAFE)) continue;
      if (space(left) || left == 0x200b) points[i].flags |= BREAK;
      if (left == ' ' || left == '\t' || (space(left) && left != '\n' && left != '\r')) points[i].flags |= STRETCH;
      if (left == '\n' || (left == '\r' && right != '\n') || left == 0x2028 || left == 0x2029)
        points[i].flags |= BREAK | HARD;
      if (i < count && CjkBreakPolicy::hasCjkBreakOpportunityBetween(left, right))
        points[i].flags |= BREAK | WORD | STRETCH;
      if (isExplicitHyphen(left) && !isSoftHyphen(left) && left != 0x2011) points[i].flags |= BREAK;
      if (left == 0x00ad && (options->hyphenation || options->emergencyHyphenation)) points[i].flags |= BREAK | HYPHEN;
      if (noBreakSpace(left) || noBreakSpace(right) || left == 0x2011 || right == 0x2011)
        points[i].flags &= ~(BREAK | EMERGENCY | HYPHEN | STRETCH);
    }
    points[count].flags |= BREAK;
    points[count].flags &= ~HYPHEN;
    previousByte = 0;
    for (const auto& value : paragraph->ruby) {
      if (value.baseStartByte < previousByte || value.baseEndByte <= value.baseStartByte ||
          !native_text::utf8Boundary(paragraph->text, value.baseStartByte) ||
          !native_text::utf8Boundary(paragraph->text, value.baseEndByte))
        return TextStatus::InvalidText;
      previousByte = value.baseEndByte;
      if (value.baseStartByte >= text.size()) continue;
      if (value.baseEndByte > text.size()) {
        // A group crossing the window remains entirely uncommitted.
        size_t start = atByte(value.baseStartByte);
        while (start && !(points[start].flags & SAFE)) --start;
        for (size_t i = start + 1; i < points.size(); ++i) points[i].flags &= ~(BREAK | EMERGENCY | HYPHEN);
        continue;
      }
      size_t start = atByte(value.baseStartByte), end = atByte(value.baseEndByte);
      while (start && !(points[start].flags & SAFE)) --start;
      while (end < count && !(points[end].flags & SAFE)) ++end;
      if (!ruby.empty() && points[start].byte < ruby[ruby.size() - 1].last) return TextStatus::InvalidText;
      NativeStyleSpan annotation{0, static_cast<uint32_t>(value.text.size()),
                                 static_cast<uint8_t>((value.style & ~(SUP | SUB)) | SUP), 0};
      NativeLineInput rubyInput;
      rubyInput.text = value.text;
      rubyInput.fontId = options->fontId;
      if (!value.text.empty()) rubyInput.spans = {&annotation, 1};
      rubyInput.readerFeatures = options->readerFeatures;
      status = engine.shapeLine(rubyInput, run);
      if (status != TextStatus::Ok) return status;
      if (!append(ruby,
                  RubyMetrics{points[start].byte, points[end].byte, run.advance26, run.ink.top26, run.ink.bottom26}))
        return TextStatus::OutOfMemory;
      for (size_t i = start + 1; i < end; ++i) points[i].flags &= ~(BREAK | EMERGENCY | HYPHEN | STRETCH);
    }
    for (const auto& link : paragraph->links)
      if (link.endByte < link.startByte || !native_text::utf8Boundary(paragraph->text, link.startByte) ||
          !native_text::utf8Boundary(paragraph->text, link.endByte))
        return TextStatus::InvalidText;

    for (size_t first = 0; first < count;) {
      while (first < count && (space(points[first].cp) || noBreakSpace(points[first].cp) || points[first].cp == 0x200b))
        ++first;
      if (first == count) break;
      size_t last = first + 1;
      while (last < count && !((points[last].flags & WORD) && (points[last].flags & SAFE))) ++last;
      bool visibleWord = false;
      for (size_t i = first; i < last; ++i) visibleWord |= selectable(points[i].cp);
      if (visibleWord && !append(words, WordRange{static_cast<uint16_t>(first), static_cast<uint16_t>(last)}))
        return TextStatus::OutOfMemory;
      first = last;
    }
    if (options->focus) {
      for (const auto& word : words.span()) {
        if (utf8IsCjkBreakable(points[word.first].cp)) continue;
        size_t end = word.first + std::clamp<size_t>((word.last - word.first) * 45 / 100, 1, 9);
        end = std::min<size_t>(end, word.last);
        while (end < word.last && !(points[end].flags & SAFE)) ++end;
        for (size_t i = word.first; i < end; ++i) points[i].style |= BOLD;
      }
    }
    // Language patterns supplement ordinary opportunities. Emergency fallback is
    // enabled only for a word which cannot fit a line in its final shaped form.
    for (const auto& word : words.span()) {
      if (!options->hyphenation && !options->emergencyHyphenation) break;
      bool thaiWord = false, nbsp = false;
      for (size_t i = word.first; i < word.last; ++i) {
        thaiWord |= native_text::isThai(points[i].cp);
        nbsp |= noBreakSpace(points[i].cp);
      }
      if (thaiWord || nbsp || inRuby(points[word.first].byte) || inRuby(points[word.last].byte)) continue;
      int32_t width;
      status = measure(engine, word.first, word.last, width);
      if (status != TextStatus::Ok) return status;
      const bool oversized = width > available(word.first);
      if (!options->hyphenation && !(oversized && options->emergencyHyphenation)) continue;
      const size_t length = word.last - word.first;
      if (!hyphenPoints.resize(length) || !hyphens.resize(length)) return TextStatus::OutOfMemory;
      for (size_t i = 0; i < length; ++i) hyphenPoints[i] = {points[word.first + i].cp, points[word.first + i].byte};
      size_t written = 0;
      if (!Hyphenator::breakOffsets(hyphenPoints.data(), length, oversized, hyphens.data(), length, written))
        return TextStatus::CapacityExceeded;
      for (size_t i = 0; i < written; ++i) {
        const size_t at = atByte(static_cast<uint32_t>(hyphens[i].byteOffset));
        if (at >= points.size() || !(points[at].flags & SAFE) || inRuby(points[at].byte)) continue;
        points[at].flags |= BREAK;
        if (hyphens[i].requiresInsertedHyphen && !(points[at].flags & WORD)) points[at].flags |= HYPHEN;
      }
    }
    // Measuring above preceded adding synthetic suffixes; invalidate those keys.
    for (auto& entry : widths) entry.first = UINT16_MAX;
    size_t first = 0;
    for (size_t end = 1; end <= count; ++end) {
      if (!(points[end].flags & BREAK)) continue;
      int32_t width;
      status = measure(engine, first, end, width);
      if (status != TextStatus::Ok) return status;
      if (width > available(first)) {
        for (size_t at = first + 1; at < end; ++at) {
          if (!(points[at].flags & SAFE) || inRuby(points[at].byte) || noBreakSpace(points[at - 1].cp) ||
              noBreakSpace(points[at].cp) || points[at - 1].cp == 0x2011 || points[at].cp == 0x2011)
            continue;
          if (utf8IsCjkBreakable(points[at - 1].cp) || utf8IsCjkBreakable(points[at].cp)) continue;
          const bool thaiCell = native_text::isThai(points[at - 1].cp) || native_text::isThai(points[at].cp);
          if ((thaiCell && (points[at].flags & EMERGENCY)) || !thaiCell) points[at].flags |= BREAK;
        }
      }
      first = end;
    }
    return TextStatus::Ok;
  }

  TextStatus fit(NativeTextEngine& engine) {
    nodes.clear();
    for (size_t i = 0; i < points.size(); ++i)
      if (points[i].flags & BREAK)
        if (!append(nodes, Node{INF, static_cast<uint16_t>(i), 0})) return TextStatus::OutOfMemory;
    if (nodes.empty() || nodes[0].point != 0) return TextStatus::InvalidText;
    const size_t count = nodes.size();
    nodes[count - 1].cost = 0;
    for (size_t i = count - 1; i-- > 0;) {
      const size_t first = nodes[i].point;
      const int32_t limit = available(first);
      bool fitted = false;
      for (size_t j = i + 1; j < count; ++j) {
        int32_t width;
        auto status = measure(engine, first, nodes[j].point, width);
        if (status != TextStatus::Ok) return status;
        if (width > limit) {
          if (!fitted) {
            nodes[i].next = static_cast<uint16_t>(j);
            nodes[i].cost = nodes[j].cost;
          }
          break;
        }
        fitted = true;
        const uint64_t slack = static_cast<uint64_t>(limit - width);
        const uint64_t cost = j == count - 1 ? 0 : std::min(INF, slack * slack + nodes[j].cost);
        if (options->hyphenation || cost <= nodes[i].cost) {
          nodes[i].cost = cost;
          nodes[i].next = static_cast<uint16_t>(j);
        }
        if (points[nodes[j].point].flags & HARD) break;
      }
      nativeTextYield();
    }
    return TextStatus::Ok;
  }

  TextStatus emitLine(NativeTextEngine& engine, size_t first, size_t last, bool lastLine, NativeLineCallback emit,
                      void* context) {
    callbackAttempted = false;
    int32_t width;
    auto status = shapeCandidate(engine, first, last, width);
    if (status != TextStatus::Ok) return status;
    const uint32_t origin = points[first].byte, end = points[last].byte;
    const int32_t indent = first == 0 && options->firstLine ? options->firstLineIndent * 64 : 0;
    int32_t remaining = std::max(0, available(first) - width);
    if (options->alignment == NativeAlignment::Justify && !lastLine && remaining) {
      size_t count = 0;
      for (const auto& cluster : run.clusters.span()) {
        const size_t at = atByte(origin + cluster.endByte);
        if (at < last && at > first && (points[at].flags & STRETCH)) ++count;
      }
      const int32_t each = count ? remaining / static_cast<int32_t>(count) : 0;
      int32_t rest = count ? remaining % static_cast<int32_t>(count) : 0;
      for (const auto& cluster : run.clusters.span()) {
        const size_t at = atByte(origin + cluster.endByte);
        if (at >= last || at <= first || !(points[at].flags & STRETCH)) continue;
        status = addGap(cluster.endByte, each + (rest-- > 0 ? 1 : 0));
        if (status != TextStatus::Ok) return status;
      }
      if (count) {
        sortGaps();
        status = engine.shapeLine(input(first, last, true), run);
        if (status != TextStatus::Ok) return status;
        remaining = 0;
      }
    }
    // Persisted/drawn payloads use the reader pnum policy. With readerFeatures
    // disabled, UI wrapping consumes only logicalText(), then draws via its
    // ordinary UI wrapper; NativeLineData::input() intentionally remains reader-only.
    NativeLayoutEmission output;
    auto& line = output.line;
    if (!line.text.assign({text.data() + origin, end - origin}) || !line.spans.assign(spans.span()) ||
        !line.gaps.assign(gaps.span()))
      return TextStatus::OutOfMemory;
    line.paragraphLevel = level;
    line.syntheticSuffixCp = (points[last].flags & HYPHEN) ? '-' : 0;
    // The final-context backtracking above leaves an oversized candidate only
    // when its first legal cluster/group boundary is itself too wide.
    if (width > available(first) && options->width > INT16_MAX) return TextStatus::CapacityExceeded;
    line.overflowClipWidth = width > available(first) ? options->width : 0;
    int32_t align = 0;
    if (options->alignment == NativeAlignment::Center)
      align = remaining / 2;
    else if (options->alignment == NativeAlignment::Right ||
             ((options->alignment == NativeAlignment::Start || options->alignment == NativeAlignment::Justify) &&
              (level & 1)))
      align = remaining;
    line.alignmentX26 = leftPad + align + ((level & 1) ? 0 : indent);
    const auto clipHitBox = [&](int32_t& x26, int32_t& width26) {
      if (!line.overflowClipWidth) return;
      const int32_t right = line.overflowClipWidth * 64;
      const int32_t clippedLeft = std::clamp(x26, 0, right);
      const int32_t clippedRight = std::clamp(x26 + width26, 0, right);
      x26 = clippedLeft;
      width26 = clippedRight - clippedLeft;
    };
    int32_t top = std::min(-run.ascender26, run.ink.top26);
    int32_t bottom = std::max(run.descender26, run.ink.bottom26);
    const int32_t baseTop = top;
    for (const auto& cluster : run.clusters.span())
      if (cluster.style & UNDERLINE) bottom = std::max(bottom, cluster.bottom26 + 128);
    for (size_t r = 0; r < ruby.size(); ++r) {
      const auto& group = ruby[r];
      if (group.first < origin || group.last > end) continue;
      const Box box = rangeBox(run, group.first - origin, group.last - origin, gaps.span());
      if (!box.found) return TextStatus::InvalidText;
      const auto& annotation = paragraph->ruby[r];
      const int32_t rubyBaseline = box.top - 64 - group.bottom;
      top = std::min(top, rubyBaseline + group.top);
      if (line.rubyText.size() > UINT32_MAX - annotation.text.size()) return TextStatus::CapacityExceeded;
      NativeRuby value;
      value.baseStartByte = group.first - origin;
      value.baseEndByte = group.last - origin;
      value.textOffset = static_cast<uint32_t>(line.rubyText.size());
      value.textBytes = static_cast<uint32_t>(annotation.text.size());
      value.x26 = line.alignmentX26 + box.left + (box.right - box.left - group.width) / 2;
      value.y26 = rubyBaseline;
      value.style = annotation.style;
      if (!append(line.ruby, value) || !line.rubyText.resize(line.rubyText.size() + annotation.text.size()))
        return TextStatus::OutOfMemory;
      if (!annotation.text.empty())
        std::memcpy(line.rubyText.data() + value.textOffset, annotation.text.data(), annotation.text.size());
    }
    const int32_t baseline = -floor26(top), height = baseline + ceil26(bottom);
    if (baseline < 0 || baseline > INT16_MAX || height < baseline || height > INT16_MAX)
      return TextStatus::CapacityExceeded;
    line.baseline = static_cast<int16_t>(baseline);
    line.lineHeight = static_cast<int16_t>(height);
    line.rubyLift = static_cast<int16_t>(std::max(0, baseline + floor26(baseTop)));
    for (auto& value : line.ruby.span()) value.y26 += baseline * 64;
    size_t selectionBytes = 0, selectionCount = 0;
    for (const auto& word : words.span()) {
      const size_t begin = std::max<size_t>(word.first, first), stop = std::min<size_t>(word.last, last);
      if (begin < stop) {
        selectionBytes += points[stop].byte - points[begin].byte + 1;
        ++selectionCount;
      }
    }
    if (!line.selectionText.reserve(selectionBytes) || !line.words.reserve(selectionCount))
      return TextStatus::OutOfMemory;
    for (const auto& word : words.span()) {
      const size_t begin = std::max<size_t>(word.first, first), stop = std::min<size_t>(word.last, last);
      if (begin >= stop) continue;
      const uint32_t startByte = points[begin].byte - origin, endByte = points[stop].byte - origin;
      const Box box = rangeBox(run, startByte, endByte, gaps.span());
      if (!box.found) continue;
      NativeWord value;
      value.startByte = startByte;
      value.endByte = endByte;
      value.selectionTextOffset = static_cast<uint32_t>(line.selectionText.size());
      value.x26 = line.alignmentX26 + box.left;
      value.width26 = box.right - box.left;
      clipHitBox(value.x26, value.width26);
      value.x26 -= line.alignmentX26;
      value.top = static_cast<int16_t>(std::max(0, baseline + floor26(box.top)));
      const int32_t wordBottom = std::min(height, baseline + ceil26(box.bottom));
      value.height = static_cast<int16_t>(std::max(0, wordBottom - value.top));
      value.style = points[begin].style;
      if (!append(line.words, value) || !line.selectionText.resize(line.selectionText.size() + endByte - startByte + 1))
        return TextStatus::OutOfMemory;
      std::memcpy(line.selectionText.data() + value.selectionTextOffset, text.data() + origin + startByte,
                  endByte - startByte);
      line.selectionText[value.selectionTextOffset + endByte - startByte] = '\0';
    }
    for (const auto& link : paragraph->links) {
      const uint32_t start = std::max(link.startByte, origin), stop = std::min(link.endByte, end);
      if (start >= stop) continue;
      bool active = false;
      NativeLinkBox box;
      int32_t linkTop = 0, linkBottom = 0;
      auto flush = [&]() -> bool {
        if (!active) return true;
        box.top = static_cast<int16_t>(std::max(0, baseline + floor26(linkTop)));
        box.height = static_cast<int16_t>(std::max(0, std::min(height, baseline + ceil26(linkBottom)) - box.top));
        active = false;
        clipHitBox(box.x26, box.width26);
        if (line.overflowClipWidth && !box.width26) return true;
        return append(output.links, box);
      };
      for (const auto& cluster : run.clusters.span()) {
        if (cluster.startByte + origin >= stop || cluster.endByte + origin <= start) {
          if (!flush()) return TextStatus::OutOfMemory;
          continue;
        }
        if (active && box.x26 + box.width26 != line.alignmentX26 + cluster.x26)
          if (!flush()) return TextStatus::OutOfMemory;
        if (!active) {
          box = {link.id, line.alignmentX26 + cluster.x26, 0, 0, 0};
          linkTop = cluster.top26;
          linkBottom = cluster.bottom26;
          active = true;
        }
        box.width26 += cluster.advance26;
        linkTop = std::min(linkTop, cluster.top26);
        linkBottom = std::max(linkBottom, cluster.bottom26);
      }
      if (!flush()) return TextStatus::OutOfMemory;
    }
    output.sourceStart = points[first].source;
    output.sourceEnd = points[last].source;
    output.startByte = origin;
    output.endByte = end;
    callbackAttempted = true;
    return emit(context, std::move(output));
  }
};

NativeParagraphLayout::~NativeParagraphLayout() {
  if (state_) {
    state_->~State();
    native_text_free(state_);
  }
}

TextStatus NativeParagraphLayout::windowPrefix(std::string_view text, size_t& bytes, bool finalInput) {
  bytes = 0;
  size_t count = 0;
  while (bytes < text.size() && count < MAX_SCALARS) {
    const size_t start = bytes;
    uint32_t cp;
    if (!native_text::nextUtf8(text, bytes, cp)) {
      bytes = start;
      const auto lead = static_cast<uint8_t>(text[start]);
      const size_t length = lead >= 0xc2 && lead <= 0xdf   ? 2
                            : lead >= 0xe0 && lead <= 0xef ? 3
                            : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                           : 0;
      if (!finalInput && length && text.size() - start < length) {
        for (size_t i = start + 1; i < text.size(); ++i) {
          const auto byte = static_cast<uint8_t>(text[i]);
          if ((byte & 0xc0) != 0x80 ||
              (i == start + 1 && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte >= 0xa0) ||
                                  (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte >= 0x90))))
            return TextStatus::InvalidText;
        }
        return TextStatus::Ok;
      }
      return TextStatus::InvalidText;
    }
    if (bytes > MAX_BYTES) {
      bytes = start;
      break;
    }
    ++count;
  }
  return TextStatus::Ok;
}

TextStatus NativeParagraphLayout::layout(const NativeParagraphView& paragraph, const NativeLayoutOptions& options,
                                         NativeLineCallback emit, void* context, size_t& consumedBytes,
                                         int8_t& paragraphLevel) {
  consumedBytes = 0;
  if (paragraph.paragraphLevel < -1 || paragraph.paragraphLevel > 1 ||
      (paragraph.resolvedLevels && paragraph.paragraphLevel < 0))
    return TextStatus::InvalidText;
  paragraphLevel = paragraph.paragraphLevel;
  if (paragraph.text.empty()) return TextStatus::Ok;
  if (!emit || !options.width) return TextStatus::InvalidText;
  Lock lock(engine_);
  size_t prefix = 0;
  auto status = windowPrefix(paragraph.text, prefix, paragraph.final);
  auto fail = [&](TextStatus error) {
    consumedBytes = 0;
    nativeTextLogError("Native paragraph layout failed");
    return error;
  };
  if (status != TextStatus::Ok) return fail(status);
  if (!prefix) return TextStatus::Ok;
  if (!state_) {
    void* memory = native_text_malloc(sizeof(State));
    if (!memory) {
      engine_.clearCaches();
      memory = native_text_malloc(sizeof(State));
    }
    if (!memory) return fail(TextStatus::OutOfMemory);
    state_ = new (memory) State();
  }
  State& state = *state_;
  state.paragraph = &paragraph;
  state.options = &options;
  state.text = paragraph.text.substr(0, prefix);
  state.final = paragraph.final && prefix == paragraph.text.size();
  status = state.prepare(engine_);
  if (status == TextStatus::OutOfMemory) {
    engine_.clearCaches();
    status = state.prepare(engine_);
  }
  if (status != TextStatus::Ok) return fail(status);
  const size_t count = state.points.size() - 1;
  // A partial parser callback is not a segmentation window boundary.
  if (!state.final && !paragraph.semanticBoundary && count < MAX_SCALARS && prefix < MAX_BYTES) return TextStatus::Ok;
  paragraphLevel = state.level;
  status = state.fit(engine_);
  if (status == TextStatus::OutOfMemory) {
    engine_.clearCaches();
    status = state.fit(engine_);
  }
  if (status != TextStatus::Ok) return fail(status);
  const size_t retain = state.final ? count : (count > RETAIN_SCALARS ? count - RETAIN_SCALARS : 0);
  size_t node = 0, emitted = 0;
  while (node + 1 < state.nodes.size()) {
    size_t next = state.nodes[node].next;
    if (next <= node || next >= state.nodes.size()) return fail(TextStatus::CapacityExceeded);
    if (!state.final && (state.nodes[next].point > retain || next + 1 == state.nodes.size())) break;
    const size_t first = state.nodes[node].point;
    // Revalidate complete final context; every retry chooses a strictly earlier
    // boundary. An indivisible oversized cluster/group is consumed exactly once.
    int32_t width;
    status = state.measure(engine_, first, state.nodes[next].point, width);
    if (status != TextStatus::Ok) return fail(status);
    while (width > state.available(first) && next > node + 1) {
      --next;
      status = state.measure(engine_, first, state.nodes[next].point, width);
      if (status != TextStatus::Ok) return fail(status);
    }
    status = state.emitLine(engine_, first, state.nodes[next].point, state.final && next + 1 == state.nodes.size(),
                            emit, context);
    if (status == TextStatus::OutOfMemory && !state.callbackAttempted) {
      engine_.clearCaches();
      status = state.emitLine(engine_, first, state.nodes[next].point, state.final && next + 1 == state.nodes.size(),
                              emit, context);
    }
    if (status != TextStatus::Ok) return fail(status);
    consumedBytes = state.points[state.nodes[next].point].byte;
    node = next;
    if (options.maxLines && ++emitted >= options.maxLines) break;
  }
  if (!state.final && !consumedBytes && count == MAX_SCALARS) return fail(TextStatus::CapacityExceeded);
  return TextStatus::Ok;
}
