#include "ThaiShape.h"

#include <ThaiCluster.h>
#include <ThaiConfig.h>

namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
ThaiGlyphPlacement glyph(const uint8_t* p) {
  return {u16(p), u16(p + 2), static_cast<int16_t>(u16(p + 4)), static_cast<int16_t>(u16(p + 6)), 0};
}
constexpr uint32_t NATIVE = UINT32_MAX;
int vowelIndex(uint32_t cp) {
  if (cp == 0xE31) return 1;
  if (cp >= 0xE34 && cp <= 0xE3A) return int(cp - 0xE34) + 2;
  if (cp == 0xE47) return 9;
  if (cp == 0xE4D) return 10;
  return 0;
}
int terminalIndex(uint32_t cp) {
  if (cp >= 0xE48 && cp <= 0xE4C) return int(cp - 0xE48) + 1;
  return cp == 0xE4E ? 6 : 0;
}
}  // namespace

bool ThaiShapeView::validate(const uint8_t* bytes, size_t size, Coverage coverage, void* context) {
  bytes_ = nullptr;
  if (!bytes || !coverage || size < 28 || size > MAX_STYLE_BYTES) return false;
  const uint32_t bases = u16(bytes), suffixes = u16(bytes + 2);
  const uint32_t dense = u32(bytes + 4), base = u32(bytes + 8);
  const uint32_t offsets = u32(bytes + 12), data = u32(bytes + 16);
  // Canonical contiguous sections eliminate overlaps and unused/unvalidated ranges.
  if (!bases || !suffixes || bases > DENSE_COUNT || suffixes > DENSE_COUNT ||
      dense != 28 || base != dense + DENSE_COUNT * 4 ||
      offsets != base + bases * 8 || data != offsets + (suffixes + 1) * 4 || data > size ||
      u16(bytes + 26) != 0 || !u16(bytes + 24)) return false;
  for (uint32_t i = 0; i < DENSE_COUNT; ++i) {
    const uint16_t b = u16(bytes + dense + i * 4), s = u16(bytes + dense + i * 4 + 2);
    if (b == UINT16_MAX && s == UINT16_MAX) continue;
    if (b >= bases || s >= suffixes) return false;
  }
  for (uint32_t i = 0; i < bases; ++i) {
    if (!coverage(context, u16(bytes + base + i * 8))) return false;
  }
  uint32_t start = u32(bytes + offsets);
  if (start != data) return false;
  for (uint32_t i = 0; i < suffixes; ++i) {
    const uint32_t end = u32(bytes + offsets + (i + 1) * 4);
    if (start >= size || end <= start || end > size) return false;
    const uint8_t count = bytes[start];
    if (count > 5 || end - start != 1u + count * 8u) return false;
    for (uint8_t j = 0; j < count; ++j) {
      if (!coverage(context, u16(bytes + start + 1 + j * 8))) return false;
    }
    start = end;
  }
  if (start != size) return false;
  bytes_ = bytes;
  return true;
}

int16_t ThaiShapeView::ascender() const { return bytes_ ? static_cast<int16_t>(u16(bytes_ + 20)) : 0; }
int16_t ThaiShapeView::descender() const { return bytes_ ? static_cast<int16_t>(u16(bytes_ + 22)) : 0; }
uint16_t ThaiShapeView::lineAdvance() const { return bytes_ ? u16(bytes_ + 24) : 0; }
bool ThaiShapeView::recipe(uint32_t key, uint16_t& base, uint16_t& suffix) const {
  if (!bytes_ || key >= DENSE_COUNT) return false;
  const uint8_t* p = bytes_ + 28 + key * 4;
  base = u16(p);
  suffix = u16(p + 2);
  return base != UINT16_MAX;
}
ThaiGlyphPlacement ThaiShapeView::baseRecord(uint16_t id) const {
  return glyph(bytes_ + u32(bytes_ + 8) + id * 8);
}
uint8_t ThaiShapeView::suffixCount(uint16_t id) const {
  return bytes_[u32(bytes_ + u32(bytes_ + 12) + id * 4)];
}
ThaiGlyphPlacement ThaiShapeView::suffixRecord(uint16_t id, uint8_t index) const {
  return glyph(bytes_ + u32(bytes_ + u32(bytes_ + 12) + id * 4) + 1 + index * 8);
}

bool ThaiGlyphCursor::unit(size_t offset, size_t& end, uint32_t& key) const {
  auto scalar = thai::detail::decode(text_, offset, true);
  if (!scalar.valid) return false;
  const uint32_t cp = scalar.value;
  end = offset + scalar.bytes;
  key = NATIVE;
  if (!thai::isBase(cp)) {
    // Dependent combining signs (including orphan/repeated signs) never form native islands.
    return !thai::isCombiningSign(cp) && cp != 0xE33;
  }
  const auto at = [&](size_t p) { return thai::detail::decode(text_, p, true); };
  auto next = at(end);
  uint32_t variant = 0;
  if (next.value == 0xE4D) {
    size_t p = end + next.bytes;
    auto tone = at(p);
    uint32_t t = 0;
    if (thai::isTone(tone.value)) { t = tone.value - 0xE48 + 1; p += tone.bytes; }
    const auto aa = at(p);
    if (aa.value == 0xE32) {
      end = p + aa.bytes;
      variant = 77 + t;
      key = (cp - 0xE01) * 82 + variant;
      return true;
    }
  }
  // Sara-am is reordered/decomposed by the baked recipe, never by rewriting source bytes.
  size_t p = end;
  uint32_t amTone = 0;
  if (thai::isTone(next.value)) { amTone = next.value - 0xE48 + 1; p += next.bytes; }
  auto am = at(p);
  if (am.value == 0xE33) {
    end = p + am.bytes;
    variant = 77 + amTone;
  } else {
    const int v = vowelIndex(next.value);
    if (v) { end += next.bytes; next = at(end); }
    const int t = terminalIndex(next.value);
    if (t) end += next.bytes;
    variant = v * 7 + t;
  }
  key = (cp - 0xE01) * 82 + variant;
  return true;
}

bool ThaiGlyphCursor::begin(std::string_view text, const ThaiShapeView& shape) {
  shape_ = nullptr;
  consumed_ = offset_ = unitEnd_ = 0;
  records_ = record_ = 0;
  first_ = true;
#if THAI_SHAPING
  if (!shape.valid() || text.empty() || !thai::isThai(thai::detail::decode(text, 0, true).value)) return false;
  thai::Cluster cluster{};
  size_t end = 0;
  if (!thai::nextCluster(text, end, cluster, true)) return false;
  consumed_ = end;
  if (!cluster.valid) return false;
  text_ = text.substr(0, end);
  bool hasRecipe = false;
  for (size_t p = 0; p < end;) {
    size_t next;
    uint32_t key;
    uint16_t base, suffix;
    if (!unit(p, next, key)) return false;
    if (key != NATIVE) {
      if (!shape.recipe(key, base, suffix)) return false;
      hasRecipe = true;
    }
    p = next;
  }
  if (!hasRecipe) return false;
  shape_ = &shape;
  return true;
#else
  (void)text;
  (void)shape;
  return false;
#endif
}

bool ThaiGlyphCursor::next(ThaiGlyphPlacement& out) {
  if (!shape_ || offset_ >= consumed_) return false;
  if (!records_) {
    uint32_t key;
    if (!unit(offset_, unitEnd_, key)) return false;  // Already checked by begin().
    if (key == NATIVE) {
      out = {thai::detail::decode(text_, offset_, true).value, 0, 0, 0, ThaiGlyphPlacement::Native};
      offset_ = unitEnd_;
    } else {
      shape_->recipe(key, base_, suffix_);
      records_ = 1 + shape_->suffixCount(suffix_);
      record_ = 0;
    }
  }
  if (records_) {
    out = record_ == 0 ? shape_->baseRecord(base_) : shape_->suffixRecord(suffix_, record_ - 1);
    if (record_ == 0) out.flags |= ThaiGlyphPlacement::RecipeStart;
    if (++record_ == records_) {
      out.flags |= ThaiGlyphPlacement::RecipeEnd;
      records_ = 0;
      offset_ = unitEnd_;
    }
  }
  if (first_) { out.flags |= ThaiGlyphPlacement::ClusterStart; first_ = false; }
  if (offset_ == consumed_) out.flags |= ThaiGlyphPlacement::ClusterEnd;
  return true;
}
