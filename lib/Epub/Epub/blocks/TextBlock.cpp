#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstring>
#ifdef CROSSPOINT_NATIVE_TEXT
#include <NativeUtf8.h>
#endif

#include "../../../../src/fontIds.h"

namespace {
template <typename T>
bool readField(serialization::BoundedFileReader& file, T& value) {
  return file.read(&value, sizeof(value)) == sizeof(value);
}
template <typename T>
bool writeField(HalFile& file, const T& value) {
  return file.write(&value, sizeof(value)) == sizeof(value);
}
bool availableBytes(serialization::BoundedFileReader& file, size_t bytes) {
  const size_t position = file.position();
  const size_t size = file.size();
  return position <= size && bytes <= size - position;
}
bool readFlag(serialization::BoundedFileReader& file, bool& flag) {
  uint8_t value = 0;
  if (!readField(file, value) || value > 1) return false;
  flag = value != 0;
  return true;
}
bool writeStyle(HalFile& file, const BlockStyle& s) {
  return writeField(file, s.alignment) && writeField(file, static_cast<uint8_t>(s.textAlignDefined)) &&
         writeField(file, s.marginTop) && writeField(file, s.marginBottom) && writeField(file, s.marginLeft) &&
         writeField(file, s.marginRight) && writeField(file, s.paddingTop) && writeField(file, s.paddingBottom) &&
         writeField(file, s.paddingLeft) && writeField(file, s.paddingRight) && writeField(file, s.textIndent) &&
         writeField(file, static_cast<uint8_t>(s.textIndentDefined)) &&
         writeField(file, static_cast<uint8_t>(s.isRtl)) && writeField(file, static_cast<uint8_t>(s.directionDefined));
}
bool readStyle(serialization::BoundedFileReader& file, BlockStyle& s) {
  uint8_t alignment = 0;
  if (!readField(file, alignment) || alignment > static_cast<uint8_t>(CssTextAlign::None)) return false;
  s.alignment = static_cast<CssTextAlign>(alignment);
  return readFlag(file, s.textAlignDefined) && readField(file, s.marginTop) && readField(file, s.marginBottom) &&
         readField(file, s.marginLeft) && readField(file, s.marginRight) && readField(file, s.paddingTop) &&
         readField(file, s.paddingBottom) && readField(file, s.paddingLeft) && readField(file, s.paddingRight) &&
         readField(file, s.textIndent) && readFlag(file, s.textIndentDefined) && readFlag(file, s.isRtl) &&
         readFlag(file, s.directionDefined);
}

#ifdef CROSSPOINT_NATIVE_TEXT
constexpr size_t MAX_NATIVE_TEXT_BYTES = 16384;
constexpr size_t MAX_NATIVE_SCALARS = 4096;
constexpr int32_t MAX_POSITION26 = INT16_MAX * 64;
bool validUtf8(std::string_view text) {
  size_t offset = 0, count = 0;
  uint32_t cp = 0;
  while (offset < text.size()) {
    if (!native_text::nextUtf8(text, offset, cp) || cp == 0 || ++count > MAX_NATIVE_SCALARS) return false;
  }
  return true;
}
bool validRange(std::string_view text, uint32_t start, uint32_t end) {
  return start < end && native_text::utf8Boundary(text, start) && native_text::utf8Boundary(text, end);
}
bool validPosition(int64_t position) { return position >= -MAX_POSITION26 && position <= MAX_POSITION26; }
bool validWord(const NativeLineData& line, const NativeWord& word) {
  const int64_t left = static_cast<int64_t>(line.alignmentX26) + word.x26;
  const int64_t right = left + word.width26;
  return validRange(line.logicalText(), word.startByte, word.endByte) && word.width26 >= 0 && word.top >= 0 &&
         word.height >= 0 && word.top + word.height <= line.lineHeight && validPosition(left) && validPosition(right) &&
         (!line.overflowClipWidth || (left >= 0 && right <= line.overflowClipWidth * 64));
}
bool validNativeHeader(const NativeLineData& line) {
  return line.text.size() <= MAX_NATIVE_TEXT_BYTES && line.spans.size() <= MAX_NATIVE_SCALARS &&
         line.words.size() <= MAX_NATIVE_SCALARS && line.gaps.size() <= MAX_NATIVE_SCALARS &&
         line.ruby.size() <= MAX_NATIVE_SCALARS && line.rubyText.size() <= MAX_NATIVE_TEXT_BYTES &&
         line.paragraphLevel >= 0 && line.paragraphLevel <= 1 && line.lineHeight >= 0 && line.baseline >= 0 &&
         line.baseline <= line.lineHeight && line.rubyLift >= 0 && line.rubyLift <= line.lineHeight &&
         validPosition(line.alignmentX26) && (line.syntheticSuffixCp == 0 || line.syntheticSuffixCp == 0x2d) &&
         line.overflowClipWidth <= INT16_MAX;
}
bool validateNative(const NativeLineData& line) {
  if (!validNativeHeader(line) || !validUtf8(line.logicalText())) return false;
  uint32_t previous = 0;
  for (const auto& span : line.spans.span()) {
    if (!validRange(line.logicalText(), span.startByte, span.endByte) || span.startByte < previous ||
        span.bidiLevel > 125)
      return false;
    previous = span.endByte;
  }
  previous = 0;
  for (const auto& word : line.words.span()) {
    if (!validWord(line, word) || word.startByte < previous) return false;
    previous = word.endByte;
  }
  previous = 0;
  bool first = true;
  for (const auto& gap : line.gaps.span()) {
    if (gap.byteOffset == 0 || !native_text::utf8Boundary(line.logicalText(), gap.byteOffset) ||
        (!first && gap.byteOffset <= previous) || gap.extraAdvance26 < 0 || !validPosition(gap.extraAdvance26))
      return false;
    previous = gap.byteOffset;
    first = false;
  }
  previous = 0;
  for (const auto& ruby : line.ruby.span()) {
    if (!validRange(line.logicalText(), ruby.baseStartByte, ruby.baseEndByte) || ruby.baseStartByte < previous ||
        ruby.textBytes == 0 || ruby.textOffset > line.rubyText.size() ||
        ruby.textBytes > line.rubyText.size() - ruby.textOffset || !validPosition(ruby.x26) || ruby.y26 < 0 ||
        ruby.y26 > line.lineHeight * 64 || !validUtf8({line.rubyText.data() + ruby.textOffset, ruby.textBytes}))
      return false;
    previous = ruby.baseEndByte;
  }
  return true;
}
bool prepareSelectionText(NativeLineData& line) {
  size_t bytes = 0;
  for (const auto& word : line.words.span()) bytes += word.endByte - word.startByte + 1;
  if (bytes > MAX_NATIVE_TEXT_BYTES + MAX_NATIVE_SCALARS || !line.selectionText.resize(bytes)) return false;
  size_t offset = 0, spanIndex = 0;
  for (auto& word : line.words.span()) {
    word.selectionTextOffset = static_cast<uint32_t>(offset);
    const size_t length = word.endByte - word.startByte;
    memcpy(line.selectionText.data() + offset, line.text.data() + word.startByte, length);
    offset += length;
    line.selectionText[offset++] = '\0';
    while (spanIndex < line.spans.size() && line.spans[spanIndex].endByte <= word.startByte) ++spanIndex;
    word.style = spanIndex < line.spans.size() && line.spans[spanIndex].startByte <= word.startByte
                     ? line.spans[spanIndex].style
                     : 0;
  }
  return true;
}
int floorPixel(int64_t position26) {
  return static_cast<int>(position26 >= 0 ? position26 / 64 : -((-position26 + 63) / 64));
}
int ceilPixel(int64_t position26) { return -floorPixel(-position26); }

bool writeNative(HalFile& file, const NativeLineData& line) {
  if (!validateNative(line)) return false;
  if (!writeField(file, static_cast<uint16_t>(line.text.size())) ||
      !writeField(file, static_cast<uint16_t>(line.spans.size())) ||
      !writeField(file, static_cast<uint16_t>(line.words.size())) ||
      !writeField(file, static_cast<uint16_t>(line.gaps.size())) ||
      !writeField(file, static_cast<uint16_t>(line.ruby.size())) || !writeField(file, line.paragraphLevel) ||
      !writeField(file, line.lineHeight) || !writeField(file, line.baseline) || !writeField(file, line.rubyLift) ||
      !writeField(file, line.alignmentX26) || !writeField(file, line.syntheticSuffixCp) ||
      !writeField(file, line.overflowClipWidth) ||
      (!line.text.empty() && file.write(line.text.data(), line.text.size()) != line.text.size()))
    return false;
  for (const auto& span : line.spans.span()) {
    if (!writeField(file, static_cast<uint16_t>(span.startByte)) ||
        !writeField(file, static_cast<uint16_t>(span.endByte)) || !writeField(file, span.style) ||
        !writeField(file, span.bidiLevel))
      return false;
  }
  for (const auto& word : line.words.span()) {
    if (!writeField(file, static_cast<uint16_t>(word.startByte)) ||
        !writeField(file, static_cast<uint16_t>(word.endByte)) || !writeField(file, word.x26) ||
        !writeField(file, word.width26) || !writeField(file, word.top) || !writeField(file, word.height))
      return false;
  }
  for (const auto& gap : line.gaps.span()) {
    if (!writeField(file, static_cast<uint16_t>(gap.byteOffset)) || !writeField(file, gap.extraAdvance26)) return false;
  }
  for (const auto& ruby : line.ruby.span()) {
    if (!writeField(file, static_cast<uint16_t>(ruby.baseStartByte)) ||
        !writeField(file, static_cast<uint16_t>(ruby.baseEndByte)) ||
        !writeField(file, static_cast<uint16_t>(ruby.textBytes)) || !writeField(file, ruby.x26) ||
        !writeField(file, ruby.y26) || !writeField(file, ruby.style) ||
        file.write(line.rubyText.data() + ruby.textOffset, ruby.textBytes) != ruby.textBytes)
      return false;
  }
  return true;
}

// Preflight variable records before allocating their arrays. Only the bounded logical
// text is resident during this pass; Ruby UTF-8 is checked one scalar at a time.
bool readRubyText(serialization::BoundedFileReader& file, size_t bytes, char* destination) {
  size_t offset = 0, scalars = 0;
  while (offset < bytes) {
    char scalar[4];
    if (file.read(scalar, 1) != 1) return false;
    const uint8_t lead = static_cast<uint8_t>(scalar[0]);
    const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (length > bytes - offset || (length > 1 && file.read(scalar + 1, length - 1) != length - 1)) return false;
    size_t cursor = 0;
    uint32_t cp = 0;
    if (!native_text::nextUtf8({scalar, length}, cursor, cp) || cursor != length || cp == 0 ||
        ++scalars > MAX_NATIVE_SCALARS)
      return false;
    if (destination) memcpy(destination + offset, scalar, length);
    offset += length;
  }
  return true;
}
bool readNative(serialization::BoundedFileReader& file, NativeLineData& line, BlockStyle& style) {
  uint16_t textBytes = 0, spanCount = 0, wordCount = 0, gapCount = 0, rubyCount = 0;
  if (!readField(file, textBytes) || !readField(file, spanCount) || !readField(file, wordCount) ||
      !readField(file, gapCount) || !readField(file, rubyCount) || !readField(file, line.paragraphLevel) ||
      !readField(file, line.lineHeight) || !readField(file, line.baseline) || !readField(file, line.rubyLift) ||
      !readField(file, line.alignmentX26) || !readField(file, line.syntheticSuffixCp) ||
      !readField(file, line.overflowClipWidth) || textBytes > MAX_NATIVE_TEXT_BYTES || spanCount > MAX_NATIVE_SCALARS ||
      wordCount > MAX_NATIVE_SCALARS || gapCount > MAX_NATIVE_SCALARS || rubyCount > MAX_NATIVE_SCALARS ||
      !validNativeHeader(line))
    return false;
  const size_t minimumBytes =
      textBytes + size_t(spanCount) * 6 + size_t(wordCount) * 16 + size_t(gapCount) * 6 + size_t(rubyCount) * 15 + 23;
  if (!availableBytes(file, minimumBytes)) return false;
  if (!line.text.resize(textBytes)) {
    file.outOfMemory();
    return false;
  }
  if ((textBytes && file.read(line.text.data(), textBytes) != textBytes) || !validUtf8(line.logicalText()))
    return false;
  const size_t recordsStart = file.position();
  size_t rubyBytes = 0;
  for (int pass = 0; pass != 2; ++pass) {
    if (pass == 1) {
      if (!line.spans.resize(spanCount) || !line.words.resize(wordCount) || !line.gaps.resize(gapCount) ||
          !line.ruby.resize(rubyCount) || !line.rubyText.resize(rubyBytes)) {
        file.outOfMemory();
        return false;
      }
      if (!file.seek(recordsStart)) return false;
    }
    uint32_t previous = 0;
    for (size_t i = 0; i < spanCount; ++i) {
      uint16_t start = 0, end = 0;
      NativeStyleSpan span;
      if (!readField(file, start) || !readField(file, end) || !readField(file, span.style) ||
          !readField(file, span.bidiLevel) || !validRange(line.logicalText(), start, end) || start < previous ||
          span.bidiLevel > 125)
        return false;
      span.startByte = start;
      span.endByte = end;
      previous = end;
      if (pass) line.spans[i] = span;
    }
    previous = 0;
    for (size_t i = 0; i < wordCount; ++i) {
      uint16_t start = 0, end = 0;
      NativeWord word;
      if (!readField(file, start) || !readField(file, end) || !readField(file, word.x26) ||
          !readField(file, word.width26) || !readField(file, word.top) || !readField(file, word.height))
        return false;
      word.startByte = start;
      word.endByte = end;
      if (start < previous || !validWord(line, word)) return false;
      previous = end;
      if (pass) line.words[i] = word;
    }
    previous = 0;
    for (size_t i = 0; i < gapCount; ++i) {
      uint16_t offset = 0;
      NativeGap gap;
      if (!readField(file, offset) || !readField(file, gap.extraAdvance26) || offset == 0 ||
          !native_text::utf8Boundary(line.logicalText(), offset) || (i && offset <= previous) ||
          gap.extraAdvance26 < 0 || !validPosition(gap.extraAdvance26))
        return false;
      gap.byteOffset = previous = offset;
      if (pass) line.gaps[i] = gap;
    }
    previous = 0;
    size_t rubyOffset = 0;
    for (size_t i = 0; i < rubyCount; ++i) {
      uint16_t start = 0, end = 0, bytes = 0;
      NativeRuby ruby;
      if (!readField(file, start) || !readField(file, end) || !readField(file, bytes) || !readField(file, ruby.x26) ||
          !readField(file, ruby.y26) || !readField(file, ruby.style) || !validRange(line.logicalText(), start, end) ||
          start < previous || bytes == 0 || bytes > MAX_NATIVE_TEXT_BYTES - rubyOffset || !validPosition(ruby.x26) ||
          ruby.y26 < 0 || ruby.y26 > line.lineHeight * 64 ||
          (pass && (rubyOffset > line.rubyText.size() || bytes > line.rubyText.size() - rubyOffset)) ||
          !readRubyText(file, bytes, pass ? line.rubyText.data() + rubyOffset : nullptr))
        return false;
      ruby.baseStartByte = start;
      ruby.baseEndByte = previous = end;
      ruby.textOffset = static_cast<uint32_t>(rubyOffset);
      ruby.textBytes = bytes;
      rubyOffset += bytes;
      if (pass) line.ruby[i] = ruby;
    }
    if (!readStyle(file, style)) return false;
    rubyBytes = rubyOffset;
  }
  if (!prepareSelectionText(line)) {
    file.outOfMemory();
    return false;
  }
  return true;
}
#endif
}  // namespace

#ifdef CROSSPOINT_NATIVE_TEXT
void* TextBlock::operator new(size_t bytes, const std::nothrow_t&) noexcept { return native_text_malloc(bytes); }
void TextBlock::operator delete(void* pointer) noexcept { native_text_free(pointer); }
void TextBlock::operator delete(void* pointer, const std::nothrow_t&) noexcept { native_text_free(pointer); }
TextBlock::TextBlock(NativeLineData&& line, const BlockStyle& style, NativeBuffer<LinkSpan> links)
    : blockStyle(style), nativeData(std::move(line)), nativePresent(true), linkSpans(std::move(links)) {
  isValid = validateNative(nativeData) && prepareSelectionText(nativeData);
  if (!isValid) LOG_ERR("TEXT", "Invalid native line or selection allocation failure");
}
std::span<const NativeRuby> TextBlock::getNativeRuby() const {
  return nativePresent && isValid ? nativeData.ruby.span() : std::span<const NativeRuby>{};
}
std::string_view TextBlock::nativeRubyText(size_t index) const {
  if (!nativePresent || !isValid || index >= nativeData.ruby.size()) return {};
  const auto& ruby = nativeData.ruby[index];
  return {nativeData.rubyText.data() + ruby.textOffset, ruby.textBytes};
}
#endif
const NativeLineData* TextBlock::nativeLine() const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return &nativeData;
#endif
  return nullptr;
}
bool TextBlock::warmNativeText(const GfxRenderer& renderer, int fontId) const {
  if (!isValid) return false;
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return renderer.warmNativeLine(fontId, nativeData);
#endif
  return true;
}
int TextBlock::layoutHeight(const GfxRenderer& renderer, int fontId, float compression) const {
  const int nominal = renderer.getLineHeight(fontId, compression);
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return std::max(nominal, static_cast<int>(nativeData.lineHeight));
#endif
  return nominal + getRubyShift(renderer.getFontAscenderSize(fontId));
}
bool TextBlock::isEmpty() {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return nativeData.empty();
#endif
  return numWords == 0;
}
uint16_t TextBlock::wordCount() const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return isValid ? static_cast<uint16_t>(nativeData.words.size()) : 0;
#endif
  return numWords;
}
const char* TextBlock::wordText(uint16_t index) const {
  if (index >= wordCount()) return "";
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return nativeData.selectionText.data() + nativeData.words[index].selectionTextOffset;
#endif
  return textArr + textOffArr[index];
}
uint16_t TextBlock::wordTextLen(uint16_t index) const {
  if (index >= wordCount()) return 0;
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return static_cast<uint16_t>(nativeData.words[index].endByte - nativeData.words[index].startByte);
#endif
  const uint16_t end = index + 1 < numWords ? textOffArr[index + 1] : textBytes;
  return end - textOffArr[index] - 1;
}
int16_t TextBlock::wordXpos(uint16_t index) const {
  if (index >= wordCount()) return 0;
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent)
    return static_cast<int16_t>(
        floorPixel(static_cast<int64_t>(nativeData.alignmentX26) + nativeData.words[index].x26));
#endif
  return xposArr[index];
}
EpdFontFamily::Style TextBlock::wordStyle(uint16_t index) const {
  if (index >= wordCount()) return EpdFontFamily::REGULAR;
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return static_cast<EpdFontFamily::Style>(nativeData.words[index].style);
#endif
  return static_cast<EpdFontFamily::Style>(stylesArr[index]);
}
uint8_t TextBlock::focusBoundary(uint16_t index) const {
  return index < numWords && focusPresent ? focusBoundaryArr[index] : 0;
}
uint16_t TextBlock::focusSuffixX(uint16_t index) const {
  return index < numWords && focusPresent ? focusSuffixXArr[index] : 0;
}
int TextBlock::wordWidth(uint16_t index) const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent && index < wordCount()) {
    const auto& word = nativeData.words[index];
    const int64_t left = static_cast<int64_t>(nativeData.alignmentX26) + word.x26;
    return ceilPixel(left + word.width26) - floorPixel(left);
  }
#endif
  return 0;  // Legacy selection measures its token using the renderer.
}
int TextBlock::wordTop(uint16_t index) const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent && index < wordCount()) return nativeData.words[index].top;
#endif
  return 0;
}
int TextBlock::wordHeight(uint16_t index) const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent && index < wordCount()) return nativeData.words[index].height;
#endif
  return 0;
}
int TextBlock::getRubyShift(int ascender) const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return nativeData.rubyLift;
#endif
  return hasRuby() ? ascender / 2 : 0;
}
size_t TextBlock::RubyTextView::size() const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (block->nativePresent) return block->nativeData.ruby.size();
#endif
  return block->rubyTexts.size();
}
std::string_view TextBlock::RubyTextView::operator[](size_t index) const {
  if (index >= size()) return {};
#ifdef CROSSPOINT_NATIVE_TEXT
  if (block->nativePresent) return block->nativeRubyText(index);
#endif
  return block->rubyTexts[index];
}

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const uint16_t textBytes) {
  // Layout documented in TextBlock.h: 16-bit arrays first, then 8-bit arrays, then text.
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  return size + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  size_t off = wc * 4;
  if (focusPresent) {
    focusSuffixXArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const BlockStyle& blockStyle,
                     std::vector<std::string> rubyTexts, std::vector<LinkSpan> linkSpans)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)) {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (!this->linkSpans.assign({linkSpans.data(), linkSpans.size()})) {
    isValid = false;
    LOG_ERR("TEXT", "Native link allocation failed");
    return;
  }
#else
  this->linkSpans = std::move(linkSpans);
#endif
  // Same invariant as deserialize(): a block never holds an all-empty rubyTexts, so a
  // ruby-less line costs nothing beyond its arena. The layout engine hands one over for
  // every line it extracts, ruby or not; release it here rather than carrying it for the
  // block's lifetime. Move-assigning an empty vector frees the buffer (clear() would not).
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  // Focus annotations are optional: empty vectors mean no word in this block has a split.
  // When present, they must be sized in lockstep with words[].
  const bool hasFocus = !focusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > 10000 ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusSuffixX.size()))) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(focusBoundary.size()),
            static_cast<uint32_t>(focusSuffixX.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  focusPresent = hasFocus;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical
  // row of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, focusPresent, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  // Pass 2: fill. Mutable aliases of the const views bound above.
  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (focusPresent) {
    auto* suffixX = const_cast<uint16_t*>(focusSuffixXArr);
    auto* boundary = const_cast<uint8_t*>(focusBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      suffixX[i] = focusSuffixX[i];
      boundary[i] = focusBoundary[i];
    }
  }
}

bool TextBlock::hasRuby() const {
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return !nativeData.ruby.empty();
#endif
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) {
    renderer.drawNativeLine(fontId, nativeData, x, y);
    return;
  }
#endif

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);

  // Resolve ruby positions. Layout (extractLine) has already reserved extraStartOffset on the
  // left and extraEndOffset on the right, so the centered rubyX is always within the page margins.
  struct RubyDrawInfo {
    int x;
    std::string text;
    BidiUtils::BidiBaseDir baseDir;
  };
  const bool blockHasRuby = hasRuby();
  std::vector<RubyDrawInfo> rubies;
  if (blockHasRuby) {
    rubies.resize(numWords);
    for (uint16_t i = 0; i < numWords; i++) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
        int groupWordCount = 1;
        while (i + groupWordCount < numWords && (wordStyle(i + groupWordCount) & EpdFontFamily::RUBY_CONTINUE) != 0) {
          groupWordCount++;
        }
        int groupActualWidth = 0;
        for (int k = 0; k < groupWordCount; ++k) {
          groupActualWidth += renderer.getTextAdvanceX(fontId, wordText(i + k), wordStyle(i + k));
        }
        const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
        const int leaderWordX = xposArr[i] + x;
        const auto baseDir =
            static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(wordText(i), blockStyle.isRtl ? 1 : 0));
        rubies[i] = {leaderWordX - (rubyWidth - groupActualWidth) / 2, rubyTexts[i], baseDir};
        i += groupWordCount - 1;
      }
    }
  }

  struct DecorationLineTracker {
    EpdFontFamily::Style style;
    int yOffset;
    int startX = -1;
    int endX = -1;
    int yPos = 0;

    bool active() const { return startX != -1; }
    void reset() {
      startX = -1;
      endX = -1;
      yPos = 0;
    }
  };

  DecorationLineTracker decorationLines[] = {
      {EpdFontFamily::UNDERLINE, ascender + 2},
      {EpdFontFamily::STRIKETHROUGH, ascender * 4 / 5},
  };

  const auto flushDecoration = [&](DecorationLineTracker& line) {
    if (line.active()) {
      renderer.drawLine(line.startX, line.yPos, line.endX, line.yPos, 2, true);
      line.reset();
    }
  };
  const auto flushDecorations = [&]() {
    for (auto& line : decorationLines) {
      flushDecoration(line);
    }
  };

  // Loop-invariant: hoisted out of the word loop so rubyTexts is scanned once,
  // not once per word.
  const int rubyShift = getRubyShift(ascender);

  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = wordText(i);
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const auto baseDir =
        static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = focusBoundary(i);

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y + rubyShift;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    const int drawX = wordX;

    if (boundary > 0) {
      // Focus split: draw bold prefix, then the regular suffix at a pre-computed x offset.
      // The bold prefix is bounded to 9 codepoints by the clamp on targetBoldChars in
      // ParsedText::addWord; 9 UTF-8 codepoints occupy at most 9 * 4 = 36 bytes, +1 for null = 37.
      // suffixX is computed at cache-creation time to avoid font metric lookups at render time.
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen =
          std::min<size_t>({static_cast<size_t>(boundary), static_cast<size_t>(wordTextLen(i)), sizeof(boldBuf) - 1});
      memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, drawX, wordY, boldBuf, true, boldStyle, baseDir);
      const int suffixX = drawX + focusSuffixXArr[i];
      renderer.drawText(fontId, suffixX, wordY, word + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, drawX, wordY, word, true, currentStyle, baseDir);
    }

    // Horizontal ruby text rendering
    if (blockHasRuby && i < rubyTexts.size() && !rubyTexts[i].empty() &&
        (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
      const int rubyY = wordY - ascender;
      renderer.drawText(fontId, rubies[i].x, rubyY, rubies[i].text.c_str(), true, EpdFontFamily::SUP,
                        rubies[i].baseDir);
    }

    if (scanning) {
      continue;
    }

    if (EpdFontFamily::hasTextDecoration(currentStyle)) {
      int lineStartX = drawX;
      int lineWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        lineWidth = (lineWidth + 1) / 2;
      }

      // Do not decorate the synthetic em-space used for paragraph indentation.
      if (wordTextLen(i) >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visibleText = word + 3;
        lineStartX += renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        lineWidth = renderer.getTextWidth(fontId, visibleText, currentStyle, baseDir);
        if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
          lineWidth = (lineWidth + 1) / 2;
        }
      }

      for (auto& line : decorationLines) {
        if ((currentStyle & line.style) == 0) {
          flushDecoration(line);
          continue;
        }

        const int lineY = wordY + line.yOffset;
        if (line.active() && line.yPos != lineY) {
          flushDecoration(line);
        }
        if (!line.active()) {
          line.startX = lineStartX;
          line.yPos = lineY;
        }
        line.endX = lineStartX + lineWidth;
      }
    } else {
      flushDecorations();
    }
  }
  flushDecorations();
}

bool TextBlock::serialize(HalFile& file) const {
  if (!isValid || !writeField(file, static_cast<uint8_t>(nativeLine() ? 1 : 0))) return false;
#ifdef CROSSPOINT_NATIVE_TEXT
  if (nativePresent) return writeNative(file, nativeData) && writeStyle(file, blockStyle);
#endif
  if (!writeField(file, numWords) || !writeField(file, static_cast<uint8_t>(focusPresent)) ||
      !writeField(file, textBytes))
    return false;
  if (numWords > 0) {
    const size_t bytes = arenaSize(numWords, focusPresent, textBytes);
    if (file.write(arena.get(), bytes) != bytes) return false;
  }
  for (size_t i = 0; i < numWords; ++i) {
    const std::string_view ruby = i < rubyTexts.size() ? std::string_view(rubyTexts[i]) : std::string_view{};
    if (ruby.size() > UINT16_MAX || !writeField(file, static_cast<uint32_t>(ruby.size())) ||
        (!ruby.empty() && file.write(ruby.data(), ruby.size()) != ruby.size()))
      return false;
  }
  return writeStyle(file, blockStyle);
}

std::unique_ptr<TextBlock> TextBlock::deserialize(serialization::BoundedFileReader& file) {
  uint8_t representation = 0;
  if (!readField(file, representation) || representation > 1) return nullptr;
  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    file.outOfMemory();
    LOG_ERR("PGE", "Deserialization failed: could not allocate TextBlock");
    return nullptr;
  }
  if (representation == 1) {
#ifdef CROSSPOINT_NATIVE_TEXT
    block->nativePresent = true;
    if (!readNative(file, block->nativeData, block->blockStyle)) return nullptr;
    return block;
#else
    return nullptr;
#endif
  }
  uint16_t wc = 0, bytes = 0;
  uint8_t hasFocus = 0;
  if (!readField(file, wc) || !readField(file, hasFocus) || !readField(file, bytes) || wc > 10000 || hasFocus > 1 ||
      (wc == 0 && bytes != 0) || (wc > 0 && bytes < wc))
    return nullptr;
  block->numWords = wc;
  block->textBytes = bytes;
  block->focusPresent = hasFocus != 0;
  const size_t size = arenaSize(wc, block->focusPresent, bytes);
  if (!availableBytes(file, size + size_t(wc) * sizeof(uint32_t) + 23)) return nullptr;
  if (wc > 0) {
    block->arena = makeUniqueNoThrow<uint8_t[]>(size);
    if (!block->arena) {
      file.outOfMemory();
      LOG_ERR("PGE", "Deserialization failed: could not allocate text arena");
      return nullptr;
    }
    if (file.read(block->arena.get(), size) != size) return nullptr;
    block->bindArenaPointers();
    if (block->textOffArr[0] != 0) return nullptr;
    for (uint16_t i = 0; i < wc; ++i) {
      const size_t start = block->textOffArr[i];
      const size_t end = i + 1 < wc ? block->textOffArr[i + 1] : bytes;
      if (start >= end || end > bytes || block->textArr[end - 1] != '\0' ||
          memchr(block->textArr + start, '\0', end - start - 1))
        return nullptr;
      if (hasFocus) {
        const size_t boundary = block->focusBoundaryArr[i];
        if (boundary > end - start - 1 ||
            (boundary < end - start - 1 && (static_cast<uint8_t>(block->textArr[start + boundary]) & 0xc0) == 0x80))
          return nullptr;
      }
    }
  }
  size_t totalRubyBytes = 0;
  for (uint16_t i = 0; i < wc; ++i) {
    uint32_t length = 0;
    if (!readField(file, length) || length > UINT16_MAX - totalRubyBytes || !availableBytes(file, length))
      return nullptr;
    totalRubyBytes += length;
    if (length == 0) continue;
    if (block->rubyTexts.empty()) block->rubyTexts.resize(wc);
    auto& ruby = block->rubyTexts[i];
    ruby.resize(length);
    if (file.read(ruby.data(), length) != length || memchr(ruby.data(), '\0', length)) return nullptr;
  }
  if (!readStyle(file, block->blockStyle)) return nullptr;
  return block;
}
