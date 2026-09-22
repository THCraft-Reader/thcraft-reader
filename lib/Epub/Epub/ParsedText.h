#pragma once

#include <EpdFontFamily.h>

#ifdef CROSSPOINT_NATIVE_TEXT
#include <NativeParagraphLayout.h>
#endif
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
#ifndef CROSSPOINT_NATIVE_TEXT
  // words/rubyTexts are std::deque, not std::vector: a paragraph can hold thousands
  // of tokens (CJK splits every character), and a vector grows by reallocating its
  // whole element array into one contiguous block (32 B/std::string -> 64-128 KB at
  // a few thousand tokens). On the ESP32-C3 that single large contiguous request
  // fails under a fragmented, BLE-resident heap and the throwing operator new
  // abort()s the firmware (fresh-open CJK crash). A deque grows in fixed ~512 B nodes
  // (largest contiguous alloc stays ~2 KB regardless of token count), so it never
  // triggers that. The per-token parallel arrays below stay vectors: 1 byte / 1 bit
  // each, they never approach the contiguous-block ceiling.
  std::deque<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   continues=false, noSpace=false: ordinary breakable word gap
  //   continues=false, noSpace=true:  breakable zero-width, stretchable CJK/Korean gap
  //   continues=true,  noSpace=false: unbreakable attachment
  //   continues=true,  noSpace=true:  breakable zero-width, non-stretching attachment
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Focus Reading emphasis: bytes [0, wordFocusBoundary) render bold, the rest at wordStyles.
  // 0 = none. An annotation rather than a token split, so the hyphenator and line breaker still
  // see whole words; TextBlock stores emphasis the same way, so extractLine passes it through.
  std::vector<uint8_t> wordFocusBoundary;
  // Internal-link identity through tokenization, hyphenation and BiDi reorder.
  // Zero means plain text; non-zero indexes linkTargets. Kept at one byte per
  // token and discarded after layout, never added to the page-cache TextBlock.
  std::vector<uint8_t> wordLinkIds;
  std::vector<std::string> linkTargets;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks do not carry any of this metadata.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  std::deque<std::string> rubyTexts;
#endif
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
#ifdef CROSSPOINT_NATIVE_TEXT
  // Fragment handles remain stable until a layout consumes a prefix. They are
  // parser ingestion boundaries, not dictionary words or shaping clusters.
  struct NativeFragment {
    uint32_t startByte = 0, endByte = 0;
    uint8_t style = 0, linkId = 0;
  };
  struct NativeRubyRecord {
    uint32_t startByte = 0, endByte = 0, textOffset = 0, textBytes = 0;
    uint8_t style = 0;
  };
  struct NativeLinkTarget {
    char href[FOOTNOTE_HREF_LEN];
  };
  NativeBuffer<char> nativeText;
  NativeBuffer<NativeFragment> nativeFragments;
  NativeBuffer<NativeSourceAnchor> nativeAnchors;
  NativeBuffer<NativeRubyRecord> nativeRuby;
  NativeBuffer<char> nativeRubyText;
  NativeBuffer<NativeLinkTarget> nativeLinkTargets;
  size_t nativeScalars = 0;
  TextStatus nativeStatus = TextStatus::Ok;
  int8_t nativeParagraphLevel = -1;
  bool nativeFirstLine = true;
  uint8_t nativeLastLinkId = 0;
  void appendNative(std::string_view text, EpdFontFamily::Style style, bool underline, bool attachToPrevious,
                    uint32_t sourceOffset, uint8_t linkId, bool synthetic);
  void failNative(TextStatus status);
  void discardNativeInput();
  void consumeNativePrefix(size_t bytes);
  bool layoutNative(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                    const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine, bool includeLastLine,
                    bool semanticBoundary);
#else
  bool isNaturalAlign;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  bool extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);
#endif

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled)
#ifndef CROSSPOINT_NATIVE_TEXT
        ,
        isNaturalAlign(false),
        hasRtlWord(false)
#endif
  {
  }
  ~ParsedText() = default;

#ifdef CROSSPOINT_NATIVE_TEXT
  void addWord(std::string_view word, EpdFontFamily::Style fontStyle, bool underline = false,
               bool attachToPrevious = false, uint32_t visibleTextOffset = 0, uint8_t linkId = 0);
  void addSyntheticText(std::string_view text, EpdFontFamily::Style style, uint32_t anchorOffset,
                        bool attachToPrevious = false, uint8_t linkId = 0);
#else
  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0);
#endif
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
#ifdef CROSSPOINT_NATIVE_TEXT
  void setRubyForWordAt(size_t index, std::string_view ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, std::string_view ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const;
  std::string_view getRubyTextAt(size_t index) const;
  bool nativeNeedsLayout() const { return nativeScalars >= 4096 || nativeText.size() >= 16384; }
  size_t nativePendingScalars() const { return nativeScalars; }
  TextStatus lastTextStatus() const { return nativeStatus; }
  bool layoutBeforeRuby(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                        const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine);
#else
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
#endif
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
#ifdef CROSSPOINT_NATIVE_TEXT
  size_t size() const { return nativeFragments.size(); }
  bool isEmpty() const { return nativeText.empty(); }
#else
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
#endif
  // On false, discard this consumed input and any partial output; do not retry it.
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true);
};
