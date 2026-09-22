#include "ParsedText.h"

#ifdef CROSSPOINT_NATIVE_TEXT
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <NativeTextEngine.h>
#include <NativeUtf8.h>
extern "C" {
#include <minibidi.h>
}
#undef when
#undef otherwise

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr size_t NATIVE_WINDOW_SCALARS = 4096;
constexpr size_t NATIVE_WINDOW_BYTES = 16384;
// Expat's parser adapter flushes after each complete MAX_WORD_SIZE chunk.
constexpr size_t NATIVE_CHUNK_CARRY = 200;

template <class T>
bool reserveNative(NativeBuffer<T>& buffer, const size_t needed, const size_t maximum) {
  if (needed > maximum) return false;
  if (needed <= buffer.capacity()) return true;
  return buffer.reserve(std::min(maximum, std::max(needed, std::max<size_t>(16, buffer.capacity() * 2))));
}

// P2/P3 direction is also needed before fitting to decide whether CSS alignment
// is the natural edge for first-line indentation. The fitter remains the sole
// authority for the resolved paragraph and per-line bidi levels.
bool nativeNaturalRtl(const std::string_view text) {
  size_t offset = 0, isolates = 0;
  uint32_t cp;
  while (native_text::nextUtf8(text, offset, cp)) {
    const auto cls = bidi_class(cp);
    if (cls == LRI || cls == RLI || cls == FSI) {
      ++isolates;
      continue;
    }
    if (cls == PDI) {
      if (isolates) --isolates;
      continue;
    }
    if (isolates) continue;
    if (cls == L) return false;
    if (cls == R || cls == AL) return true;
  }
  return false;
}

int floorNativePixel(const int32_t value) {
  return value >= 0 ? value / 64 : -static_cast<int>((-static_cast<int64_t>(value) + 63) / 64);
}
int ceilNativePixel(const int32_t value) {
  return value >= 0 ? static_cast<int>((static_cast<int64_t>(value) + 63) / 64) : -(-value / 64);
}
}  // namespace

void ParsedText::failNative(const TextStatus status) {
  if (nativeStatus != TextStatus::Ok) return;
  nativeStatus = status;
  LOG_ERR("TEXT", "Native EPUB input/layout failed (%u)", static_cast<unsigned>(status));
}

void ParsedText::discardNativeInput() {
  nativeText.clear();
  nativeFragments.clear();
  nativeAnchors.clear();
  nativeRuby.clear();
  nativeRubyText.clear();
  nativeScalars = 0;
}
void ParsedText::addWord(const std::string_view word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint32_t visibleTextOffset, const uint8_t linkId) {
  appendNative(word, fontStyle, underline, attachToPrevious, visibleTextOffset, linkId, false);
}

void ParsedText::addSyntheticText(const std::string_view text, const EpdFontFamily::Style style,
                                  const uint32_t anchorOffset, const bool attachToPrevious, const uint8_t linkId) {
  appendNative(text, style, false, attachToPrevious, anchorOffset, linkId, true);
}

void ParsedText::appendNative(const std::string_view word, const EpdFontFamily::Style fontStyle, const bool underline,
                              const bool attachToPrevious, const uint32_t visibleTextOffset, const uint8_t linkId,
                              const bool synthetic) {
  if (nativeStatus != TextStatus::Ok || word.empty()) return;
  size_t offset = 0, scalars = 0;
  uint32_t cp;
  while (offset < word.size()) {
    if (!native_text::nextUtf8(word, offset, cp) || cp == 0) {
      failNative(TextStatus::InvalidText);
      return;
    }
    ++scalars;
  }
  const bool space = !attachToPrevious && !nativeText.empty();
  const size_t addedBytes = word.size() + space, addedScalars = scalars + space;
  if (addedBytes > NATIVE_WINDOW_BYTES + NATIVE_CHUNK_CARRY - nativeText.size() ||
      addedScalars > NATIVE_WINDOW_SCALARS + NATIVE_CHUNK_CARRY - nativeScalars ||
      (!synthetic && scalars > UINT32_MAX - visibleTextOffset)) {
    failNative(TextStatus::CapacityExceeded);
    return;
  }
  if (linkId > nativeLinkTargets.size()) {
    failNative(TextStatus::InvalidText);
    return;
  }
  const size_t oldBytes = nativeText.size();
  const size_t newBytes = oldBytes + addedBytes;
  const size_t oldScalars = nativeScalars;
  const size_t newScalars = oldScalars + addedScalars;
  if (!reserveNative(nativeText, newBytes, NATIVE_WINDOW_BYTES + NATIVE_CHUNK_CARRY) ||
      !reserveNative(nativeFragments, nativeFragments.size() + 1, NATIVE_WINDOW_SCALARS + NATIVE_CHUNK_CARRY) ||
      !reserveNative(nativeAnchors, newScalars + 1, NATIVE_WINDOW_SCALARS + NATIVE_CHUNK_CARRY + 1)) {
    failNative(TextStatus::OutOfMemory);
    return;
  }
  nativeText.resize(newBytes);
  nativeAnchors.resize(newScalars + 1);
  if (space) {
    nativeText[oldBytes] = ' ';
    // Collapsed HTML whitespace occupies its original source interval, which
    // can span more than one codepoint. The following fragment owns its end.
    nativeAnchors[oldScalars].byteOffset = static_cast<uint32_t>(oldBytes);
    if (synthetic) nativeAnchors[oldScalars].sourceOffset = visibleTextOffset;
  }
  const size_t startByte = oldBytes + space;
  std::memcpy(nativeText.data() + startByte, word.data(), word.size());
  offset = 0;
  size_t index = oldScalars + space;
  uint32_t source = visibleTextOffset;
  while (offset < word.size()) {
    nativeAnchors[index++] = {static_cast<uint32_t>(startByte + offset), source};
    if (!synthetic) ++source;
    native_text::nextUtf8(word, offset, cp);
  }
  nativeAnchors[index] = {static_cast<uint32_t>(newBytes), source};
  const uint8_t style = static_cast<uint8_t>(fontStyle) | (underline ? EpdFontFamily::UNDERLINE : 0);
  const size_t fragment = nativeFragments.size();
  nativeFragments.resize(fragment + 1);
  nativeFragments[fragment] = {static_cast<uint32_t>(startByte), static_cast<uint32_t>(newBytes), style, linkId};
  nativeScalars = newScalars;
}

uint8_t ParsedText::addLinkTarget(const char* href) {
  if (nativeStatus != TextStatus::Ok || !href || !*href) return 0;
  const size_t bytes = strnlen(href, FOOTNOTE_HREF_LEN);
  if (bytes >= FOOTNOTE_HREF_LEN) return 0;
  // Reuse the identity, including across retained layout windows.
  for (size_t i = 0; i < nativeLinkTargets.size(); ++i)
    if (std::strcmp(nativeLinkTargets[i].href, href) == 0) return nativeLastLinkId = static_cast<uint8_t>(i + 1);
  for (size_t i = 0; i < nativeLinkTargets.size(); ++i) {
    if (*nativeLinkTargets[i].href) continue;
    std::memcpy(nativeLinkTargets[i].href, href, bytes + 1);
    return nativeLastLinkId = static_cast<uint8_t>(i + 1);
  }
  if (nativeLinkTargets.size() == UINT8_MAX) {
    failNative(TextStatus::CapacityExceeded);
    return 0;
  }
  const size_t index = nativeLinkTargets.size();
  if (!reserveNative(nativeLinkTargets, index + 1, UINT8_MAX)) {
    failNative(TextStatus::OutOfMemory);
    return 0;
  }
  nativeLinkTargets.resize(index + 1);
  std::memcpy(nativeLinkTargets[index].href, href, bytes + 1);
  return nativeLastLinkId = static_cast<uint8_t>(index + 1);
}

bool ParsedText::linkTargetMatches(const uint8_t linkId, const char* href) const {
  return href && linkId && linkId <= nativeLinkTargets.size() &&
         std::strcmp(nativeLinkTargets[linkId - 1].href, href) == 0;
}

EpdFontFamily::Style ParsedText::getWordStyleAt(const size_t index) const {
  return index < nativeFragments.size() ? static_cast<EpdFontFamily::Style>(nativeFragments[index].style)
                                        : EpdFontFamily::REGULAR;
}

std::string_view ParsedText::getRubyTextAt(const size_t index) const {
  if (index < nativeFragments.size()) {
    for (const auto& ruby : nativeRuby.span()) {
      if (ruby.startByte == nativeFragments[index].startByte)
        return ruby.textBytes ? std::string_view(nativeRubyText.data() + ruby.textOffset, ruby.textBytes)
                              : std::string_view();
    }
  }
  return {};
}

void ParsedText::setRubyForWordAt(const size_t index, const std::string_view ruby) {
  if (index >= nativeFragments.size()) return;
  size_t count = 1;
  for (const auto& group : nativeRuby.span()) {
    if (group.startByte != nativeFragments[index].startByte) continue;
    while (index + count < nativeFragments.size() && nativeFragments[index + count].startByte < group.endByte) ++count;
    break;
  }
  setRubyGroupAt(index, count, ruby);
}

void ParsedText::setRubyGroupAt(const size_t startIndex, const size_t count, std::string_view text) {
  if (nativeStatus != TextStatus::Ok || startIndex >= nativeFragments.size() || !count) return;
  size_t offset = 0;
  uint32_t cp;
  while (offset < text.size()) {
    if (!native_text::nextUtf8(text, offset, cp) || cp == 0) {
      failNative(TextStatus::InvalidText);
      return;
    }
  }
  const size_t stopIndex = startIndex + std::min(count, nativeFragments.size() - startIndex);
  const uint32_t start = nativeFragments[startIndex].startByte, end = nativeFragments[stopIndex - 1].endByte;
  size_t at = 0;
  while (at < nativeRuby.size() && nativeRuby[at].startByte < start) ++at;
  const bool replace = at < nativeRuby.size() && nativeRuby[at].startByte == start;
  if ((at && nativeRuby[at - 1].endByte > start) ||
      (at + replace < nativeRuby.size() && nativeRuby[at + replace].startByte < end)) {
    failNative(TextStatus::InvalidText);
    return;
  }
  const size_t oldLength = replace ? nativeRuby[at].textBytes : 0;
  const size_t oldOffset = replace ? nativeRuby[at].textOffset
                                   : (at < nativeRuby.size() ? nativeRuby[at].textOffset : nativeRubyText.size());
  if (text.size() > NATIVE_WINDOW_BYTES - (nativeRubyText.size() - oldLength)) {
    failNative(TextStatus::CapacityExceeded);
    return;
  }
  // The public getter borrows this arena. Preserve such a view before growth
  // or moving a later annotation into the replacement's tail.
  NativeBuffer<char> borrowed;
  const auto sourceAddress = reinterpret_cast<uintptr_t>(text.data());
  const auto arenaAddress = reinterpret_cast<uintptr_t>(nativeRubyText.data());
  if (!text.empty() && sourceAddress >= arenaAddress && sourceAddress - arenaAddress < nativeRubyText.size()) {
    if (!borrowed.assign({text.data(), text.size()})) {
      failNative(TextStatus::OutOfMemory);
      return;
    }
    text = {borrowed.data(), borrowed.size()};
  }
  const size_t newSize = nativeRubyText.size() - oldLength + text.size();
  if (!reserveNative(nativeRubyText, newSize, NATIVE_WINDOW_BYTES) ||
      !reserveNative(nativeRuby, nativeRuby.size() + !replace, NATIVE_WINDOW_SCALARS + NATIVE_CHUNK_CARRY)) {
    failNative(TextStatus::OutOfMemory);
    return;
  }
  if (replace) {
    for (size_t i = startIndex + 1; i < nativeFragments.size() && nativeFragments[i].startByte < nativeRuby[at].endByte;
         ++i)
      nativeFragments[i].style &= ~EpdFontFamily::RUBY_CONTINUE;
  }
  const size_t tail = nativeRubyText.size() - oldOffset - oldLength;
  if (tail)
    std::memmove(nativeRubyText.data() + oldOffset + text.size(), nativeRubyText.data() + oldOffset + oldLength, tail);
  nativeRubyText.resize(newSize);
  if (!text.empty()) std::memcpy(nativeRubyText.data() + oldOffset, text.data(), text.size());
  if (!replace) {
    const size_t oldCount = nativeRuby.size();
    nativeRuby.resize(oldCount + 1);
    if (oldCount > at)
      std::memmove(nativeRuby.data() + at + 1, nativeRuby.data() + at, (oldCount - at) * sizeof(NativeRubyRecord));
  }
  for (size_t i = at + 1; i < nativeRuby.size(); ++i)
    nativeRuby[i].textOffset = static_cast<uint32_t>(nativeRuby[i].textOffset - oldLength + text.size());
  nativeRuby[at] = {start, end, static_cast<uint32_t>(oldOffset), static_cast<uint32_t>(text.size()),
                    static_cast<uint8_t>(nativeFragments[startIndex].style & ~EpdFontFamily::RUBY_CONTINUE)};
  for (size_t i = startIndex + 1; i < stopIndex; ++i) nativeFragments[i].style |= EpdFontFamily::RUBY_CONTINUE;
}

void ParsedText::ensureRubyCapacity() {
  // All ruby storage grows through the checked native allocator on attachment.
}

void ParsedText::consumeNativePrefix(const size_t bytes) {
  size_t scalars = 0;
  while (scalars < nativeScalars && nativeAnchors[scalars].byteOffset < bytes) ++scalars;
  const size_t remaining = nativeText.size() - bytes;
  if (remaining) std::memmove(nativeText.data(), nativeText.data() + bytes, remaining);
  nativeText.resize(remaining);
  const size_t anchorCount = nativeAnchors.size() - scalars;
  std::memmove(nativeAnchors.data(), nativeAnchors.data() + scalars, anchorCount * sizeof(NativeSourceAnchor));
  nativeAnchors.resize(anchorCount);
  for (auto& anchor : nativeAnchors.span()) anchor.byteOffset -= static_cast<uint32_t>(bytes);
  nativeScalars -= scalars;
  size_t write = 0;
  for (auto fragment : nativeFragments.span()) {
    if (fragment.endByte <= bytes) continue;
    fragment.startByte = static_cast<uint32_t>(std::max<size_t>(fragment.startByte, bytes) - bytes);
    fragment.endByte -= static_cast<uint32_t>(bytes);
    nativeFragments[write++] = fragment;
  }
  nativeFragments.resize(write);
  size_t rubyBytes = 0;
  write = 0;
  for (auto ruby : nativeRuby.span()) {
    if (ruby.endByte <= bytes) continue;
    ruby.startByte -= static_cast<uint32_t>(bytes);
    ruby.endByte -= static_cast<uint32_t>(bytes);
    if (ruby.textBytes)
      std::memmove(nativeRubyText.data() + rubyBytes, nativeRubyText.data() + ruby.textOffset, ruby.textBytes);
    ruby.textOffset = static_cast<uint32_t>(rubyBytes);
    rubyBytes += ruby.textBytes;
    nativeRuby[write++] = ruby;
  }
  nativeRuby.resize(write);
  nativeRubyText.resize(rubyBytes);
  // A link can still be open when its last currently buffered fragment commits.
  bool referenced[UINT8_MAX] = {};
  if (nativeLastLinkId) referenced[nativeLastLinkId - 1] = true;
  for (const auto& fragment : nativeFragments.span())
    if (fragment.linkId) referenced[fragment.linkId - 1] = true;
  for (size_t i = 0; i < nativeLinkTargets.size(); ++i)
    if (!referenced[i]) nativeLinkTargets[i].href[0] = '\0';
}

bool ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                                       const bool includeLastLine) {
  return layoutNative(renderer, fontId, viewportWidth, processLine, includeLastLine, false);
}

bool ParsedText::layoutBeforeRuby(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                  const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine) {
  return layoutNative(renderer, fontId, viewportWidth, processLine, false, true);
}

bool ParsedText::layoutNative(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                              const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                              const bool includeLastLine, const bool semanticBoundary) {
  const auto fail = [&](const TextStatus status) {
    failNative(status);
    discardNativeInput();
    renderer.recordTextFailure(nativeStatus);
    return false;
  };
  if (nativeStatus != TextStatus::Ok) return fail(nativeStatus);
  if (nativeText.empty()) return true;
  if (!includeLastLine && !semanticBoundary && !nativeNeedsLayout()) return true;
  if (!renderer.nativeTextEngine()) return fail(TextStatus::InvalidFont);
  if (!processLine || !viewportWidth) return fail(TextStatus::InvalidText);
  renderer.clearTextStatus();

  // A fitter failure must not publish part of this operation to the parser.
  // Staging is bounded by the source window, and every control/payload is charged
  // to the same native budget. The public callback itself remains borrowed.
  struct PendingLines {
    NativeBuffer<TextBlock*> blocks;
    ParsedText* owner;
    ~PendingLines() {
      for (auto* block : blocks.span()) delete block;
    }
  } pending{{}, this};
  if (!pending.blocks.reserve(nativeScalars)) return fail(TextStatus::OutOfMemory);
  const auto emit = [](void* context, NativeLayoutEmission&& value) -> TextStatus {
    auto& pending = *static_cast<PendingLines*>(context);
    auto& owner = *pending.owner;
    NativeBuffer<TextBlock::LinkSpan> links;
    if (!links.resize(value.links.size())) return TextStatus::OutOfMemory;
    for (size_t i = 0; i < value.links.size(); ++i) {
      const auto& input = value.links[i];
      if (!input.id || input.id > owner.nativeLinkTargets.size()) return TextStatus::InvalidText;
      const int left = floorNativePixel(input.x26);
      const int right = ceilNativePixel(input.x26 + input.width26);
      if (left < INT16_MIN || left > INT16_MAX || right - left > INT16_MAX) return TextStatus::CapacityExceeded;
      auto& link = links[i];
      std::memcpy(link.href, owner.nativeLinkTargets[input.id - 1].href, sizeof(link.href));
      link.x = static_cast<int16_t>(left);
      link.width = static_cast<int16_t>(right - left);
      link.topLift = 0;
      link.top = input.top;
      link.height = input.height;
    }
    BlockStyle style = owner.blockStyle;
    style.isRtl = (value.line.paragraphLevel & 1) != 0;
    auto block = makeUniqueNoThrow<TextBlock>(std::move(value.line), style, std::move(links));
    if (!block || !block->valid()) return TextStatus::OutOfMemory;
    block->setSourceRange(value.sourceStart, value.sourceEnd);
    const size_t index = pending.blocks.size();
    if (!pending.blocks.resize(index + 1)) return TextStatus::OutOfMemory;
    pending.blocks[index] = block.release();
    return TextStatus::Ok;
  };

  NativeParagraphLayout fitter(*renderer.nativeTextEngine());
  NativeBuffer<NativeStyleSpan> spans;
  NativeBuffer<NativeLinkRange> links;
  NativeBuffer<NativeRubyInput> ruby;
  bool atSemanticBoundary = semanticBoundary;
  while (!nativeText.empty() && (includeLastLine || nativeNeedsLayout() || atSemanticBoundary)) {
    const std::string_view text(nativeText.data(), nativeText.size());
    size_t windowBytes = 0;
    const auto prefixStatus = NativeParagraphLayout::windowPrefix(text, windowBytes, true);
    if (prefixStatus != TextStatus::Ok) return fail(prefixStatus);
    if (!windowBytes) return fail(TextStatus::CapacityExceeded);
    if (!spans.reserve(nativeFragments.size()) || !links.reserve(nativeFragments.size()) ||
        !ruby.resize(nativeRuby.size()))
      return fail(TextStatus::OutOfMemory);
    spans.clear();
    links.clear();
    for (const auto& fragment : nativeFragments.span()) {
      const uint8_t style = fragment.style & ~EpdFontFamily::RUBY_CONTINUE;
      const uint32_t start = spans.empty() ? 0 : spans[spans.size() - 1].endByte;
      if (!spans.empty()) spans[spans.size() - 1].endByte = fragment.startByte;
      if (!spans.empty() && spans[spans.size() - 1].style == style) {
        spans[spans.size() - 1].endByte = fragment.endByte;
      } else {
        const size_t index = spans.size();
        spans.resize(index + 1);
        spans[index] = {index ? fragment.startByte : 0, fragment.endByte, style, 0};
      }
      if (fragment.linkId) {
        if (!links.empty() && links[links.size() - 1].id == fragment.linkId &&
            links[links.size() - 1].endByte == start) {
          links[links.size() - 1].endByte = fragment.endByte;
        } else {
          const size_t index = links.size();
          links.resize(index + 1);
          links[index] = {fragment.startByte, fragment.endByte, fragment.linkId};
        }
      }
    }
    for (size_t i = 0; i < nativeRuby.size(); ++i) {
      const auto& input = nativeRuby[i];
      ruby[i] = {input.startByte,
                 input.endByte,
                 {input.textBytes ? nativeRubyText.data() + input.textOffset : "", input.textBytes},
                 input.style};
    }
    NativeParagraphView view;
    view.text = text;
    view.spans = spans.span();
    view.sourceAnchors = nativeAnchors.span();
    view.links = links.span();
    view.ruby = ruby.span();
    view.paragraphLevel = nativeParagraphLevel < 0 && blockStyle.directionDefined
                              ? static_cast<int8_t>(blockStyle.isRtl)
                              : nativeParagraphLevel;
    view.final = includeLastLine && windowBytes == text.size();
    view.semanticBoundary = atSemanticBoundary;
    atSemanticBoundary = false;
    NativeLayoutOptions options;
    options.fontId = fontId;
    options.width = viewportWidth;  // Parser already removed horizontal insets.
    options.hyphenation = hyphenationEnabled;
    options.emergencyHyphenation = true;
    options.focus = focusReadingEnabled;
    options.readerFeatures = true;
    options.firstLine = nativeFirstLine;
    switch (blockStyle.alignment) {
      case CssTextAlign::Left:
        options.alignment = NativeAlignment::Left;
        break;
      case CssTextAlign::Right:
        options.alignment = NativeAlignment::Right;
        break;
      case CssTextAlign::Center:
        options.alignment = NativeAlignment::Center;
        break;
      case CssTextAlign::Justify:
        options.alignment = NativeAlignment::Justify;
        break;
      default:
        options.alignment = NativeAlignment::Start;
        break;
    }
    const bool rtl =
        view.paragraphLevel >= 0 ? (view.paragraphLevel & 1) : nativeNaturalRtl(text.substr(0, windowBytes));
    const bool natural =
        options.alignment == NativeAlignment::Start || options.alignment == NativeAlignment::Justify ||
        (rtl ? options.alignment == NativeAlignment::Right : options.alignment == NativeAlignment::Left);
    if (nativeFirstLine && natural) {
      if (blockStyle.textIndentDefined) {
        if (blockStyle.textIndent < 0 || !extraParagraphSpacing) options.firstLineIndent = blockStyle.textIndent;
      } else if (!extraParagraphSpacing) {
        const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
        if (renderer.lastTextStatus() != TextStatus::Ok) return fail(renderer.lastTextStatus());
        if (spaceWidth > INT16_MAX / 3) return fail(TextStatus::CapacityExceeded);
        options.firstLineIndent = static_cast<int16_t>(spaceWidth * 3);
      }
    }
    size_t consumed = 0;
    const auto status = fitter.layout(view, options, emit, &pending, consumed, nativeParagraphLevel);
    if (status != TextStatus::Ok) return fail(status);
    if (!consumed) {
      if (view.semanticBoundary) break;
      return fail(TextStatus::CapacityExceeded);
    }
    nativeFirstLine = false;
    blockStyle.isRtl = (nativeParagraphLevel & 1) != 0;
    consumeNativePrefix(consumed);
  }
  for (auto*& block : pending.blocks.span()) {
    const uint32_t source = block->sourceStartOffset();
    std::unique_ptr<TextBlock> owned(block);
    block = nullptr;
    processLine(std::move(owned), source);
  }
  return true;
}

#else

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "CjkBreakPolicy.h"
#include "TokenBoundary.h"
#include "hyphenation/HyphenationCommon.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;
constexpr size_t MIN_JUSTIFY_GAPS = 1;

// Byte-level pre-check: Hebrew UTF-8 lead bytes 0xD6-0xD7, Arabic/Syriac 0xD8-0xDB.
bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

using CjkBreakPolicy::hasCjkBreakOpportunityBetween;
using CjkBreakPolicy::isNoBreakAfterCjkPunctuation;
using CjkBreakPolicy::isNoBreakBeforeCjkPunctuation;

bool containsCjkBreakableCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

uint32_t countCodepoints(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* const end = ptr + text.size();
  uint32_t count = 0;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

std::vector<size_t> cjkCharacterBreakByteOffsets(const std::string& text) {
  struct CodepointBoundary {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointBoundary> codepoints;
  codepoints.reserve(text.size());
  bool hasCjkBreakable = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) {
      hasCjkBreakable = true;
    }
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjkBreakable || codepoints.size() < 2) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    const uint32_t current = codepoints[i].cp;
    const uint32_t next = codepoints[i + 1].cp;
    if (!hasCjkBreakOpportunityBetween(current, next)) continue;
    allowedOffsets.push_back(codepoints[i].endOffset);
  }
  return allowedOffsets;
}

int computeJustifyExtra(const int spareSpace, const size_t gapCount) {
  if (gapCount < MIN_JUSTIFY_GAPS || spareSpace <= 0) return 0;
  // Distribute the spare space evenly across gaps. Do NOT bail out to 0 when the
  // per-gap stretch is large: a sparse line (few words on a wide page) legitimately
  // needs big gaps to reach the margin. Returning 0 there disables justification for
  // that line, leaving it right-aligned (RTL) / left-aligned (LTR) — the mismatched
  // alignment bug. Match the un-capped behavior of the old code.
  return spareSpace / static_cast<int>(gapCount);
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

// True when a token ends with a hyphen or dash that allows a line break after it. U+00AD is
// excluded because it renders as nothing, so only a hyphenation break -- which substitutes a
// visible '-' -- may use it; U+2011 because forbidding that break is its purpose.
bool endsWithBreakableHyphen(const std::string& token) {
  if (token.empty()) return false;
  return TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(token));
}

// Focus Reading renders the first `focusBoundary` bytes of a token bold and the rest at the
// token's own style; 0 means no emphasis. The bold run is at most 9 codepoints (see addWord),
// so it is copied to a stack buffer rather than a substring: this runs once per word during
// pagination and must not allocate.
constexpr size_t FOCUS_PREFIX_BUF_SIZE = 40;

// Advance from the token origin to the start of its regular-weight suffix: the bold prefix plus
// the kerning across the weight change. Doubles as the suffix's x offset inside the word.
uint16_t measureFocusPrefixAdvance(const GfxRenderer& renderer, const int fontId, const std::string& word,
                                   const EpdFontFamily::Style style, const uint8_t focusBoundary) {
  char prefixBuf[FOCUS_PREFIX_BUF_SIZE];
  const size_t prefixLen = std::min<size_t>(focusBoundary, FOCUS_PREFIX_BUF_SIZE - 1);
  memcpy(prefixBuf, word.data(), prefixLen);
  prefixBuf[prefixLen] = '\0';

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  const auto* suffixPtr = reinterpret_cast<const unsigned char*>(word.c_str() + focusBoundary);
  const int kerning = renderer.getKerning(fontId, lastCodepoint(prefixBuf), utf8NextCodepoint(&suffixPtr), boldStyle);
  return static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, prefixBuf, boldStyle) + kerning);
}

// Advance width of a whole token, accounting for a bold focus prefix when it has one.
uint16_t measureFocusWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                               const EpdFontFamily::Style style, const uint8_t focusBoundary,
                               const bool appendHyphen = false) {
  if (focusBoundary == 0) {
    return measureWordWidth(renderer, fontId, word, style, appendHyphen);
  }
  if (focusBoundary >= word.size()) {
    // The bold run covers the whole token, as for a split candidate ending on the boundary.
    return measureWordWidth(renderer, fontId, word, static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD),
                            appendHyphen);
  }
  const uint16_t suffixWidth =
      appendHyphen ? measureWordWidth(renderer, fontId, word.substr(focusBoundary), style, true)
                   : static_cast<uint16_t>(renderer.getTextAdvanceX(fontId, word.c_str() + focusBoundary, style));
  return measureFocusPrefixAdvance(renderer, fontId, word, style, focusBoundary) + suffixWidth;
}

// Focus boundaries for the two halves of a token split at `splitOffset`.
uint8_t focusBoundaryBefore(const uint8_t focusBoundary, const size_t splitOffset) {
  return static_cast<uint8_t>(std::min<size_t>(focusBoundary, splitOffset));
}
uint8_t focusBoundaryAfter(const uint8_t focusBoundary, const size_t splitOffset) {
  return focusBoundary > splitOffset ? static_cast<uint8_t>(focusBoundary - splitOffset) : 0;
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

}  // namespace

uint32_t ParsedText::visibleOffsetBaseAt(const size_t wordIndex) const {
  uint32_t base = visibleOffsetBase;
  for (const auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex > wordIndex) break;
    base = rebase.base;
  }
  return base;
}

uint32_t ParsedText::visibleOffsetAt(const size_t wordIndex) const {
  if (wordIndex >= wordVisibleOffsetDeltas.size()) return 0;
  return visibleOffsetBaseAt(wordIndex) + wordVisibleOffsetDeltas[wordIndex];
}

void ParsedText::pushVisibleOffset(const uint32_t offset) {
  uint32_t base = visibleOffsetBase;
  if (wordVisibleOffsetDeltas.empty()) {
    visibleOffsetBase = offset;
    base = offset;
  } else if (!visibleOffsetRebases.empty()) {
    base = visibleOffsetRebases.back().base;
  }

  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    visibleOffsetRebases.push_back({wordVisibleOffsetDeltas.size(), offset});
    base = offset;
  }
  wordVisibleOffsetDeltas.push_back(static_cast<uint16_t>(offset - base));
}

void ParsedText::insertVisibleOffset(const size_t wordIndex, const uint32_t offset) {
  const uint32_t base = wordIndex > 0 ? visibleOffsetBaseAt(wordIndex - 1) : visibleOffsetBase;
  for (auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex >= wordIndex) rebase.wordIndex++;
  }

  uint32_t insertionBase = base;
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    const auto rebaseIt = std::find_if(visibleOffsetRebases.begin(), visibleOffsetRebases.end(),
                                       [wordIndex](const auto& rebase) { return rebase.wordIndex > wordIndex; });
    visibleOffsetRebases.insert(rebaseIt, {wordIndex, offset});
    insertionBase = offset;
  }
  wordVisibleOffsetDeltas.insert(wordVisibleOffsetDeltas.begin() + wordIndex,
                                 static_cast<uint16_t>(offset - insertionBase));
}

void ParsedText::eraseVisibleOffsetPrefix(const size_t count) {
  if (count >= wordVisibleOffsetDeltas.size()) {
    wordVisibleOffsetDeltas.clear();
    visibleOffsetRebases.clear();
    visibleOffsetBase = 0;
    return;
  }

  const uint32_t newBase = visibleOffsetBaseAt(count);
  wordVisibleOffsetDeltas.erase(wordVisibleOffsetDeltas.begin(), wordVisibleOffsetDeltas.begin() + count);
  size_t writeIndex = 0;
  for (auto rebase : visibleOffsetRebases) {
    if (rebase.wordIndex <= count) continue;
    rebase.wordIndex -= count;
    visibleOffsetRebases[writeIndex++] = rebase;
  }
  visibleOffsetRebases.resize(writeIndex);
  visibleOffsetBase = newBase;
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint32_t visibleTextOffset, const uint8_t linkId) {
  if (word.empty()) return;

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  word = utf8ComposeNfc(word);

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  const auto pushToken = [&](std::string token, const bool continues, const bool noSpaceBefore,
                             const uint8_t focusBoundary, const uint32_t tokenOffset) {
    words.push_back(std::move(token));
    wordStyles.push_back(baseStyle);
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordFocusBoundary.push_back(focusBoundary);
    wordLinkIds.push_back(linkId);
    pushVisibleOffset(tokenOffset);
    if (!rubyTexts.empty()) {
      rubyTexts.push_back("");
    }
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = false;
  // Only a glued token (attachToPrevious == true, i.e. no whitespace separated it from the
  // previous one in the source) may be turned into a gap-less break opportunity. When real
  // whitespace separated the two words, that space is content and must be rendered: Korean
  // is a space-delimited script written in Hangul, which utf8IsCjkBreakable() covers.
  if (attachToPrevious && !words.empty() &&
      hasCjkBreakOpportunityBetween(lastCodepoint(words.back()), firstCodepoint(word))) {
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }

  // Bulk-reserve the per-token parallel arrays before a burst of pushes so they
  // don't repeatedly double. Only the std::vector arrays are reserved: words and
  // rubyTexts are std::deque (chunked growth, no reserve()/capacity() and no large
  // contiguous reallocation to avoid). wordStyles' capacity gauges them all since
  // pushToken() keeps every array in lockstep.
  const auto ensureTokenCapacity = [&](const size_t additionalTokens) {
    if (additionalTokens == 0) return;
    const size_t requiredSize = words.size() + additionalTokens;
    if (wordStyles.capacity() >= requiredSize) return;

    size_t newCapacity = wordStyles.capacity() < 16 ? 16 : wordStyles.capacity();
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }

    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordNoSpaceBefore.reserve(newCapacity);
    wordFocusBoundary.reserve(newCapacity);
    wordLinkIds.reserve(newCapacity);
    wordVisibleOffsetDeltas.reserve(newCapacity);
  };

  if (auto breakOffsets = cjkCharacterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    ensureTokenCapacity(breakOffsets.size() + 1);
    bool firstToken = true;
    size_t tokenStart = 0;
    uint32_t tokenVisibleOffset = visibleTextOffset;
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
      pushToken(std::string(token), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, /*focusBoundary=*/0, tokenVisibleOffset);
      tokenVisibleOffset += countCodepoints(token);
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      pushToken(word.substr(tokenStart), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, /*focusBoundary=*/0, tokenVisibleOffset);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, /*focusBoundary=*/0,
              visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, /*focusBoundary=*/0,
              visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text).
  ensureTokenCapacity(word.length());

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore) {
    const unsigned char* wordBegin = reinterpret_cast<const unsigned char*>(word.data());
    const unsigned char* segmentBegin = reinterpret_cast<const unsigned char*>(segment.data());
    uint32_t segmentOffset = visibleTextOffset;
    const unsigned char* offsetPtr = wordBegin;
    while (offsetPtr < segmentBegin) {
      utf8NextCodepoint(&offsetPtr);
      segmentOffset++;
    }
    if (!isWord) {
      // Punctuation and Numbers stay regular
      words.emplace_back(segment);
      wordStyles.push_back(baseStyle);
      wordContinues.push_back(attach);
      wordNoSpaceBefore.push_back(noSpaceBefore);
      wordFocusBoundary.push_back(0);
      wordLinkIds.push_back(linkId);
      pushVisibleOffset(segmentOffset);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        words.emplace_back(segment);
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordFocusBoundary.push_back(0);
        wordLinkIds.push_back(linkId);
        pushVisibleOffset(segmentOffset);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // One token carrying the emphasis as a byte boundary, so the word stays whole for the
        // hyphenator and the line breaker. The renderer applies BOLD to bytes [0, splitByteOffset).
        words.emplace_back(segment);
        wordStyles.push_back(baseStyle);
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordFocusBoundary.push_back(static_cast<uint8_t>(std::min<size_t>(splitByteOffset, 255)));
        wordLinkIds.push_back(linkId);
        pushVisibleOffset(segmentOffset);
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment glues seamlessly to the prefix. After a visible explicit-hyphen
      // character, continues=true + noSpaceBefore=true records a breakable attachment: it may wrap,
      // but when it stays on the line it receives kerning only, never a space or justification.
      const bool breakAfterPrev = !isFirstSegment && !words.empty() && endsWithBreakableHyphen(words.back());
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  const bool breakAfterPrev = !isFirstSegment && !words.empty() && endsWithBreakableHyphen(words.back());
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : breakAfterPrev);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

uint8_t ParsedText::addLinkTarget(const char* href) {
  if (!href || href[0] == '\0' || strnlen(href, FOOTNOTE_HREF_LEN) >= FOOTNOTE_HREF_LEN ||
      linkTargets.size() >= UINT8_MAX) {
    return 0;
  }
  linkTargets.emplace_back(href);
  return static_cast<uint8_t>(linkTargets.size());
}

bool ParsedText::linkTargetMatches(const uint8_t linkId, const char* href) const {
  return linkId > 0 && linkId <= linkTargets.size() && href && linkTargets[linkId - 1] == href;
}

void ParsedText::setRubyForWordAt(size_t index, const std::string& ruby) {
  if (index >= words.size()) return;
  if (rubyTexts.size() <= index) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[index] = ruby;
}

void ParsedText::setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby) {
  if (startIndex >= words.size()) return;
  if (rubyTexts.size() <= startIndex) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[startIndex] = ruby;
  for (size_t i = 1; i < count; i++) {
    size_t idx = startIndex + i;
    if (idx >= words.size()) break;
    if (rubyTexts.size() <= idx) {
      rubyTexts.resize(words.size());
    }
    rubyTexts[idx] = "";
    wordStyles[idx] =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(wordStyles[idx]) | EpdFontFamily::RUBY_CONTINUE);
    wordContinues[idx] = true;       // Prevent page breaker from splitting the Group Ruby!
    wordNoSpaceBefore[idx] = false;  // Ensure allowsBreak returns false!
  }
}

void ParsedText::ensureRubyCapacity() {
  // No-op: rubyTexts is a std::deque (chunked growth, no capacity to pre-reserve
  // and no large contiguous reallocation to avoid). Kept for call-site stability.
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& renderer, const int fontId) const {
  if (!isFirstLine || !isNaturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0 || !extraParagraphSpacing) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  if (!extraParagraphSpacing) {
    return renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 3;
  }
  return 0;
}
// Consumes data to minimize memory usage
bool ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return true;
  }

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(words[i].c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (auto s : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    renderer.ensureSdCardFontReady(fontId, words, hyphenationEnabled, styleMask);
  }

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices =
        computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    if (!extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine,
                     renderer, fontId)) {
      return false;
    }
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
    wordFocusBoundary.erase(wordFocusBoundary.begin(), wordFocusBoundary.begin() + consumed);
    wordLinkIds.erase(wordLinkIds.begin(), wordLinkIds.begin() + consumed);
    eraseVisibleOffsetPrefix(consumed);
    if (!rubyTexts.empty()) {
      const size_t rtConsumed = std::min(consumed, rubyTexts.size());
      rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
    }
  }
  return true;
}

static inline bool isCjkIdeograph(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

// The first word of a line may have its ruby characters wider than the word (the base text). In that case, we need to
// move the base text to the right a bit so that ruby text doesn't overflow the left border, and it is still centered
// over the base text. This function calculates how much we need to move the base text to the right.
int ParsedText::calculateRubyExtraStartOffset(const size_t wordIdx, const size_t maxWordIdx,
                                              const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || wordIdx >= rubyTexts.size() || rubyTexts[wordIdx].empty() ||
      (wordStyles[wordIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  size_t groupWordCount = 1;
  while (wordIdx + groupWordCount < maxWordIdx &&
         (wordStyles[wordIdx + groupWordCount] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    groupWordCount++;
  }
  int groupActualWidth = 0;
  for (size_t k = 0; k < groupWordCount; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, words[wordIdx + k], wordStyles[wordIdx + k]);
  }
  const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[wordIdx].c_str(), EpdFontFamily::SUP);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  const int leftOverlap = (rubyWidth - groupActualWidth) / 2;

  // This function is only ever called for the first word of a line.
  // words[wordIdx - 1], if it exists, is always the last word of the *prior* line
  // and cannot absorb any left overhang on the current line.
  // The full leftOverlap must therefore be reserved as a visual indent so the
  // ruby text does not overflow the left margin.
  return leftOverlap;
}

// The last ruby group on a line may have its ruby characters wider than the group's base text.
// The right half of that overhang protrudes past the last base character. This function returns
// the amount of right-margin space that must be reserved so the ruby does not overflow the right
// border. It mirrors calculateRubyExtraStartOffset: words[lineBreak] is on the *next* line and
// cannot absorb any of the right overhang on the current line, so the full rightOverlap is returned.
int ParsedText::calculateRubyExtraEndOffset(const size_t lineStartIdx, const size_t lineBreakIdx,
                                            const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || lineBreakIdx == 0 || lineStartIdx >= lineBreakIdx) {
    return 0;
  }

  // Walk backwards from the last word to find the leader of the last ruby group on the line.
  size_t leaderIdx = lineBreakIdx - 1;
  while (leaderIdx > lineStartIdx && (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    leaderIdx--;
  }

  // leaderIdx must be a ruby group leader (non-empty ruby, no RUBY_CONTINUE flag).
  if (leaderIdx >= rubyTexts.size() || rubyTexts[leaderIdx].empty() ||
      (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  // Measure the group.
  int groupActualWidth = 0;
  for (size_t k = leaderIdx; k < lineBreakIdx; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, words[k], wordStyles[k]);
  }
  const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[leaderIdx].c_str(), EpdFontFamily::SUP);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  return (rubyWidth - groupActualWidth) / 2;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureFocusWordWidth(renderer, fontId, words[i], wordStyles[i], wordFocusBoundary[i]));
  }

  // Adjust widths for ruby groups to comply with JLReq standards
  if (!rubyTexts.empty()) {
    struct RubyGroupInfo {
      size_t start;
      size_t count;
      int baseWidth;
      int rubyWidth;
      int leftOverlap;
      int rightOverlap;
    };

    std::vector<RubyGroupInfo> groups;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) == 0) {
        RubyGroupInfo g;
        g.start = i;
        g.baseWidth = wordWidths[i];
        g.count = 1;
        while (i + g.count < words.size() && (wordStyles[i + g.count] & EpdFontFamily::RUBY_CONTINUE) != 0) {
          g.baseWidth += wordWidths[i + g.count];
          g.count++;
        }
        g.rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
        g.leftOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        g.rightOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        groups.push_back(g);
        i += g.count - 1;
      }
    }

    // Adjust widths based on adjacent characters and group-to-group spacing
    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
      const auto& g = groups[gIdx];

      // 1. Preceding character (left overhang)
      if (g.start > 0) {
        const uint32_t cpPrev = lastCodepoint(words[g.start - 1]);
        if (isCjkIdeograph(cpPrev)) {
          wordWidths[g.start - 1] += g.leftOverlap;
        } else {
          const int maxLeftOverhang = wordWidths[g.start - 1] / 2;
          wordWidths[g.start - 1] += std::max(0, g.leftOverlap - maxLeftOverhang);
        }
      }

      // 2. Succeeding character (right overhang / group collision)
      const size_t nextIdx = g.start + g.count;
      if (nextIdx < words.size()) {
        if (gIdx + 1 < groups.size() && groups[gIdx + 1].start == nextIdx) {
          // Adjacent ruby groups: compute collision
          const auto& nextG = groups[gIdx + 1];
          const int collision = g.rightOverlap + nextG.leftOverlap;
          if (collision > 0) {
            wordWidths[g.start + g.count - 1] += collision;
          }
        } else {
          // Regular character following: check if it's Kanji
          const uint32_t cpNext = firstCodepoint(words[nextIdx]);
          if (isCjkIdeograph(cpNext)) {
            wordWidths[g.start + g.count - 1] += g.rightOverlap;
          } else {
            const int maxRightOverhang = wordWidths[nextIdx] / 2;
            wordWidths[g.start + g.count - 1] += std::max(0, g.rightOverlap - maxRightOverhang);
          }

          // Check if there is another ruby group further ahead separated only by non-ideographs
          if (gIdx + 1 < groups.size()) {
            const auto& nextG = groups[gIdx + 1];
            bool onlyNonIdeographsInBetween = true;
            int gapWidth = 0;
            for (size_t k = nextIdx; k < nextG.start; ++k) {
              const uint32_t cp = firstCodepoint(words[k]);
              if (isCjkIdeograph(cp)) {
                onlyNonIdeographsInBetween = false;
                break;
              }
              gapWidth += wordWidths[k];
            }
            if (onlyNonIdeographsInBetween) {
              const int maxRightOverhang = wordWidths[g.start + g.count - 1] / 2;
              const int maxLeftOverhang = wordWidths[nextG.start - 1] / 2;
              const int allowedRight = std::min(g.rightOverlap, maxRightOverhang);
              const int allowedLeft = std::min(nextG.leftOverlap, maxLeftOverhang);
              const int touchOverlap = allowedRight + allowedLeft - gapWidth;
              if (touchOverlap > 0) {
                wordWidths[g.start + g.count - 1] += touchOverlap;
              }
            }
          }
        }
      }
    }
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Attached and breakable-attached boundaries both use kerning when kept on one line.
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && noSpaceBeforeVec[j]) {
        gap = 0;
      } else if (j > static_cast<size_t>(i)) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }

      // Calculate extraStartOffset for the first word on the line (i) (protect left margin)
      const int extraStartOffset = (j == i) ? calculateRubyExtraStartOffset(i, totalWordCount, renderer, fontId) : 0;

      currlen += wordWidths[j] + gap + (j == i ? extraStartOffset : 0);

      if (currlen > effectivePageWidth) {
        break;
      }

      // A normal continuation is unbreakable. continues=true + noSpaceBefore=true is the compact
      // breakable-attachment state used after explicit hyphen characters.
      if (j + 1 < totalWordCount && !TokenBoundary::allowsBreak(continuesVec[j + 1], noSpaceBeforeVec[j + 1])) {
        continue;
      }

      const int extraEndOffset = calculateRubyExtraEndOffset(i, j + 1, renderer, fontId);

      if (currlen + extraEndOffset > effectivePageWidth) {
        continue;  // Cannot split here as it would overflow the right margin
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      // Favor longer lines when line-breaking costs are equal, to avoid unnecessary short lines in Chinese and Japanese
      // text.
      if (cost <= dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec,
                                                            std::vector<bool>& noSpaceBeforeVec) {
  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && continuesVec[currentIndex]) {
        // Attached and breakable-attached boundaries both use kerning when kept on one line.
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && noSpaceBeforeVec[currentIndex]) {
        spacing = 0;
      } else if (!isFirstWord) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() &&
           !TokenBoundary::allowsBreak(continuesVec[currentIndex], noSpaceBeforeVec[currentIndex])) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  const uint8_t focusBoundary = wordFocusBoundary[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements). Focus emphasis is a byte
  // annotation, so the hyphenator sees the whole word and every legal break is reachable.
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureFocusWordWidth(renderer, fontId, word.substr(0, offset), style,
                                                  focusBoundaryBefore(focusBoundary, offset), needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  uint32_t remainderOffset = visibleOffsetAt(wordIndex);
  const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* splitPtr = offsetPtr + chosenOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    remainderOffset++;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  // Emphasis follows the text across the split, so a break at or after the boundary leaves the
  // remainder fully regular.
  wordFocusBoundary.insert(wordFocusBoundary.begin() + wordIndex + 1, focusBoundaryAfter(focusBoundary, chosenOffset));
  wordLinkIds.insert(wordLinkIds.begin() + wordIndex + 1, wordLinkIds[wordIndex]);
  wordFocusBoundary[wordIndex] = focusBoundaryBefore(focusBoundary, chosenOffset);
  // Invariant: a boundary is always strictly inside its token, so an all-bold part carries BOLD in
  // its style with boundary 0 and nothing downstream special-cases boundary == size.
  if (wordFocusBoundary[wordIndex] >= words[wordIndex].size()) {
    wordStyles[wordIndex] = static_cast<EpdFontFamily::Style>(wordStyles[wordIndex] | EpdFontFamily::BOLD);
    wordFocusBoundary[wordIndex] = 0;
  }
  if (wordIndex + 1 <= rubyTexts.size()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth =
      measureFocusWordWidth(renderer, fontId, remainder, style, wordFocusBoundary[wordIndex + 1]);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

bool ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::unique_ptr<TextBlock>, uint32_t)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const uint32_t lineVisibleOffset = visibleOffsetAt(lastBreakAt);

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0, renderer, fontId);

  std::vector<std::string> lineRubyTexts(lineWordCount);
  if (!rubyTexts.empty() && lastBreakAt < rubyTexts.size()) {
    const size_t copyCount = std::min(lineBreak, rubyTexts.size()) - lastBreakAt;
    std::copy(rubyTexts.begin() + lastBreakAt, rubyTexts.begin() + lastBreakAt + copyCount, lineRubyTexts.begin());
  }

  const int extraStartOffset = calculateRubyExtraStartOffset(lastBreakAt, lineBreak, renderer, fontId);
  const int extraEndOffset = calculateRubyExtraEndOffset(lastBreakAt, lineBreak, renderer, fontId);

  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    std::string word = std::move(words[lastBreakAt + i]);
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    if (wordIdx == 0) continue;
    const size_t boundaryIdx = lastBreakAt + wordIdx;
    const bool isSpaceToken = lineWords[wordIdx] == " ";
    if (TokenBoundary::isJustifiableGap(continuesVec[boundaryIdx], noSpaceBeforeVec[boundaryIdx], isSpaceToken)) {
      actualGapCount++;
    }
    if (continuesVec[boundaryIdx]) {
      totalNaturalGaps += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    } else if (!noSpaceBeforeVec[boundaryIdx]) {
      totalNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                                   firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // extraEndOffset reserves space for any ruby group at the right edge of the line.
  const int spareSpace = effectivePageWidth - extraStartOffset - extraEndOffset - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                               ? computeJustifyExtra(spareSpace, actualGapCount)
                               : 0;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusBoundaryScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusBoundaryScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusBoundaryScratch.push_back(wordFocusBoundary[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx > 0 && reorderedNoSpaceBeforeScratch[wordIdx]) {
        // Unicode break opportunity with no inserted Latin-style space. It is still
        // a stretchable gap for justified CJK/Korean text.
        reorderedGapCount++;
      } else if (wordIdx > 0 && !reorderedContinuesScratch[wordIdx]) {
        reorderedGapCount++;
        reorderedNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                         reorderedStylesScratch[wordIdx - 1]);
      } else if (wordIdx > 0 && reorderedContinuesScratch[wordIdx]) {
        if (reorderedWordsScratch[wordIdx] == " ") {
          reorderedGapCount++;
        }
        reorderedNaturalGaps +=
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                firstCodepoint(reorderedWordsScratch[wordIdx]), reorderedStylesScratch[wordIdx - 1]);
      }
    }

    const int reorderedSpare =
        effectivePageWidth - extraStartOffset - extraEndOffset - reorderedWordWidthSum - reorderedNaturalGaps;
    const int reorderedJustifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                          ? computeJustifyExtra(reorderedSpare, reorderedGapCount)
                                          : 0;

    const int justifyContribution = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                        ? reorderedJustifyExtra * static_cast<int>(reorderedGapCount)
                                        : 0;
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifyContribution;

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      lineXPos.push_back(static_cast<int16_t>(xpos));
      xpos += reorderedWidthsScratch[wordIdx];

      const bool nextIsContinuation =
          wordIdx + 1 < reorderedWidthsScratch.size() && reorderedContinuesScratch[wordIdx + 1];
      if (nextIsContinuation) {
        int advance =
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                firstCodepoint(reorderedWordsScratch[wordIdx + 1]), reorderedStylesScratch[wordIdx]);
        // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
        // no-break space must not receive justifyExtra, or the line over-stretches by one
        // gap and the last word is pushed past the right margin (issue #2185).
        if (wordIdx > 0 && reorderedWordsScratch[wordIdx] == " " && reorderedContinuesScratch[wordIdx] &&
            effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          advance += reorderedJustifyExtra;
        }
        xpos += advance;
      } else if (wordIdx + 1 < reorderedWidthsScratch.size()) {
        const bool nextNoSpace = reorderedNoSpaceBeforeScratch[wordIdx + 1];
        int gap = nextNoSpace ? 0
                              : renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                                         reorderedStylesScratch[wordIdx]);
        if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          gap += reorderedJustifyExtra;
        }
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
  } else {
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          // Cross-boundary kerning for continuation words
          int advance = renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                            firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0: see the LTR branch — a leading no-break space is not a justifiable gap.
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos -= advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace
                      ? 0
                      : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                 firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent + extraStartOffset;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          int advance = wordWidths[lastBreakAt + wordIdx];
          advance += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                         firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
          // no-break space must not receive justifyExtra, or the line over-stretches by one
          // gap and the last word is pushed past the right margin (issue #2185).
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos += advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace
                      ? 0
                      : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                 firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos += wordWidths[lastBreakAt + wordIdx] + gap;
        }
      }
    }
  }

  const auto focusBoundaryAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusBoundaryScratch[idx] : wordFocusBoundary[lastBreakAt + idx];
  };

  std::vector<TextBlock::LinkSpan> lineLinks;
  std::vector<uint8_t> lineLinkIdsSeen;
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t linkId = wordLinkIds[lastBreakAt + (willReorder ? visualOrderScratch[i] : i)];
    if (linkId == 0 || linkId > linkTargets.size()) continue;

    size_t spanIndex = 0;
    while (spanIndex < lineLinkIdsSeen.size() && lineLinkIdsSeen[spanIndex] != linkId) spanIndex++;
    int width = willReorder ? reorderedWidthsScratch[i] : wordWidths[lastBreakAt + i];
    const int right = lineXPos[i] + width;
    const int topLift =
        (lineWordStyles[i] & EpdFontFamily::SUP) != 0 ? renderer.getFontAscenderSize(fontId) * 2 / 5 : 0;

    if (spanIndex == lineLinkIdsSeen.size()) {
      lineLinks.emplace_back();
      auto& span = lineLinks.back();
      strncpy(span.href, linkTargets[linkId - 1].c_str(), sizeof(span.href) - 1);
      span.href[sizeof(span.href) - 1] = '\0';
      span.x = lineXPos[i];
      span.width = static_cast<int16_t>(width);
      span.topLift = static_cast<int16_t>(topLift);
      lineLinkIdsSeen.push_back(linkId);
    } else {
      auto& span = lineLinks[spanIndex];
      const int left = std::min<int>(span.x, lineXPos[i]);
      const int mergedRight = std::max<int>(span.x + span.width, right);
      span.x = static_cast<int16_t>(left);
      span.width = static_cast<int16_t>(mergedRight - left);
      span.topLift = std::max<int16_t>(span.topLift, static_cast<int16_t>(topLift));
    }
  }

  // Fast path: no word on this line carries focus emphasis, so pass empty boundary/suffixX
  // vectors. TextBlock pays zero per-word RAM cost for these annotations when they are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (focusBoundaryAt(i) != 0) {
      lineHasFocusSplit = true;
      break;
    }
  }

  if (!lineHasFocusSplit) {
    // TextBlock flattens the vectors into its arena; they stay owned here and die at return.
    auto block = makeUniqueNoThrow<TextBlock>(lineWords, lineXPos, lineWordStyles, std::vector<uint8_t>{},
                                              std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts),
                                              std::move(lineLinks));
    if (!block || !block->valid()) {
      LOG_ERR("PTX", "Layout failed: TextBlock or arena allocation failed");
      return false;
    }
    processLine(std::move(block), lineVisibleOffset);
    return true;
  }

  // Each word is one TextBlock entry carrying its own boundary; all that remains is the suffix x
  // offset the renderer needs to resume in regular weight, i.e. the bold prefix's advance.
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSuffixX;
  outBoundaries.reserve(lineWordCount);
  outSuffixX.reserve(lineWordCount);
  for (size_t i = 0; i < lineWordCount; i++) {
    const uint8_t boundary = focusBoundaryAt(i);
    outBoundaries.push_back(boundary);
    outSuffixX.push_back(
        boundary == 0 ? 0 : measureFocusPrefixAdvance(renderer, fontId, lineWords[i], lineWordStyles[i], boundary));
  }

  auto block = makeUniqueNoThrow<TextBlock>(lineWords, lineXPos, lineWordStyles, outBoundaries, outSuffixX, blockStyle,
                                            std::move(lineRubyTexts), std::move(lineLinks));
  if (!block || !block->valid()) {
    LOG_ERR("PTX", "Layout failed: TextBlock or arena allocation failed");
    return false;
  }
  processLine(std::move(block), lineVisibleOffset);
  return true;
}
#endif
