#include "ThaiShape.h"

#include <ThaiCluster.h>
#include <ThaiConfig.h>

#include <algorithm>
#include <climits>

#include "EpdFontData.h"

int ThaiShapeView::glyphIndex(const uint32_t cp) {
  if (cp >= 0xE01 && cp <= 0xE2E) return static_cast<int>(cp - 0xE01);
  if (cp == 0xE31) return 46;
  if (cp == 0xE32) return 47;
  if (cp >= 0xE34 && cp <= 0xE3A) return static_cast<int>(cp - 0xE34) + 48;
  if (cp >= 0xE47 && cp <= 0xE4E) return static_cast<int>(cp - 0xE47) + 55;
  return -1;
}

const ThaiGlyphMetrics* ThaiShapeView::glyph(const uint32_t cp) const {
  const int index = glyphIndex(cp);
  return metrics_ && index >= 0 && (coverage_ & (uint64_t{1} << index)) ? metrics_ + index : nullptr;
}

bool ThaiShapeView::hasNativeLigature(const uint32_t left, const uint32_t right) const {
  if (!nativeFont_ || !nativeFont_->ligaturePairs || left > 0xFFFF || right > 0xFFFF) return false;
  const auto* end = nativeFont_->ligaturePairs + nativeFont_->ligaturePairCount;
  const uint32_t key = (left << 16) | right;
  const auto* pair = std::lower_bound(nativeFont_->ligaturePairs, end, key,
                                      [](const EpdLigaturePair& p, uint32_t value) { return p.pair < value; });
  return pair != end && pair->pair == key && pair->ligatureCp != 0;
}
namespace {
bool firstMark(const uint32_t cp) {
  return thai::isAboveVowel(cp) || thai::isBelowVowel(cp) || cp == 0xE47 || cp == 0xE4D;
}
bool terminalMark(const uint32_t cp) { return (cp >= 0xE48 && cp <= 0xE4C) || cp == 0xE4E; }
bool fixedOffset(const int32_t pixels, int16_t& result) {
  const int32_t value = pixels * 16;
  if (value < INT16_MIN || value > INT16_MAX) return false;
  result = static_cast<int16_t>(value);
  return true;
}
}  // namespace

bool ThaiGlyphCursor::prepareUnit(const size_t offset) {
  count_ = record_ = 0;
  const auto scalar = thai::detail::decode(text_, offset, true);
  if (!scalar.valid) return false;
  const uint32_t cp = scalar.value;
  unitEnd_ = offset + scalar.bytes;
  if (!thai::isBase(cp)) {
    if (thai::isCombiningSign(cp) || cp == 0xE33) return false;
    records_[count_++] = {cp, 0, 0, 0, ThaiGlyphPlacement::Native};
    return true;
  }

  const auto* base = shape_->glyph(cp);
  if (!base || !base->width || !base->height) return false;
  const auto at = [&](size_t p) { return thai::detail::decode(text_, p, true); };
  uint32_t marks[2]{};
  uint8_t markCount = 0;
  bool saraAm = false;
  auto next = at(unitEnd_);
  // Both Unicode spellings emit nikhahit before the tone and Sara AA after it.
  if (next.value == 0xE4D) {
    size_t p = unitEnd_ + next.bytes;
    const auto tone = at(p);
    if (thai::isTone(tone.value)) p += tone.bytes;
    const auto aa = at(p);
    if (aa.value == 0xE32) {
      marks[markCount++] = 0xE4D;
      if (thai::isTone(tone.value)) marks[markCount++] = tone.value;
      unitEnd_ = p + aa.bytes;
      saraAm = true;
    }
  }
  if (!saraAm) {
    size_t p = unitEnd_;
    uint32_t tone = 0;
    if (thai::isTone(next.value)) {
      tone = next.value;
      p += next.bytes;
    }
    const auto am = at(p);
    if (am.value == 0xE33) {
      marks[markCount++] = 0xE4D;
      if (tone) marks[markCount++] = tone;
      unitEnd_ = p + am.bytes;
      saraAm = true;
    } else {
      if (firstMark(next.value)) {
        marks[markCount++] = next.value;
        unitEnd_ += next.bytes;
        next = at(unitEnd_);
      }
      if (terminalMark(next.value)) {
        marks[markCount++] = next.value;
        unitEnd_ += next.bytes;
      }
    }
  }
  if (unitEnd_ > text_.size()) return false;
  records_[count_++] = {cp, base->advanceX, 0, 0, 0};
  // A plain base stays native so an otherwise unpositioned cluster can use ligatures.
  if (!markCount) {
    records_[0].flags = ThaiGlyphPlacement::Native;
    return true;
  }

  const int advance = (base->advanceX + 8) / 16;
  const auto* body = shape_->glyph(0xE01);
  const int bodyTop = body && body->height ? std::min<int>(base->top, body->top) : base->top;
  // Sloped or left-side tails (ส, ช, ฬ) are not right-side ascenders.
  const bool avoidRightAscender = (cp == 0xE1B || cp == 0xE1D || cp == 0xE1F) && base->top > bodyTop + 1;  // ป ฝ ฟ
  const int right = base->left + base->width - (avoidRightAscender ? std::max<int>(1, base->width / 4) : 0);
  int above = bodyTop;
  int below = base->top - base->height;
  for (uint8_t i = 0; i < markCount; ++i) {
    const uint32_t markCp = marks[i];
    const auto* mark = shape_->glyph(markCp);
    if (!mark || !mark->width || !mark->height) return false;
    // Native bearings preserve the font's right-side Thai attachment. Only move
    // an upper mark left when its bitmap would reach a right-side ascender.
    const int x = !thai::isBelowVowel(markCp) && avoidRightAscender
                      ? std::min(0, right - (advance + mark->left + mark->width))
                      : 0;
    int y;
    if (thai::isBelowVowel(markCp)) {
      y = std::min(0, below - 1 - mark->top);
      below = mark->top + y - mark->height;
    } else {
      y = std::max(0, above + 1 - (mark->top - mark->height));
      above = mark->top + y;
    }
    Record record{markCp, 0, 0, 0, 0};
    if (!fixedOffset(x, record.xOffset) || !fixedOffset(y, record.yOffset)) return false;
    records_[count_++] = record;
  }
  if (saraAm) {
    const auto* aa = shape_->glyph(0xE32);
    if (!aa) return false;
    records_[count_++] = {0xE32, aa->advanceX, 0, 0, 0};
  }
  records_[0].flags = ThaiGlyphPlacement::RecipeStart;
  records_[count_ - 1].flags |= ThaiGlyphPlacement::RecipeEnd;
  return true;
}

bool ThaiGlyphCursor::begin(const std::string_view text, const ThaiShapeView& shape, const bool nativeLigatures) {
  shape_ = nullptr;
  consumed_ = offset_ = unitEnd_ = 0;
  count_ = record_ = 0;
#if THAI_SHAPING
  if (!shape.valid() || text.empty() || !thai::isThai(thai::detail::decode(text, 0, true).value)) return false;
  thai::Cluster cluster{};
  if (!thai::nextCluster(text, consumed_, cluster, true) || !cluster.valid) return false;
  for (size_t p = 0; p < consumed_;) {
    const auto scalar = thai::detail::decode(text, p, true);
    p += scalar.bytes;
    const auto next = thai::detail::decode(text, p, true);
    if (nativeLigatures && !thai::isCombiningSign(scalar.value) && next.valid &&
        shape.hasNativeLigature(scalar.value, next.value))
      return false;
  }
  text_ = text.substr(0, consumed_);
  shape_ = &shape;
  bool positioned = false;
  // Admit the whole outer cluster before exposing any placements.
  for (size_t p = 0; p < consumed_;) {
    if (!prepareUnit(p) || unitEnd_ <= p || unitEnd_ > consumed_) {
      shape_ = nullptr;
      count_ = 0;
      return false;
    }
    positioned |= !(records_[0].flags & ThaiGlyphPlacement::Native);
    p = unitEnd_;
  }
  count_ = record_ = 0;
  if (!positioned) shape_ = nullptr;
  return positioned;
#else
  (void)text;
  (void)shape;
  (void)nativeLigatures;
  return false;
#endif
}

bool ThaiGlyphCursor::next(ThaiGlyphPlacement& out) {
  if (!shape_ || offset_ >= consumed_) return false;
  if (!count_ && !prepareUnit(offset_)) return false;
  const auto& record = records_[record_];
  out = {record.codepoint, record.advance, record.xOffset, record.yOffset, record.flags, offset_, unitEnd_};
  if (!offset_ && !record_) out.flags |= ThaiGlyphPlacement::ClusterStart;
  if (++record_ == count_) {
    offset_ = unitEnd_;
    count_ = record_ = 0;
  }
  if (offset_ == consumed_) out.flags |= ThaiGlyphPlacement::ClusterEnd;
  return true;
}
