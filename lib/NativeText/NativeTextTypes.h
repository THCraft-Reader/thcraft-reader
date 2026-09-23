#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "NativeAllocator.h"

enum class TextStatus : uint8_t { Ok, OutOfMemory, InvalidFont, InvalidText, StorageError, CapacityExceeded };

// Trivial typed storage charged to the shared native allocation budget.
// Callers retain capacity between operations; growth never throws.
template <class T>
class NativeBuffer {
  static_assert(std::is_trivially_copyable_v<T>);
  T* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;

 public:
  NativeBuffer() = default;
  ~NativeBuffer() { native_text_free(data_); }
  NativeBuffer(const NativeBuffer&) = delete;
  NativeBuffer& operator=(const NativeBuffer&) = delete;
  NativeBuffer(NativeBuffer&& other) noexcept { swap(other); }
  NativeBuffer& operator=(NativeBuffer&& other) noexcept {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }
  void swap(NativeBuffer& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
  }
  bool reserve(size_t count) {
    if (count <= capacity_) return true;
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) return false;
    void* memory = native_text_realloc(data_, count * sizeof(T));
    if (!memory) return false;
    data_ = static_cast<T*>(memory);
    capacity_ = count;
    return true;
  }
  bool resize(size_t count) {
    if (!reserve(count)) return false;
    size_ = count;
    return true;
  }
  bool assign(std::span<const T> values) {
    if (!resize(values.size())) return false;
    if (!values.empty()) std::memmove(data_, values.data(), values.size_bytes());
    return true;
  }
  void clear() { size_ = 0; }
  void reset() {
    native_text_free(data_);
    data_ = nullptr;
    size_ = capacity_ = 0;
  }
  T* data() { return data_; }
  const T* data() const { return data_; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }
  T& operator[](size_t index) { return data_[index]; }
  const T& operator[](size_t index) const { return data_[index]; }
  std::span<T> span() { return {data_, size_}; }
  std::span<const T> span() const { return {data_, size_}; }
};

struct NativeStyleSpan {
  uint32_t startByte = 0, endByte = 0;
  uint8_t style = 0, bidiLevel = 0;
};
struct NativeGap {
  uint32_t byteOffset = 0;
  int32_t extraAdvance26 = 0;
};
struct NativeLineInput {
  std::string_view text;
  int fontId = 0;
  std::span<const NativeStyleSpan> spans;
  int8_t paragraphLevel = -1;
  uint32_t syntheticSuffixCp = 0;
  std::span<const NativeGap> gaps;
  bool readerFeatures = true;
  bool resolvedLevels = false;
  int8_t characterSpacing = 0;       // Pixels between visible non-space shaping clusters, never within marks.
  uint8_t wordSpacingPercent = 100;  // Scales Unicode space advances before justification/ruby gaps.
};
struct NativeGlyph {
  uint64_t faceIdentity = 0;
  uint32_t glyphId = 0;
  uint32_t startByte = 0, endByte = 0;
  int32_t x26 = 0, y26 = 0, advanceX26 = 0, advanceY26 = 0;
  uint32_t rasterSize26 = 0;
  uint8_t syntheticStyle = 0, style = 0;
};
struct NativeCluster {
  uint32_t startByte = 0, endByte = 0;
  int32_t x26 = 0, advance26 = 0, top26 = 0, bottom26 = 0;
  uint8_t bidiLevel = 0, style = 0;
  bool unsafeToBreak = false;
};
struct NativeBounds {
  int32_t left26 = 0, top26 = 0, right26 = 0, bottom26 = 0;
};
struct NativeGlyphRun {
  NativeBuffer<NativeGlyph> glyphs;
  NativeBuffer<NativeCluster> clusters;
  int32_t advance26 = 0, ascender26 = 0, descender26 = 0;
  NativeBounds ink;
  int8_t paragraphLevel = 0;
  void clear() {
    glyphs.clear();
    clusters.clear();
    advance26 = ascender26 = descender26 = 0;
    ink = {};
    paragraphLevel = 0;
  }
};
struct NativeBitmapView {
  const uint8_t* data = nullptr;
  int width = 0, height = 0, pitch = 0, left = 0, top = 0;
};
struct NativeWord {
  uint32_t startByte = 0, endByte = 0, selectionTextOffset = 0;
  int32_t x26 = 0, width26 = 0;
  int16_t top = 0, height = 0;
  uint8_t style = 0;
};
struct NativeRuby {
  uint32_t baseStartByte = 0, baseEndByte = 0, textOffset = 0, textBytes = 0;
  int32_t x26 = 0, y26 = 0;
  uint8_t style = 0;
};
struct NativeLineData {
  NativeBuffer<char> text;
  NativeBuffer<NativeStyleSpan> spans;
  NativeBuffer<NativeWord> words;
  NativeBuffer<NativeGap> gaps;
  NativeBuffer<NativeRuby> ruby;
  NativeBuffer<char> rubyText;
  NativeBuffer<char> selectionText;
  int8_t paragraphLevel = 0;
  int16_t lineHeight = 0, baseline = 0, rubyLift = 0;
  int32_t alignmentX26 = 0;
  uint32_t syntheticSuffixCp = 0;
  uint16_t overflowClipWidth = 0;  // Nonzero only for a forced oversized cluster/group.
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  std::string_view logicalText() const { return {text.data(), text.size()}; }
  bool empty() const { return text.empty() && ruby.empty(); }
  NativeLineInput input(int fontId) const {
    return {logicalText(), fontId, spans.span(), paragraphLevel,   syntheticSuffixCp,
            gaps.span(),   true,   true,         characterSpacing, wordSpacingPercent};
  }
};
struct NativeVariation {
  uint32_t tag = 0;
  float value = 0;
};
struct NativeFontFile {
  std::string_view path;
  uint8_t style = 0;
  std::span<const NativeVariation> axes;
};
