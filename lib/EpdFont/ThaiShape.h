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
  bool read(const ThaiShapeSource& source, uint32_t offset, uint8_t* output, size_t count);
  void touch(uint8_t slot);
  int acquire();
  void retain(uint8_t slot);
  void release(uint8_t slot);
  Block blocks_[BLOCK_COUNT];
  Stage stages_[CLUSTER_SLOTS];
  uint8_t mru_[BLOCK_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7};
  void (*idleCallback_)(ThaiShapeCache*) = nullptr;
};
static_assert(sizeof(ThaiShapeCache) <= 8 * 1024, "Shared shaping working set exceeds 8 KiB");

// Stable owner identity: backing memory/file outlives all borrowing views/cursors.
class ThaiShapeSource {
 public:
  using Read = bool (*)(void* context, uint32_t offset, uint8_t* output, size_t count);
  ThaiShapeSource() = default;
  ~ThaiShapeSource() { reset(); }
  ThaiShapeSource(const ThaiShapeSource&) = delete;
  ThaiShapeSource& operator=(const ThaiShapeSource&) = delete;
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

// Immutable validated style metadata; optional dense bytes are an exact retained copy.
class ThaiShapeView {
 public:
  static constexpr uint32_t DENSE_COUNT = 46 * 82;
  static constexpr uint32_t MAX_STYLE_BYTES = 96 * 1024;
  static constexpr uint32_t MAX_FAMILY_BYTES = 384 * 1024;
  using Coverage = bool (*)(void*, uint32_t);
  bool validate(ThaiShapeSource& source, uint32_t offset, size_t size, Coverage coverage, void* context);
  void setDenseIndex(const uint8_t* index) { denseIndex_ = index; }
  bool valid() const { return source_ != nullptr; }
  bool failed() const { return source_ && source_->failed(); }
  int16_t ascender() const { return valid() ? ascender_ : 0; }
  int16_t descender() const { return valid() ? descender_ : 0; }
  uint16_t lineAdvance() const { return valid() ? lineAdvance_ : 0; }

 private:
  friend class ThaiGlyphCursor;
  bool read(uint32_t offset, uint8_t* output, size_t count) const;
  bool recipe(uint32_t key, uint16_t& base, uint16_t& suffix) const;
  bool stageRecipe(uint16_t base, uint16_t suffix, uint8_t* records, uint8_t& count) const;
  ThaiGlyphPlacement baseRecord(uint16_t id) const;
  ThaiGlyphPlacement suffixRecord(uint16_t id, uint8_t index) const;
  uint8_t suffixCount(uint16_t id) const;
  ThaiShapeSource* source_ = nullptr;
  const uint8_t* bytes_ = nullptr;
  const uint8_t* denseIndex_ = nullptr;
  uint32_t offset_ = 0;
  uint32_t size_ = 0;
  uint32_t baseOffset_ = 0;
  uint32_t suffixOffsets_ = 0;
  uint32_t suffixData_ = 0;
  uint16_t bases_ = 0;
  uint16_t suffixes_ = 0;
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
  // Identify a native scalar or one complete recipe without copying source.
  bool unit(size_t offset, size_t& end, uint32_t& key) const;
  std::string_view text_;
  const ThaiShapeView* shape_ = nullptr;
  ThaiShapeCache* cache_ = nullptr;
  size_t consumed_ = 0;
  size_t offset_ = 0;
  size_t unitEnd_ = 0;
  uint16_t base_ = 0;
  uint16_t suffix_ = 0;
  uint8_t record_ = 0;
  uint8_t records_ = 0;
  uint8_t stage_ = 0;
  uint8_t stageUnit_ = 0;
  bool first_ = true;
};
