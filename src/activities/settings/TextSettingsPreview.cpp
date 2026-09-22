#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#ifndef CROSSPOINT_NATIVE_TEXT
#include <FontCacheManager.h>
#else
#include <NativeUtf8.h>
#endif
#include <GfxRenderer.h>
#include <I18n.h>

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

// Lay the sample text out through the reader engine into layout.lines
bool relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  layout.lines.clear();

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  ParsedText parsed(SETTINGS.extraParagraphSpacing != 0, SETTINGS.hyphenationEnabled != 0,
                    SETTINGS.focusReadingEnabled != 0, style);

  const char* text = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
#ifdef CROSSPOINT_NATIVE_TEXT
  const std::string_view sample(text);
  size_t offset = 0, wordStart = 0;
  uint32_t sourceOffset = 0, wordSourceOffset = 0, cp = 0;
  // UTF-8 byte length bounds the possible one-cluster line count.
  layout.lines.reserve(sample.size() + 1);
  while (offset < sample.size()) {
    const size_t start = offset;
    if (!native_text::nextUtf8(sample, offset, cp)) {
      renderer.recordTextFailure(TextStatus::InvalidText);
      layout.key = {};
      return false;
    }
    if (cp == ' ') {
      if (start > wordStart) {
        parsed.addWord(sample.substr(wordStart, start - wordStart), EpdFontFamily::REGULAR, false, false,
                       wordSourceOffset);
      }
      wordStart = offset;
      wordSourceOffset = sourceOffset + 1;
    }
    ++sourceOffset;
  }
  if (wordStart < sample.size()) {
    parsed.addWord(sample.substr(wordStart), EpdFontFamily::REGULAR, false, false, wordSourceOffset);
  }
#else
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
#endif

  if (!parsed.layoutAndExtractLines(
          renderer, fontId, static_cast<uint16_t>(textWidth),
          [&layout](std::unique_ptr<TextBlock> line, uint32_t) { layout.lines.push_back(std::move(line)); })) {
    layout.lines.clear();
    layout.key = {};
    return false;
  }
#ifdef CROSSPOINT_NATIVE_TEXT
  for (const auto& line : layout.lines) {
    if (!line->warmNativeText(renderer, fontId)) {
      layout.lines.clear();
      layout.key = {};
      return false;
    }
  }
#endif
  return true;
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

  // Include native source/fallback identity, not only the selected logical font ID.
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.fontPointSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineCompression = compression,
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0,
                       .textLayoutFingerprint = renderer.textLayoutFingerprint(fontId)};
  if (key != layout.key) {
#ifndef CROSSPOINT_NATIVE_TEXT
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, I18N.get(StrId::STR_FONT_PREVIEW_TEXT), SETTINGS.focusReadingEnabled ? 0x03 : 0x01);
    }
#endif
    if (!relayout(layout, renderer, fontId, textWidth)) {
#ifdef CROSSPOINT_NATIVE_TEXT
      const char* error =
          renderer.lastTextStatus() == TextStatus::OutOfMemory ? tr(STR_MEMORY_ERROR) : tr(STR_TEXT_RENDER_ERROR);
      renderer.drawText(UI_10_FONT_ID, textLeft, top + previewPadding, error);
#else
      renderer.drawText(UI_10_FONT_ID, textLeft, top + previewPadding, tr(STR_MEMORY_ERROR));
#endif
      return;
    }
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      const int height = line->layoutHeight(renderer, fontId, compression);
      if (y + height > textBottomLimit) return;
      line->render(renderer, fontId, textLeft, y);
      y += height;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
