#pragma once

#include <EpdFontFamily.h>
#include <ThaiLineBreaker.h>

#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "WordStore.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
#ifdef CROSSPOINT_PARSER_TEST
 public:
#endif
  // Word text lives in wordStore (chunked bump arena, NUL-terminated entries);
  // words holds 8-byte handles into it. This replaces the former
  // std::deque<std::string>: per-word string objects, their SSO spills, and
  // every hyphenation/NFC temporary were the layout path's dominant
  // small-allocation churn, and any failed implicit allocation abort()s under
  // -fno-exceptions. Handles stay in a std::deque for the #2814 reason: no
  // large contiguous reallocation at CJK token counts (deque grows in fixed
  // ~512 B nodes). On arena OOM the word is dropped and hadDroppedWords()
  // latches so the section build can fail readably instead of aborting.
  // rubyTexts stays a deque of strings: ruby is rare and per-block small.
  // The per-token parallel arrays below stay vectors: 1 byte / 1 bit each,
  // they never approach the contiguous-block ceiling.
  WordStore wordStore;
  std::deque<WordStore::StoredWord> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   continues=false, noSpace=false: ordinary breakable word gap
  //   continues=false, noSpace=true:  breakable zero-width, stretchable CJK/Korean gap
  //   continues=true,  noSpace=false: unbreakable attachment
  //   continues=true,  noSpace=true:  breakable zero-width, non-stretching attachment
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Allocated only after analyzed Thai arrives. Entries rank the boundary before
  // each token; ordinary non-Thai paragraphs keep the existing flags alone.
  bool hasThaiTokens = false;
  std::vector<thai::BreakKind> wordBreakRanks;
  // Only analyzed, complete-cluster spans may supply styled Thai expansion edges.
  std::vector<bool> wordThaiAnalyzed;
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
  BlockStyle blockStyle;
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphIndentSpaces;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  bool droppedWords = false;
  bool thaiJustificationMetrics = false;
  std::unique_ptr<char[]> thaiPrefixScratch;
  size_t thaiPrefixCapacity = 0;
  bool firstLineConsumed = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  std::string_view wordAt(const size_t i) const { return wordStore.view(words[i]); }
  bool storeWord(std::string_view text, WordStore::StoredWord& out);
  void ensureTokenCapacity(size_t additionalTokens);
  void initializeThaiRanks();
  void setTokenBoundary(size_t index, thai::BreakKind before);
  void appendToken(std::string_view token, EpdFontFamily::Style style, bool continues, bool noSpaceBefore,
                   uint8_t focusBoundary, uint32_t visibleOffset, uint8_t linkId, bool padRuby);
  void addWordImpl(std::string word, EpdFontFamily::Style style, bool underline, bool attachToPrevious,
                   uint32_t visibleOffset, uint8_t linkId, const thai::BreakKind* before);
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
  std::vector<size_t> computeThaiLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                            std::vector<uint16_t>& wordWidths);
  size_t splitThaiGroup(size_t begin, size_t end, int availableWidth, const GfxRenderer& renderer, int fontId,
                        std::vector<uint16_t>& wordWidths);
  bool splitThaiToken(size_t index, size_t offset, thai::BreakKind rank, const GfxRenderer& renderer, int fontId,
                      std::vector<uint16_t>& wordWidths);
  int tokenGap(size_t index, const GfxRenderer& renderer, int fontId) const;
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId, std::span<uint16_t> thaiGapCounts,
                   std::span<uint16_t> thaiBudgets);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
#ifdef THAI_RENDER_PROBE
  static uint8_t probeThaiSpaceWeight;
#endif
  explicit ParsedText(const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle(), const uint8_t paragraphIndentSpaces = 2)
      : blockStyle(blockStyle),
        paragraphIndentSpaces(paragraphIndentSpaces),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0);
  void addWordWithBoundary(std::string word, EpdFontFamily::Style style, bool underline, thai::BreakKind before,
                           uint32_t visibleOffset, uint8_t linkId);
  void addAnalyzedToken(std::string_view token, EpdFontFamily::Style style, thai::BreakKind before,
                        uint32_t visibleOffset, uint8_t linkId);
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // True once any word was dropped because the text arena could not allocate.
  // Callers must treat the block as incomplete and fail the section build.
  bool hadDroppedWords() const { return droppedWords; }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true, int8_t characterSpacing = 0,
                             uint8_t wordSpacingPercent = 100);
};
