#include "DictionaryDefinitionActivity.h"

#if defined(CROSSPOINT_NATIVE_TEXT)
#include <Memory.h>
#include <NativeParagraphLayout.h>
#include <NativeTextEngine.h>
#else
#include <FontCacheManager.h>
#endif
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace {

#if !defined(CROSSPOINT_NATIVE_TEXT)
// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;
#endif

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Limit HTML parser work. Large definitions use the plain-text adapter
// (shared shaped Pages on native; retained byte spans on legacy).
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  renderer.clearTextStatus();
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  const bool styled = htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages();
#if defined(CROSSPOINT_NATIVE_TEXT)
  nativeLayoutStatus = renderer.lastTextStatus();
  if (!styled && nativeLayoutStatus == TextStatus::Ok) {
    definition = htmlToPlainText(definition);
    nativeLayoutStatus = layoutNativeText();
  }
  if (nativeLayoutStatus != TextStatus::Ok) {
    LOG_ERR("TEXT", "Dictionary text layout failed (%u)", static_cast<unsigned>(nativeLayoutStatus));
    pages.clear();
  }
#else
  if (!styled) {
    definition = htmlToPlainText(definition);
    wrapText();
  }
#endif
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (auto* engine = renderer.nativeTextEngine()) engine->clearCaches();
#else
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
#endif
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int topArea = (isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding + metrics.headerHeight;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  return {renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING,
          renderer.getScreenHeight() - topArea - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

#if defined(CROSSPOINT_NATIVE_TEXT)
TextStatus DictionaryDefinitionActivity::layoutNativeText() {
  pages.clear();
  currentPage = 0;
  totalPages = 1;
  auto* engine = renderer.nativeTextEngine();
  if (!engine) return TextStatus::InvalidFont;
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.width > UINT16_MAX || body.height <= 0 || body.height > INT16_MAX ||
      definition.size() > UINT32_MAX)
    return TextStatus::CapacityExceeded;
  const int fontId = SETTINGS.getReaderFontId();
  const int nominalHeight = renderer.getLineHeight(fontId);
  if (renderer.lastTextStatus() != TextStatus::Ok) return renderer.lastTextStatus();
  if (nominalHeight <= 0 || nominalHeight > INT16_MAX) return TextStatus::CapacityExceeded;

  struct PageContext {
    GfxRenderer& renderer;
    std::vector<std::unique_ptr<Page>>& pages;
    int fontId;
    int viewportHeight;
    int y = 0;
    std::unique_ptr<Page> pending;
  } context{renderer, pages, fontId, body.height};
  const auto emit = [](void* raw, NativeLayoutEmission&& emission) -> TextStatus {
    auto& ctx = *static_cast<PageContext*>(raw);
    BlockStyle style;
    style.alignment = emission.line.paragraphLevel & 1 ? CssTextAlign::Right : CssTextAlign::Left;
    auto block = makeUniqueNoThrow<TextBlock>(std::move(emission.line), style);
    if (!block || !block->valid()) return TextStatus::OutOfMemory;
    const int height = block->layoutHeight(ctx.renderer, ctx.fontId, 1.0f);
    if (ctx.renderer.lastTextStatus() != TextStatus::Ok) return ctx.renderer.lastTextStatus();
    if (height <= 0) return TextStatus::InvalidText;
    if (ctx.pending && ctx.y > 0 && height > ctx.viewportHeight - ctx.y) {
      if (ctx.pages.size() >= INT32_MAX) return TextStatus::CapacityExceeded;
      ctx.pages.push_back(std::move(ctx.pending));
      ctx.y = 0;
    }
    if (!ctx.pending) {
      ctx.pending = makeUniqueNoThrow<Page>();
      if (!ctx.pending) return TextStatus::OutOfMemory;
    }
    if (ctx.pending->elements.size() >= Page::MAX_ELEMENTS_PER_PAGE) return TextStatus::CapacityExceeded;
    auto line = makeUniqueNoThrow<PageLine>(std::move(block), 0, static_cast<int16_t>(ctx.y));
    if (!line) return TextStatus::OutOfMemory;
    if (!ctx.pending->addElement(std::move(line))) return TextStatus::OutOfMemory;
    // An over-tall first line is consumed once and clipped by drawBody().
    ctx.y += height;
    return TextStatus::Ok;
  };

  NativeParagraphLayout fitter(*engine);
  NativeLayoutOptions options;
  options.fontId = fontId;
  options.width = static_cast<uint16_t>(body.width);
  options.alignment = NativeAlignment::Start;
  const std::string_view source(definition);
  size_t paragraphStart = 0;
  while (paragraphStart < source.size()) {
    const size_t newline = source.find('\n', paragraphStart);
    const size_t paragraphEnd = newline == std::string_view::npos ? source.size() : newline;
    auto paragraph = source.substr(paragraphStart, paragraphEnd - paragraphStart);
    if (!paragraph.empty() && paragraph.back() == '\r') paragraph.remove_suffix(1);
    if (paragraph.empty()) {
      NativeLayoutEmission blank;
      blank.line.lineHeight = static_cast<int16_t>(nominalHeight);
      const auto status = emit(&context, std::move(blank));
      if (status != TextStatus::Ok) return status;
    } else {
      size_t offset = 0;
      int8_t paragraphLevel = -1;
      options.firstLine = true;
      while (offset < paragraph.size()) {
        const auto remaining = paragraph.substr(offset);
        size_t bytes = 0;
        auto status = NativeParagraphLayout::windowPrefix(remaining, bytes, true);
        if (status != TextStatus::Ok) return status;
        if (bytes == 0) return TextStatus::CapacityExceeded;
        NativeParagraphView view;
        view.text = remaining.substr(0, bytes);
        view.sourceStart = static_cast<uint32_t>(paragraphStart + offset);
        view.sourceUnit = NativeSourceUnit::Byte;
        view.paragraphLevel = paragraphLevel;
        view.final = bytes == remaining.size();
        size_t consumed = 0;
        status = fitter.layout(view, options, emit, &context, consumed, paragraphLevel);
        if (status != TextStatus::Ok) return status;
        if (consumed == 0 || consumed > bytes) return TextStatus::CapacityExceeded;
        offset += consumed;
        options.firstLine = false;
      }
    }
    if (newline == std::string_view::npos) break;
    paragraphStart = newline + 1;
  }
  if (context.pending) {
    if (pages.size() >= INT32_MAX) return TextStatus::CapacityExceeded;
    pages.push_back(std::move(context.pending));
  }
  totalPages = std::max(1, static_cast<int>(pages.size()));
  definition.clear();
  definition.shrink_to_fit();
  return TextStatus::Ok;
}
#else
int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}
#endif

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Native and styled definitions paint Pages. Only legacy plain definitions
// use byte spans; the legacy cache scan also calls this before real painting.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
#if defined(CROSSPOINT_NATIVE_TEXT)
    int clipX, clipY, clipWidth, clipHeight;
    renderer.getClipRect(clipX, clipY, clipWidth, clipHeight);
    const auto body = bodyArea();
    const int left = std::max(x, clipX);
    const int top = std::max(startY, clipY);
    renderer.setClipRect(left, top, std::max(0, std::min(x + body.width, clipX + clipWidth) - left),
                         std::max(0, std::min(startY + body.height, clipY + clipHeight) - top));
#endif
    pages[currentPage]->render(renderer, fontId, x, startY);
#if defined(CROSSPOINT_NATIVE_TEXT)
    renderer.setClipRect(clipX, clipY, clipWidth, clipHeight);
#endif
    return;
  }
#if !defined(CROSSPOINT_NATIVE_TEXT)
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf);
  }
#endif
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (nativeLayoutStatus != TextStatus::Ok) {
    if (auto* engine = renderer.nativeTextEngine()) engine->clearCaches();
    GUI.drawPopup(renderer,
                  nativeLayoutStatus == TextStatus::OutOfMemory ? tr(STR_MEMORY_ERROR) : tr(STR_TEXT_RENDER_ERROR));
    return;
  }
#endif

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  // Header: matched headword left, page counter right.
  const int headerY = contentY + metrics.topPadding + 10;
  renderer.drawText(UI_12_FONT_ID, contentX + SIDE_PADDING, headerY, headword.c_str(), true, EpdFontFamily::BOLD);
  if (totalPages > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - SIDE_PADDING - counterWidth, headerY, counter);
  }

  // Warm real native glyphs without a framebuffer scan; legacy SD fonts keep
  // their scan/prewarm/draw transaction.
  const int fontId = SETTINGS.getReaderFontId();
  const int bodyStartY = contentY + metrics.topPadding + metrics.headerHeight;
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (!pages.empty() && !pages[currentPage]->warmNativeText(renderer, fontId)) return;
#else
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
#endif
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (renderer.lastTextStatus() != TextStatus::Ok) return;
#endif
  renderer.displayBuffer();
}
