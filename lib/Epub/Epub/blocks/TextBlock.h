#pragma once
#include <BoundedFileReader.h>
#include <EpdFontFamily.h>
#include <HalStorage.h>
#ifdef CROSSPOINT_NATIVE_TEXT
#include <NativeTextTypes.h>

#include <new>
#endif

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"
#include "Epub/FootnoteEntry.h"

struct NativeLineData;

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// six parallel vectors: a resident page holds ~25-30 of these blocks, and the
// vector-of-string layout cost ~250 throwing allocations per page load, which
// was the primary driver of heap fragmentation on the ESP32-C3.
//
// Arena layout, in order (2-byte alignment holds by construction: all 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
//   int16_t  xpos[wordCount]
//   uint16_t focusSuffixX[wordCount]   present only when focusPresent
//   uint8_t  styles[wordCount]
//   uint8_t  focusBoundary[wordCount]  present only when focusPresent
//   char     text[textBytes]           all words back to back, NUL-terminated
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to C APIs (drawText) with no std::string materialization.
//
// Focus split semantics (unchanged from the vector layout): boundary N > 0
// means the first N bytes of word i render bold, the remainder in the base
// style. N is bounded to 9 codepoints (<= 36 UTF-8 bytes) by the clamp in
// ParsedText::addWord. focusSuffixX is the pre-computed pixel offset from the
// word start to the regular suffix. Both arrays are omitted from the arena
// entirely when no word on the line has a split (zero per-word RAM cost when
// focus reading is disabled).
class TextBlock final : public Block {
 public:
  struct LinkSpan {
    char href[FOOTNOTE_HREF_LEN];
    int16_t x;
    int16_t width;
    int16_t topLift;
#ifdef CROSSPOINT_NATIVE_TEXT
    int16_t top = 0;
    int16_t height = 0;
#endif
  };

 private:
  BlockStyle blockStyle;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // total size of the text region, including NULs
  // getRubyTexts() presents either backing store without copying annotations.
  bool focusPresent = false;
  bool isValid = true;
#ifdef CROSSPOINT_NATIVE_TEXT
  NativeLineData nativeData;
  bool nativePresent = false;
  // Layout-only source range; page source LUTs and links persist their own positions.
  uint32_t nativeSourceStart = 0;
  uint32_t nativeSourceEnd = 0;
#endif
  // The ONLY allocation: makeUniqueNoThrow, so OOM yields an invalid block
  // instead of abort() (bare new is not nothrow with -fno-exceptions).
  std::unique_ptr<uint8_t[]> arena;
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint16_t* focusSuffixXArr = nullptr;  // null when !focusPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* focusBoundaryArr = nullptr;  // null when !focusPresent
  const char* textArr = nullptr;
  std::vector<std::string> rubyTexts;
  // Layout-only metadata. ChapterHtmlSlimParser moves it into Page::links
  // immediately; cached TextBlocks therefore keep the same compact format.
#ifdef CROSSPOINT_NATIVE_TEXT
  NativeBuffer<LinkSpan> linkSpans;
#else
  std::vector<LinkSpan> linkSpans;
#endif

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasFocus, uint16_t textBytes);
  void bindArenaPointers();

 public:
  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. On arena OOM the block is empty and valid()
  // is false -- callers must check and fail the line instead of using it.
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<std::string> rubyTexts = {}, std::vector<LinkSpan> linkSpans = {});
#ifdef CROSSPOINT_NATIVE_TEXT
  static void* operator new(size_t bytes, const std::nothrow_t&) noexcept;
  static void operator delete(void* pointer) noexcept;
  static void operator delete(void* pointer, const std::nothrow_t&) noexcept;
  explicit TextBlock(NativeLineData&& line, const BlockStyle& blockStyle, NativeBuffer<LinkSpan> linkSpans = {});
  std::span<const NativeRuby> getNativeRuby() const;
  std::string_view nativeRubyText(size_t index) const;
  void setSourceRange(uint32_t start, uint32_t end) {
    nativeSourceStart = start;
    nativeSourceEnd = end;
  }
  uint32_t sourceStartOffset() const { return nativeSourceStart; }
  uint32_t sourceEndOffset() const { return nativeSourceEnd; }
#endif
  const NativeLineData* nativeLine() const;
  bool warmNativeText(const GfxRenderer& renderer, int fontId) const;
  int layoutHeight(const GfxRenderer& renderer, int fontId, float compression) const;
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override;
  class RubyTextView {
    const TextBlock* block;

   public:
    explicit RubyTextView(const TextBlock* block) : block(block) {}
    size_t size() const;
    bool empty() const { return size() == 0; }
    std::string_view operator[](size_t index) const;
    std::string_view back() const { return (*this)[size() - 1]; }
  };
  bool valid() const { return isValid; }
  uint16_t wordCount() const;
  // Selection strings are NUL-terminated in either representation.
  const char* wordText(uint16_t i) const;
  uint16_t wordTextLen(uint16_t i) const;
  int16_t wordXpos(uint16_t i) const;
  EpdFontFamily::Style wordStyle(uint16_t i) const;
  // Native focus is already represented by cluster-safe style spans.
  uint8_t focusBoundary(uint16_t i) const;
  uint16_t focusSuffixX(uint16_t i) const;
  int wordWidth(uint16_t i) const;
  int wordTop(uint16_t i) const;
  int wordHeight(uint16_t i) const;
  bool hasRuby() const;
  int getRubyShift(int ascender) const;
  RubyTextView getRubyTexts() const { return RubyTextView(this); }
#ifdef CROSSPOINT_NATIVE_TEXT
  NativeBuffer<LinkSpan> takeLinkSpans() { return std::move(linkSpans); }
#else
  std::vector<LinkSpan> takeLinkSpans() { return std::move(linkSpans); }
#endif

  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(serialization::BoundedFileReader& file);
};
