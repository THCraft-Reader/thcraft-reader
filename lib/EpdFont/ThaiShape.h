#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

struct ThaiGlyphPlacement {
  uint32_t codepoint;
  uint16_t advanceFP;
  int16_t xOffsetFP;
  int16_t yOffsetFP;
  uint8_t flags;
  // Original byte span of one native scalar or atomic base/mark recipe.
  // Every glyph of a reordered/decomposed recipe carries the same span.
  size_t sourceBegin = 0;
  size_t sourceEnd = 0;
  enum Flag : uint8_t { Native = 1, ClusterStart = 2, ClusterEnd = 4, RecipeStart = 8, RecipeEnd = 16 };
};

class ThaiShapeSource;

// Shared by every paged font. All operations require the font/render lifetime lock.
class ThaiShapeCache {
 public:
  static constexpr size_t BLOCK_BYTES = 512;
  static constexpr size_t BLOCK_COUNT = 8;
  static constexpr size_t CLUSTER_SLOTS = 2;
  static constexpr size_t RECIPE_SETS = 16;
  static constexpr size_t RECIPE_WAYS = 8;
  void invalidate(const ThaiShapeSource& source);
  bool hasLeases() const;
  void setIdleCallback(void (*callback)(ThaiShapeCache*)) { idleCallback_ = callback; }

 private:
  friend class ThaiShapeSource;
  friend class ThaiGlyphCursor;
  struct Block {
    const ThaiShapeSource* owner = nullptr;
    uint32_t offset = 0;
    uint16_t length = 0;
    uint8_t bytes[BLOCK_BYTES];
  };
  struct Unit {
    uint8_t sourceBegin;
    uint8_t sourceEnd;
    uint8_t recordCount;
    uint8_t records[48];
  };
  static_assert(sizeof(Unit) == 51, "Cluster units must remain byte-packed");
  struct Stage {
    Unit units[32];
    uint32_t references = 0;
    uint8_t count = 0;
  };
  // A resolved recipe keyed by its lookup-entry companion offset (unique per
  // style and key), so a repeated cluster stages without companion reads.
  struct Recipe {
    static constexpr uint8_t MAX_RECORDS = 4;  // longer recipes stay on the block path
    const ThaiShapeSource* owner = nullptr;
    uint32_t denseOffset : 24;
    uint32_t recordCount : 3;  // 0 = the dense index maps no recipe
    uint32_t recent : 1;
    uint8_t records[MAX_RECORDS * 8];
  };
  bool read(const ThaiShapeSource& source, uint32_t offset, uint8_t* output, size_t count);
  Recipe* recipeSet(const ThaiShapeSource& source, uint32_t denseOffset);
  const Recipe* findRecipe(const ThaiShapeSource& source, uint32_t denseOffset);
  void storeRecipe(const ThaiShapeSource& source, uint32_t denseOffset, const uint8_t* records, uint8_t count);
  void touch(uint8_t slot);
  int acquire();
  void retain(uint8_t slot);
  void release(uint8_t slot);
  Block blocks_[BLOCK_COUNT];
  Stage stages_[CLUSTER_SLOTS];
  Recipe recipes_[RECIPE_SETS * RECIPE_WAYS];
  uint8_t mru_[BLOCK_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7};
  void (*idleCallback_)(ThaiShapeCache*) = nullptr;
};
static_assert(sizeof(ThaiShapeCache) <= 14 * 1024, "Shared shaping working set exceeds 14 KiB");

// Stable owner identity: backing memory/file outlives all borrowing views/cursors.
class ThaiShapeSource {
 public:
  using Read = bool (*)(void* context, uint32_t offset, uint8_t* output, size_t count);
  ThaiShapeSource() = default;
  ~ThaiShapeSource() { reset(); }
  ThaiShapeSource(const ThaiShapeSource&) = delete;
  ThaiShapeSource& operator=(const ThaiShapeSource&) = delete;
  // Views follow the current backing, so switching resident -> cached under the
  // render lock keeps every validated view usable without revalidation.
  void setResident(const uint8_t* bytes, uint32_t size);
  void setCached(void* context, Read read, uint32_t size, ThaiShapeCache& cache);
  void reset();
  bool readBytes(uint32_t offset, uint8_t* output, size_t count) const;
  bool failed() const { return failed_; }
  void setFailureCallback(void (*callback)(void*, uint32_t), void* context) {
    failureCallback_ = callback;
    failureContext_ = context;
  }

 private:
  friend class ThaiShapeCache;
  friend class ThaiShapeView;
  friend class ThaiGlyphCursor;
  bool fail(uint32_t offset) const;
  const uint8_t* resident_ = nullptr;
  void* context_ = nullptr;
  Read read_ = nullptr;
  ThaiShapeCache* cache_ = nullptr;
  uint32_t size_ = 0;
  mutable bool failed_ = false;
  void (*failureCallback_)(void*, uint32_t) = nullptr;
  void* failureContext_ = nullptr;
};

// Load-time sequential reader: each byte is read once, in file order, and handed
// to the observer, so validation and the payload CRC share one pass.
class ThaiShapeStream {
 public:
  using Observe = void (*)(void* context, const uint8_t* bytes, size_t count);
  ThaiShapeStream(ThaiShapeSource& source, uint32_t offset, Observe observe, void* context)
      : source_(source), observe_(observe), context_(context), offset_(offset) {}
  bool read(uint8_t* output, size_t count);
  ThaiShapeSource& source() const { return source_; }
  uint32_t offset() const { return offset_; }

 private:
  ThaiShapeSource& source_;
  Observe observe_;
  void* context_;
  uint32_t offset_;
};

// Immutable validated style metadata; an optional lookup table is an exact retained copy.
// CPSHAPE v2 style: 12-byte header, DENSE_COUNT 3-byte entries, base records, mark records.
class ThaiShapeView {
 public:
  static constexpr uint32_t DENSE_COUNT = 46 * 82;
  static constexpr uint32_t ENTRY_BYTES = 3;
  static constexpr uint32_t DENSE_BYTES = DENSE_COUNT * ENTRY_BYTES;
  static constexpr uint32_t HEADER_BYTES = 12;
  static constexpr uint32_t RECORD_BYTES = 8;
  // Entry bits: base ID 0-6, mark count 7-9, first mark record 10-23.
  static constexpr uint32_t MAX_BASES = 127;
  static constexpr uint32_t MAX_MARKS = 5;
  static constexpr uint32_t MAX_RECORDS = 16383;
  static constexpr uint32_t ABSENT_ENTRY = 0xFFFFFF;
  static constexpr uint32_t MAX_STYLE_BYTES = 96 * 1024;
  static constexpr uint32_t MAX_FAMILY_BYTES = 384 * 1024;
  using Coverage = bool (*)(void*, uint32_t);
  // Reads exactly `size` bytes from the stream. A non-null `denseCopy` (DENSE_BYTES)
  // receives the lookup table as it is validated.
  bool validate(ThaiShapeStream& stream, size_t size, Coverage coverage, void* context, uint8_t* denseCopy);
  void setDenseIndex(const uint8_t* index) { denseIndex_ = index; }
  bool valid() const { return source_ != nullptr; }
  bool failed() const { return source_ && source_->failed(); }
  int16_t ascender() const { return valid() ? ascender_ : 0; }
  int16_t descender() const { return valid() ? descender_ : 0; }
  uint16_t lineAdvance() const { return valid() ? lineAdvance_ : 0; }

 private:
  friend class ThaiGlyphCursor;
  struct Entry {
    uint8_t base;
    uint8_t marks;
    uint16_t first;
  };
  bool read(uint32_t offset, uint8_t* output, size_t count) const;
  const uint8_t* bytes() const { return source_->resident_ ? source_->resident_ + offset_ : nullptr; }
  bool recipe(uint32_t key, Entry& entry) const;
  bool stageRecipe(const Entry& entry, uint8_t* records, uint8_t& count) const;
  uint32_t denseOffset(uint32_t key) const { return offset_ + HEADER_BYTES + key * ENTRY_BYTES; }
  uint32_t baseOffset() const { return HEADER_BYTES + DENSE_BYTES; }
  uint32_t markOffset() const { return baseOffset() + bases_ * RECORD_BYTES; }
  ThaiGlyphPlacement baseRecord(uint8_t id) const;
  ThaiGlyphPlacement markRecord(uint16_t index) const;
  ThaiShapeSource* source_ = nullptr;
  const uint8_t* denseIndex_ = nullptr;
  uint32_t offset_ = 0;
  uint32_t size_ = 0;
  uint16_t bases_ = 0;
  uint16_t records_ = 0;
  int16_t ascender_ = 0;
  int16_t descender_ = 0;
  uint16_t lineAdvance_ = 0;
};

class ThaiGlyphCursor {
 public:
  ThaiGlyphCursor() = default;
  ~ThaiGlyphCursor();
  ThaiGlyphCursor(const ThaiGlyphCursor& other);
  ThaiGlyphCursor& operator=(const ThaiGlyphCursor& other);
  ThaiGlyphCursor(ThaiGlyphCursor&& other) noexcept;
  ThaiGlyphCursor& operator=(ThaiGlyphCursor&& other) noexcept;
  bool begin(std::string_view text, const ThaiShapeView& shape);
  bool next(ThaiGlyphPlacement& out);
  size_t consumedBytes() const { return consumed_; }

 private:
  void release();
  void copyState(const ThaiGlyphCursor& other);
  // SD-backed recipe for `key`, memoized in the shared cache. False when the
  // key has no recipe or a companion read failed.
  static bool cachedRecipe(const ThaiShapeView& shape, uint32_t key, uint8_t* records, uint8_t& count);
  // Identify a native scalar or one complete recipe without copying source.
  bool unit(size_t offset, size_t& end, uint32_t& key) const;
  std::string_view text_;
  const ThaiShapeView* shape_ = nullptr;
  ThaiShapeCache* cache_ = nullptr;
  size_t consumed_ = 0;
  size_t offset_ = 0;
  size_t unitEnd_ = 0;
  uint16_t firstMark_ = 0;
  uint8_t base_ = 0;
  uint8_t record_ = 0;
  uint8_t records_ = 0;
  uint8_t stage_ = 0;
  uint8_t stageUnit_ = 0;
  bool first_ = true;
};
