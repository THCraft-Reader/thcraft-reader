#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <ThaiSegmenter.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace textsettings {

PreviewLayout::PreviewLayout() = default;
PreviewLayout::~PreviewLayout() = default;

namespace {

// Map the paragraph-alignment setting to the engine's CssTextAlign (BOOK_STYLE = justified)
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

const char* previewText() {
  return SETTINGS.paragraphAlignment == CrossPointSettings::THAI_JUSTIFIED ? tr(STR_THAI_JUSTIFY_PREVIEW_TEXT)
                                                                           : tr(STR_FONT_PREVIEW_TEXT);
}

void addThaiPreview(ParsedText& parsed, const std::string_view text) {
  const thai::ThaiDictionary dictionary;
  uint32_t visibleOffset = 0;
  size_t offset = 0;
  const auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (offset < text.size()) {
    if (whitespace(text[offset])) {
      ++offset;
      ++visibleOffset;
      continue;
    }
    const size_t begin = offset;
    while (offset < text.size() && !whitespace(text[offset])) ++offset;
    const auto run = text.substr(begin, offset - begin);
    size_t segmentOffset = 0;
    thai::BreakKind before = thai::BreakKind::Space;
    thai::Segment segment{};
    while (thai::nextSegment(run, segmentOffset, segment, true, dictionary)) {
      before = before == thai::BreakKind::Prohibited || segment.before == thai::BreakKind::Prohibited
                   ? thai::BreakKind::Prohibited
                   : static_cast<thai::BreakKind>(
                         std::min(static_cast<uint8_t>(before), static_cast<uint8_t>(segment.before)));
      const auto piece = run.substr(segment.begin, segment.end - segment.begin);
      if (segment.valid) {
        parsed.addAnalyzedToken(piece, EpdFontFamily::REGULAR, before, visibleOffset, 0);
      } else {
        parsed.addWordWithBoundary(std::string(piece), EpdFontFamily::REGULAR, false, before, visibleOffset, 0);
      }
      for (const unsigned char byte : piece) visibleOffset += (byte & 0xc0) != 0x80;
      before = segment.after;
    }
  }
}

// Lay the sample text out through the reader engine into layout.lines
void relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  layout.lines.clear();

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  ParsedText parsed(SETTINGS.hyphenationEnabled != 0, SETTINGS.focusReadingEnabled != 0, style,
                    SETTINGS.paragraphIndentSpaces);

  const char* text = previewText();
  if (SETTINGS.paragraphAlignment == CrossPointSettings::THAI_JUSTIFIED) {
    addThaiPreview(parsed, text);
  } else {
    // Existing previews retain generic NFC/CJK/RTL/focus tokenization.
    std::string word;
    for (const char* p = text;; p++) {
      if (*p == ' ' || *p == '\0') {
        if (!word.empty()) {
          parsed.addWord(word, EpdFontFamily::REGULAR);
          word.clear();
        }
        if (*p == '\0') break;
      } else {
        word.push_back(*p);
      }
    }
  }

  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(textWidth),
      [&layout](std::unique_ptr<TextBlock> line, uint32_t) { layout.lines.push_back(std::move(line)); }, true,
      SETTINGS.getCharacterSpacing(), SETTINGS.wordSpacing);
}

}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top,
                   int height, const char* familyName, const char* sizeName) {
  const int left = previewPadding;
  const int width = renderer.getScreenWidth() - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelReserved = labelH + labelGap + previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
  const int labelY = top + height - previewPadding - labelH;
  renderer.drawText(UI_10_FONT_ID, left, labelY, labelBuf);

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

  const float compression = SETTINGS.getReaderLineCompression();
  const int lineAdvance = std::max(1, renderer.getLineHeight(fontId, compression));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  // Re-lay-out (and re-prewarm glyphs) only when a layout-affecting setting or the
  // geometry changed; else reuse the cache. The prewarm inputs are (fontId, constant
  // sample text, styleMask<-focusReading), all of which are key fields, so a matching
  // key means an identical prewarm call. This relies on nothing else evicting the SD
  // glyph cache while this activity is up — true today: the only evictor is
  // FontCacheManager::PrewarmScope, used solely by the reader/dictionary activities.
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.fontPointSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .paragraphIndentSpaces = SETTINGS.paragraphIndentSpaces,
                       .characterSpacing = SETTINGS.getCharacterSpacing(),
                       .wordSpacingPercent = SETTINGS.wordSpacing,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, previewText(), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
    relayout(layout, renderer, fontId, textWidth);
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      if (y + lineH > textBottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
