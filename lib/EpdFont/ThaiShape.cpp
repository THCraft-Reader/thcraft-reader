#include "ThaiShape.h"

#include <ThaiCluster.h>
#include <ThaiConfig.h>

#include <algorithm>
#include <cstring>

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

void ThaiShapeCache::invalidate(const ThaiShapeSource& source) {
  for (auto& block : blocks_) {
    if (block.owner == &source) block.owner = nullptr;
  }
}

bool ThaiShapeCache::hasLeases() const {
  for (const auto& stage : stages_) {
    if (stage.references) return true;
  }
  return false;
}

void ThaiShapeCache::touch(uint8_t slot) {
  size_t position = 0;
  while (mru_[position] != slot) ++position;
  while (position) {
    mru_[position] = mru_[position - 1];
    --position;
  }
  mru_[0] = slot;
}

bool ThaiShapeCache::read(const ThaiShapeSource& source, uint32_t offset, uint8_t* output, size_t count) {
  while (count) {
    const uint32_t aligned = offset - offset % BLOCK_BYTES;
    uint8_t slot = BLOCK_COUNT;
    for (uint8_t i = 0; i < BLOCK_COUNT; ++i) {
      if (blocks_[i].owner == &source && blocks_[i].offset == aligned) {
        slot = i;
        break;
      }
    }
    if (slot == BLOCK_COUNT) {
      for (uint8_t i = 0; i < BLOCK_COUNT; ++i) {
        if (!blocks_[i].owner) {
          slot = i;
          break;
        }
      }
      if (slot == BLOCK_COUNT) slot = mru_[BLOCK_COUNT - 1];
      auto& block = blocks_[slot];
      block.owner = nullptr;
      const size_t length = std::min<size_t>(BLOCK_BYTES, source.size_ - aligned);
      if (!source.read_ || !source.read_(source.context_, aligned, block.bytes, length)) return source.fail(aligned);
      block.offset = aligned;
      block.length = static_cast<uint16_t>(length);
      block.owner = &source;
    }
    touch(slot);
    const auto& block = blocks_[slot];
    const size_t within = offset - aligned;
    const size_t length = std::min(count, size_t(block.length) - within);
    std::memcpy(output, block.bytes + within, length);
    output += length;
    offset += length;
    count -= length;
  }
  return true;
}

int ThaiShapeCache::acquire() {
  for (size_t i = 0; i < CLUSTER_SLOTS; ++i) {
    if (!stages_[i].references) {
      stages_[i].references = 1;
      stages_[i].count = 0;
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ThaiShapeCache::retain(uint8_t slot) { ++stages_[slot].references; }

void ThaiShapeCache::release(uint8_t slot) {
  --stages_[slot].references;
  // The callback can destroy this cache: it must be the final access to self.
  if (!hasLeases() && idleCallback_) idleCallback_(this);
}

void ThaiShapeSource::reset() {
  if (cache_) cache_->invalidate(*this);
  resident_ = nullptr;
  context_ = nullptr;
  read_ = nullptr;
  cache_ = nullptr;
  size_ = 0;
  failed_ = false;
  failureCallback_ = nullptr;
  failureContext_ = nullptr;
}

void ThaiShapeSource::setResident(const uint8_t* bytes, uint32_t size) {
  reset();
  resident_ = bytes;
  size_ = size;
}

void ThaiShapeSource::setCached(void* context, Read read, uint32_t size, ThaiShapeCache& cache) {
  reset();
  context_ = context;
  read_ = read;
  size_ = size;
  cache_ = &cache;
}

bool ThaiShapeSource::fail(uint32_t offset) const {
  if (!failed_) {
    failed_ = true;
    if (failureCallback_) failureCallback_(failureContext_, offset);
  }
  return false;
}

bool ThaiShapeSource::readBytes(uint32_t offset, uint8_t* output, size_t count) const {
  if (failed_) return false;
  if (offset > size_ || count > size_ - offset || (count && !output)) return fail(offset);
  if (!count) return true;
  if (resident_) {
    std::memcpy(output, resident_ + offset, count);
    return true;
  }
  return cache_ ? cache_->read(*this, offset, output, count) : fail(offset);
}

bool ThaiShapeView::read(uint32_t offset, uint8_t* output, size_t count) const {
  if (offset > size_ || count > size_ - offset) return source_->fail(offset_);
  return source_->readBytes(offset_ + offset, output, count);
}

bool ThaiShapeView::validate(ThaiShapeSource& source, uint32_t offset, size_t size, Coverage coverage, void* context) {
  *this = ThaiShapeView{};
  if (!coverage || size < 28 || size > MAX_STYLE_BYTES || offset > source.size_ || size > source.size_ - offset ||
      source.failed())
    return false;
  uint8_t header[28];
  if (!source.readBytes(offset, header, sizeof(header))) return false;
  const uint32_t bases = u16(header), suffixes = u16(header + 2);
  const uint32_t dense = u32(header + 4), base = u32(header + 8);
  const uint32_t offsets = u32(header + 12), data = u32(header + 16);
  // Canonical contiguous sections eliminate overlaps and unused/unvalidated ranges.
  if (!bases || !suffixes || bases > DENSE_COUNT || suffixes > DENSE_COUNT || dense != 28 ||
      base != dense + DENSE_COUNT * 4 || offsets != base + bases * 8 || data != offsets + (suffixes + 1) * 4 ||
      data > size || u16(header + 26) != 0 || !u16(header + 24))
    return false;
  uint8_t scratch[8];
  for (uint32_t i = 0; i < DENSE_COUNT; ++i) {
    if (!source.readBytes(offset + dense + i * 4, scratch, 4)) return false;
    const uint16_t b = u16(scratch), s = u16(scratch + 2);
    if (b == UINT16_MAX && s == UINT16_MAX) continue;
    if (b >= bases || s >= suffixes) return false;
  }
  for (uint32_t i = 0; i < bases; ++i) {
    if (!source.readBytes(offset + base + i * 8, scratch, 8) || !coverage(context, u16(scratch))) return false;
  }
  if (!source.readBytes(offset + offsets, scratch, 4)) return false;
  uint32_t start = u32(scratch);
  if (start != data) return false;
  for (uint32_t i = 0; i < suffixes; ++i) {
    if (!source.readBytes(offset + offsets + (i + 1) * 4, scratch, 4)) return false;
    const uint32_t end = u32(scratch);
    if (start >= size || end <= start || end > size) return false;
    if (!source.readBytes(offset + start, scratch, 1)) return false;
    const uint8_t count = scratch[0];
    if (count > 5 || end - start != 1u + count * 8u) return false;
    for (uint8_t j = 0; j < count; ++j) {
      if (!source.readBytes(offset + start + 1 + j * 8, scratch, 8) || !coverage(context, u16(scratch))) return false;
    }
    start = end;
  }
  if (start != size) return false;
  source_ = &source;
  bytes_ = source.resident_ ? source.resident_ + offset : nullptr;
  offset_ = offset;
  size_ = static_cast<uint32_t>(size);
  baseOffset_ = base;
  suffixOffsets_ = offsets;
  suffixData_ = data;
  bases_ = bases;
  suffixes_ = suffixes;
  ascender_ = static_cast<int16_t>(u16(header + 20));
  descender_ = static_cast<int16_t>(u16(header + 22));
  lineAdvance_ = u16(header + 24);
  return true;
}

bool ThaiShapeView::recipe(uint32_t key, uint16_t& base, uint16_t& suffix) const {
  if (!source_ || source_->failed() || key >= DENSE_COUNT) return false;
  uint8_t scratch[4];
  const uint8_t* p = denseIndex_ ? denseIndex_ + key * 4 : bytes_ ? bytes_ + 28 + key * 4 : scratch;
  if (p == scratch && !read(28 + key * 4, scratch, sizeof(scratch))) return false;
  base = u16(p);
  suffix = u16(p + 2);
  if (base == UINT16_MAX && suffix == UINT16_MAX) return false;
  if (base >= bases_ || suffix >= suffixes_) return source_->fail(offset_ + 28 + key * 4);
  return true;
}

bool ThaiShapeView::stageRecipe(uint16_t base, uint16_t suffix, uint8_t* records, uint8_t& count) const {
  uint8_t offsets[8];
  if (!read(suffixOffsets_ + suffix * 4, offsets, sizeof(offsets))) return false;
  const uint32_t start = u32(offsets), end = u32(offsets + 4);
  if (start < suffixData_ || start >= size_ || end <= start || end > size_) return source_->fail(offset_);
  uint8_t suffixCount;
  if (!read(start, &suffixCount, 1)) return false;
  if (suffixCount > 5 || end - start != 1u + suffixCount * 8u) return source_->fail(offset_ + start);
  if (!read(baseOffset_ + base * 8, records, 8) || !read(start + 1, records + 8, suffixCount * 8)) return false;
  count = 1 + suffixCount;
  return true;
}

ThaiGlyphPlacement ThaiShapeView::baseRecord(uint16_t id) const { return glyph(bytes_ + baseOffset_ + id * 8); }
uint8_t ThaiShapeView::suffixCount(uint16_t id) const { return bytes_[u32(bytes_ + suffixOffsets_ + id * 4)]; }
ThaiGlyphPlacement ThaiShapeView::suffixRecord(uint16_t id, uint8_t index) const {
  return glyph(bytes_ + u32(bytes_ + suffixOffsets_ + id * 4) + 1 + index * 8);
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
    if (thai::isTone(tone.value)) {
      t = tone.value - 0xE48 + 1;
      p += tone.bytes;
    }
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
  if (thai::isTone(next.value)) {
    amTone = next.value - 0xE48 + 1;
    p += next.bytes;
  }
  auto am = at(p);
  if (am.value == 0xE33) {
    end = p + am.bytes;
    variant = 77 + amTone;
  } else {
    const int v = vowelIndex(next.value);
    if (v) {
      end += next.bytes;
      next = at(end);
    }
    const int t = terminalIndex(next.value);
    if (t) end += next.bytes;
    variant = v * 7 + t;
  }
  key = (cp - 0xE01) * 82 + variant;
  return true;
}

void ThaiGlyphCursor::release() {
  auto* cache = cache_;
  cache_ = nullptr;
  shape_ = nullptr;
  if (cache) cache->release(stage_);
}

ThaiGlyphCursor::~ThaiGlyphCursor() { release(); }

void ThaiGlyphCursor::copyState(const ThaiGlyphCursor& other) {
  text_ = other.text_;
  shape_ = other.shape_;
  cache_ = other.cache_;
  consumed_ = other.consumed_;
  offset_ = other.offset_;
  unitEnd_ = other.unitEnd_;
  base_ = other.base_;
  suffix_ = other.suffix_;
  record_ = other.record_;
  records_ = other.records_;
  stage_ = other.stage_;
  stageUnit_ = other.stageUnit_;
  first_ = other.first_;
}

ThaiGlyphCursor::ThaiGlyphCursor(const ThaiGlyphCursor& other) {
  copyState(other);
  if (cache_) cache_->retain(stage_);
}

ThaiGlyphCursor& ThaiGlyphCursor::operator=(const ThaiGlyphCursor& other) {
  if (this != &other) {
    release();
    copyState(other);
    if (cache_) cache_->retain(stage_);
  }
  return *this;
}

ThaiGlyphCursor::ThaiGlyphCursor(ThaiGlyphCursor&& other) noexcept {
  copyState(other);
  other.cache_ = nullptr;
  other.shape_ = nullptr;
}

ThaiGlyphCursor& ThaiGlyphCursor::operator=(ThaiGlyphCursor&& other) noexcept {
  if (this != &other) {
    release();
    copyState(other);
    other.cache_ = nullptr;
    other.shape_ = nullptr;
  }
  return *this;
}

bool ThaiGlyphCursor::begin(std::string_view text, const ThaiShapeView& shape) {
  release();
  consumed_ = offset_ = unitEnd_ = 0;
  records_ = record_ = stageUnit_ = 0;
  first_ = true;
#if THAI_SHAPING
  if (!shape.valid() || text.empty() || !thai::isThai(thai::detail::decode(text, 0, true).value)) return false;
  thai::Cluster cluster{};
  size_t end = 0;
  if (!thai::nextCluster(text, end, cluster, true)) return false;
  consumed_ = end;
  if (!cluster.valid || shape.failed()) return false;
  if (end > 128) return shape.source_->fail(shape.offset_);
  text_ = text.substr(0, end);
  bool hasRecipe = false;
  for (size_t p = 0; p < end;) {
    size_t next;
    uint32_t key;
    uint16_t base, suffix;
    if (!unit(p, next, key) || next <= p || next > end) return false;
    if (key != NATIVE) {
      if (!shape.recipe(key, base, suffix)) return false;
      hasRecipe = true;
    }
    p = next;
  }
  if (!hasRecipe) return false;
  if (!shape.bytes_) {
    auto* cache = shape.source_->cache_;
    const int slot = cache->acquire();
    if (slot < 0) return shape.source_->fail(shape.offset_);
    cache_ = cache;
    stage_ = static_cast<uint8_t>(slot);
    auto& stage = cache->stages_[stage_];
    for (size_t p = 0; p < end;) {
      size_t next;
      uint32_t key;
      if (stage.count == 32 || !unit(p, next, key)) {
        shape.source_->fail(shape.offset_);
        release();
        return false;
      }
      auto& staged = stage.units[stage.count];
      staged.sourceBegin = static_cast<uint8_t>(p);
      staged.sourceEnd = static_cast<uint8_t>(next);
      staged.recordCount = 0;
      if (key != NATIVE) {
        uint16_t base, suffix;
        if (!shape.recipe(key, base, suffix) || !shape.stageRecipe(base, suffix, staged.records, staged.recordCount)) {
          release();
          return false;
        }
      }
      ++stage.count;
      p = next;
    }
    // Cached iteration owns only the immutable stage, never the owner's view.
    return true;
  }
  shape_ = &shape;
  return true;
#else
  (void)text;
  (void)shape;
  return false;
#endif
}

bool ThaiGlyphCursor::next(ThaiGlyphPlacement& out) {
  if ((!shape_ && !cache_) || offset_ >= consumed_) return false;
  if (cache_) {
    const auto& unit = cache_->stages_[stage_].units[stageUnit_];
    out = unit.recordCount ? glyph(unit.records + record_ * 8)
                           : ThaiGlyphPlacement{thai::detail::decode(text_, unit.sourceBegin, true).value, 0, 0, 0,
                                                ThaiGlyphPlacement::Native};
    out.sourceBegin = unit.sourceBegin;
    out.sourceEnd = unit.sourceEnd;
    if (unit.recordCount && record_ == 0) out.flags |= ThaiGlyphPlacement::RecipeStart;
    if (!unit.recordCount || ++record_ == unit.recordCount) {
      if (unit.recordCount) out.flags |= ThaiGlyphPlacement::RecipeEnd;
      record_ = 0;
      offset_ = unit.sourceEnd;
      ++stageUnit_;
    }
    if (first_) {
      out.flags |= ThaiGlyphPlacement::ClusterStart;
      first_ = false;
    }
    if (offset_ == consumed_) {
      out.flags |= ThaiGlyphPlacement::ClusterEnd;
      release();
    }
    return true;
  }
  const size_t sourceBegin = offset_;
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
  out.sourceBegin = sourceBegin;
  out.sourceEnd = unitEnd_;
  if (first_) {
    out.flags |= ThaiGlyphPlacement::ClusterStart;
    first_ = false;
  }
  if (offset_ == consumed_) out.flags |= ThaiGlyphPlacement::ClusterEnd;
  return true;
}
