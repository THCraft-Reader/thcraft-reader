#pragma once

#include <Epub/Page.h>
#if defined(CROSSPOINT_NATIVE_TEXT)
#include <NativeTextTypes.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Paged dictionary viewer. Native definitions use shared shaped line layout
// and Page payloads; legacy plain definitions retain byte spans.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
#if !defined(CROSSPOINT_NATIVE_TEXT)
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };
#endif

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  BodyArea bodyArea() const;
  bool layoutHtmlPages();
#if defined(CROSSPOINT_NATIVE_TEXT)
  TextStatus layoutNativeText();
#else
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
#endif
  void drawBody(int fontId, int x, int startY) const;

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  // HTML definitions and all native plain definitions use reader-identical Pages.
  std::vector<std::unique_ptr<Page>> pages;
#if defined(CROSSPOINT_NATIVE_TEXT)
  TextStatus nativeLayoutStatus = TextStatus::Ok;
#else
  std::vector<Line> lines;
  int linesPerPage = 1;
#endif
  int currentPage = 0;
  int totalPages = 1;
  ButtonNavigator buttonNavigator;
};
