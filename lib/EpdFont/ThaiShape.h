#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

struct EpdFontData;

struct ThaiGlyphPlacement {
  uint32_t codepoint;
  uint16_t advanceFP;
  int16_t xOffsetFP;
  int16_t yOffsetFP;
  uint8_t flags;
  // Reordered Sara Am components share their complete original source span.
  size_t sourceBegin = 0;
  size_t sourceEnd = 0;
  enum Flag : uint8_t { Native = 1, ClusterStart = 2, ClusterEnd = 4, RecipeStart = 8, RecipeEnd = 16 };
};

struct ThaiGlyphMetrics {
  uint8_t width;
  uint8_t height;
  uint16_t advanceX;
  int16_t left;
  int16_t top;
};
static_assert(sizeof(ThaiGlyphMetrics) == 8, "Thai metrics must remain compact");

// Borrows a font-owned snapshot; placement never reads SD or allocates memory.
class ThaiShapeView {
 public:
  static constexpr uint8_t ALGORITHM_REVISION = 2;
  static constexpr uint8_t GLYPH_COUNT = 63;
  static int glyphIndex(uint32_t cp);
  void setMetrics(const ThaiGlyphMetrics* metrics, uint64_t coverage) {
    metrics_ = metrics;
    coverage_ = coverage;
  }
  void setNativeFont(const EpdFontData& data) { nativeFont_ = &data; }
  bool hasNativeLigature(uint32_t left, uint32_t right) const;
  bool valid() const { return metrics_ && coverage_; }
  const ThaiGlyphMetrics* glyph(uint32_t cp) const;

 private:
  const ThaiGlyphMetrics* metrics_ = nullptr;
  const EpdFontData* nativeFont_ = nullptr;
  uint64_t coverage_ = 0;
};

class ThaiGlyphCursor {
 public:
  bool begin(std::string_view text, const ThaiShapeView& shape, bool nativeLigatures = true);
  bool next(ThaiGlyphPlacement& out);
  size_t consumedBytes() const { return consumed_; }

 private:
  struct Record {
    uint32_t codepoint;
    uint16_t advance;
    int16_t xOffset;
    int16_t yOffset;
    uint8_t flags;
  };
  bool prepareUnit(size_t offset);
  std::string_view text_;
  const ThaiShapeView* shape_ = nullptr;
  size_t consumed_ = 0;
  size_t offset_ = 0;
  size_t unitEnd_ = 0;
  Record records_[4]{};
  uint8_t count_ = 0;
  uint8_t record_ = 0;
};
static_assert(sizeof(ThaiGlyphCursor) <= 128, "Thai cursor must fit the render stack");
