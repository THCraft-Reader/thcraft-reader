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

// Immutable, fully validated style payload. The owner retains the backing bytes.
class ThaiShapeView {
 public:
  static constexpr uint32_t DENSE_COUNT = 46 * 82;
  static constexpr uint32_t MAX_STYLE_BYTES = 96 * 1024;
  static constexpr uint32_t MAX_FAMILY_BYTES = 384 * 1024;
  using Coverage = bool (*)(void*, uint32_t);
  bool validate(const uint8_t* bytes, size_t size, Coverage coverage, void* context);
  bool valid() const { return bytes_ != nullptr; }
  int16_t ascender() const;
  int16_t descender() const;
  uint16_t lineAdvance() const;

 private:
  friend class ThaiGlyphCursor;
  const uint8_t* bytes_ = nullptr;
  bool recipe(uint32_t key, uint16_t& base, uint16_t& suffix) const;
  ThaiGlyphPlacement baseRecord(uint16_t id) const;
  ThaiGlyphPlacement suffixRecord(uint16_t id, uint8_t index) const;
  uint8_t suffixCount(uint16_t id) const;
};

class ThaiGlyphCursor {
 public:
  bool begin(std::string_view text, const ThaiShapeView& shape);
  bool next(ThaiGlyphPlacement& out);
  size_t consumedBytes() const { return consumed_; }

 private:
  // Identify a native scalar or one complete recipe without allocating/copying source.
  bool unit(size_t offset, size_t& end, uint32_t& key) const;
  std::string_view text_;
  const ThaiShapeView* shape_ = nullptr;
  size_t consumed_ = 0;
  size_t offset_ = 0;
  size_t unitEnd_ = 0;
  uint16_t base_ = 0;
  uint16_t suffix_ = 0;
  uint8_t record_ = 0;
  uint8_t records_ = 0;
  bool first_ = true;
};
