#include "ThaiShape.h"

#include <ThaiCluster.h>
#include <ThaiConfig.h>

#include <algorithm>
#include <cstring>

namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u24(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
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
  for (auto& recipe : recipes_) {
    if (recipe.owner == &source) recipe.owner = nullptr;
  }
}

ThaiShapeCache::Recipe* ThaiShapeCache::recipeSet(const ThaiShapeSource& source, uint32_t denseOffset) {
  // The companion size spreads different fonts' identical keys across sets.
  const uint32_t mixed = (denseOffset / ThaiShapeView::ENTRY_BYTES + source.size_) * 2654435761u;
  return recipes_ + (mixed >> 16) % RECIPE_SETS * RECIPE_WAYS;
}

const ThaiShapeCache::Recipe* ThaiShapeCache::findRecipe(const ThaiShapeSource& source, uint32_t denseOffset) {
  Recipe* set = recipeSet(source, denseOffset);
  for (size_t way = 0; way < RECIPE_WAYS; ++way) {
    if (set[way].owner == &source && set[way].denseOffset == denseOffset) {
      set[way].recent = 1;
      return set + way;
    }
  }
  return nullptr;
}

void ThaiShapeCache::storeRecipe(const ThaiShapeSource& source, uint32_t denseOffset, const uint8_t* records,
                                 uint8_t count) {
  if (count > Recipe::MAX_RECORDS) return;
  Recipe* set = recipeSet(source, denseOffset);
  // Second-chance victim: an empty way, else the first not hit since the last sweep.
  size_t victim = RECIPE_WAYS;
  for (size_t way = 0; way < RECIPE_WAYS && victim == RECIPE_WAYS; ++way) {
    if (!set[way].owner) victim = way;
  }
  for (size_t way = 0; way < RECIPE_WAYS && victim == RECIPE_WAYS; ++way) {
    if (!set[way].recent) victim = way;
  }
  if (victim == RECIPE_WAYS) {
    for (size_t way = 0; way < RECIPE_WAYS; ++way) set[way].recent = 0;
    victim = 0;
  }
  auto& recipe = set[victim];
  recipe.owner = &source;
  recipe.denseOffset = denseOffset;
  recipe.recordCount = count;
  recipe.recent = 1;
  std::memcpy(recipe.records, records, count * 8);
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

bool ThaiShapeStream::read(uint8_t* output, size_t count) {
  if (!source_.readBytes(offset_, output, count)) return false;
  if (observe_) observe_(context_, output, count);
  offset_ += static_cast<uint32_t>(count);
  return true;
}

bool ThaiShapeView::read(uint32_t offset, uint8_t* output, size_t count) const {
  if (offset > size_ || count > size_ - offset) return source_->fail(offset_);
  return source_->readBytes(offset_ + offset, output, count);
}

bool ThaiShapeView::validate(ThaiShapeStream& stream, size_t size, Coverage coverage, void* context,
                             uint8_t* denseCopy) {
  *this = ThaiShapeView{};
  auto& source = stream.source();
  const uint32_t offset = stream.offset();
  if (!coverage || size < HEADER_BYTES + DENSE_BYTES || size > MAX_STYLE_BYTES || offset > source.size_ ||
      size > source.size_ - offset || source.failed())
    return false;
  uint8_t header[HEADER_BYTES];
  if (!stream.read(header, sizeof(header))) return false;
  const uint32_t bases = u16(header), records = u16(header + 2);
  // Canonical contiguous sections leave no unused or unvalidated bytes.
  if (!bases || bases > MAX_BASES || records > MAX_RECORDS || !u16(header + 8) || u16(header + 10) != 0 ||
      size != HEADER_BYTES + DENSE_BYTES + (bases + records) * RECORD_BYTES)
    return false;
  constexpr uint32_t CHUNK_ENTRIES = 64;
  uint8_t chunk[CHUNK_ENTRIES * ENTRY_BYTES];
  static_assert(sizeof(chunk) % RECORD_BYTES == 0, "Record pass reuses the entry chunk");
  for (uint32_t key = 0; key < DENSE_COUNT;) {
    const uint32_t entries = std::min(CHUNK_ENTRIES, DENSE_COUNT - key);
    if (!stream.read(chunk, entries * ENTRY_BYTES)) return false;
    for (uint32_t i = 0; i < entries; ++i) {
      const uint32_t entry = u24(chunk + i * ENTRY_BYTES);
      if (entry == ABSENT_ENTRY) continue;
      const uint32_t base = entry & 0x7F, marks = (entry >> 7) & 7, first = entry >> 10;
      // Base-only recipes canonically point at record 0.
      if (base >= bases || marks > MAX_MARKS || first + marks > records || (!marks && first)) return false;
    }
    if (denseCopy) std::memcpy(denseCopy + key * ENTRY_BYTES, chunk, entries * ENTRY_BYTES);
    key += entries;
  }
  // Base and mark records are contiguous; every one must name a CPFont glyph.
  for (uint32_t remaining = bases + records; remaining;) {
    const uint32_t count = std::min<uint32_t>(remaining, sizeof(chunk) / RECORD_BYTES);
    if (!stream.read(chunk, count * RECORD_BYTES)) return false;
    for (uint32_t i = 0; i < count; ++i) {
      if (!coverage(context, u16(chunk + i * RECORD_BYTES))) return false;
    }
    remaining -= count;
  }
  source_ = &source;
  offset_ = offset;
  size_ = static_cast<uint32_t>(size);
  bases_ = static_cast<uint16_t>(bases);
  records_ = static_cast<uint16_t>(records);
  ascender_ = static_cast<int16_t>(u16(header + 4));
  descender_ = static_cast<int16_t>(u16(header + 6));
  lineAdvance_ = u16(header + 8);
  return true;
}

bool ThaiShapeView::recipe(uint32_t key, Entry& entry) const {
  if (!source_ || source_->failed() || key >= DENSE_COUNT) return false;
  uint8_t scratch[ENTRY_BYTES];
  const uint8_t* resident = bytes();
  const uint8_t* p = denseIndex_ ? denseIndex_ + key * ENTRY_BYTES
                     : resident  ? resident + HEADER_BYTES + key * ENTRY_BYTES
                                 : scratch;
  if (p == scratch && !read(HEADER_BYTES + key * ENTRY_BYTES, scratch, sizeof(scratch))) return false;
  const uint32_t value = u24(p);
  if (value == ABSENT_ENTRY) return false;
  entry.base = value & 0x7F;
  entry.marks = (value >> 7) & 7;
  entry.first = static_cast<uint16_t>(value >> 10);
  // SD-backed entries are re-checked: the file is read again after validation.
  if (entry.base >= bases_ || entry.marks > MAX_MARKS || entry.first + entry.marks > records_)
    return source_->fail(denseOffset(key));
  return true;
}

bool ThaiShapeView::stageRecipe(const Entry& entry, uint8_t* records, uint8_t& count) const {
  if (!read(baseOffset() + entry.base * RECORD_BYTES, records, RECORD_BYTES) ||
      !read(markOffset() + entry.first * RECORD_BYTES, records + RECORD_BYTES, entry.marks * RECORD_BYTES))
    return false;
  count = 1 + entry.marks;
  return true;
}

ThaiGlyphPlacement ThaiShapeView::baseRecord(uint8_t id) const {
  return glyph(bytes() + baseOffset() + id * RECORD_BYTES);
}
ThaiGlyphPlacement ThaiShapeView::markRecord(uint16_t index) const {
  return glyph(bytes() + markOffset() + index * RECORD_BYTES);
}

bool ThaiGlyphCursor::cachedRecipe(const ThaiShapeView& shape, uint32_t key, uint8_t* records, uint8_t& count) {
  auto& cache = *shape.source_->cache_;
  const uint32_t denseOffset = shape.denseOffset(key);
  if (const auto* known = cache.findRecipe(*shape.source_, denseOffset)) {
    count = static_cast<uint8_t>(known->recordCount);
    std::memcpy(records, known->records, count * 8);
    return count != 0;
  }
  ThaiShapeView::Entry entry;
  count = 0;
  if (shape.recipe(key, entry)) {
    if (!shape.stageRecipe(entry, records, count)) return false;
  } else if (shape.failed()) {
    return false;
  }
  cache.storeRecipe(*shape.source_, denseOffset, records, count);
  return count != 0;
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
  firstMark_ = other.firstMark_;
  base_ = other.base_;
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
  auto* cache = shape.bytes() ? nullptr : shape.source_->cache_;
  bool hasRecipe = false;
  for (size_t p = 0; p < end;) {
    size_t next;
    uint32_t key;
    if (!unit(p, next, key) || next <= p || next > end) return false;
    if (key != NATIVE) {
      ThaiShapeView::Entry entry;
      uint8_t records[sizeof(ThaiShapeCache::Unit::records)], count;
      if (cache ? !cachedRecipe(shape, key, records, count) : !shape.recipe(key, entry)) return false;
      hasRecipe = true;
    }
    p = next;
  }
  if (!hasRecipe) return false;
  if (cache) {
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
      if (key != NATIVE && !cachedRecipe(shape, key, staged.records, staged.recordCount)) {
        release();
        return false;
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
      ThaiShapeView::Entry entry{};
      // Admitted by begin(); only a fault latched since then fails here.
      if (!shape_->recipe(key, entry)) return false;
      base_ = entry.base;
      firstMark_ = entry.first;
      records_ = 1 + entry.marks;
      record_ = 0;
    }
  }
  if (records_) {
    out = record_ == 0 ? shape_->baseRecord(base_) : shape_->markRecord(firstMark_ + record_ - 1);
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
