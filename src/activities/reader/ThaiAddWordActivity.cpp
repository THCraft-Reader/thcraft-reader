#include "ThaiAddWordActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <ThaiUserWords.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ThaiUserWordStore.h"
#include "activities/ActivityResult.h"

namespace {
bool isUserWord(std::string_view text) {
  uint8_t symbols[thai::MAX_DICTIONARY_WORD_CODEPOINTS];
  return thai::encodeUserWord(text, symbols, sizeof(symbols)) != 0;
}
}  // namespace

ThaiAddWordActivity::ThaiAddWordActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::unique_ptr<Page> page, const int marginLeft, const int marginTop,
                                         std::string selection)
    : Activity("ThaiAddWord", renderer, mappedInput),
      page(std::move(page)),
      marginLeft(marginLeft),
      marginTop(marginTop),
      // Selections snap to whole segments, so drop the spaces and punctuation
      // they bring along; what remains must be one Thai word.
      candidate(thai::trimToUserWord(selection)) {}

void ThaiAddWordActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  RenderLock lock;
  if (isUserWord(candidate)) {
    showSheet();
  } else {
    showMessage(StrId::STR_THAI_WORD_INVALID);
  }
}

void ThaiAddWordActivity::showSheet() {
  // Trimming removes one whole cluster; offer it only when a valid word remains.
  const size_t startTrim = thai::firstClusterEnd(candidate);
  const size_t endTrim = thai::lastClusterStart(candidate);
  const char* labels[std::tuple_size_v<decltype(options)>];
  int count = 0;
  const auto add = [&](const Option option, const char* label) {
    options[count] = option;
    labels[count++] = label;
  };
  add(Option::Add, tr(STR_ADD_WORD));
  if (startTrim && isUserWord(std::string_view(candidate).substr(startTrim)))
    add(Option::TrimStart, tr(STR_TRIM_START));
  if (endTrim && isUserWord(std::string_view(candidate).substr(0, endTrim))) add(Option::TrimEnd, tr(STR_TRIM_END));
  add(Option::Cancel, tr(STR_CANCEL));
  popup.show(tr(STR_THAI_WORDS), candidate.c_str(), labels, count, 0, [this](const int index) { onOption(index); });
  redrawPage = true;
  requestUpdate();
}

void ThaiAddWordActivity::showMessage(const StrId message) {
  static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
  popup.show(message, OPTIONS, 1, 0, [this](int) { step = Step::Close; });
  redrawPage = true;
  requestUpdate();
}

void ThaiAddWordActivity::onOption(const int index) {
  switch (options[index]) {
    case Option::Add:
      step = Step::Commit;
      break;
    case Option::TrimStart:
      candidate.erase(0, thai::firstClusterEnd(candidate));
      step = Step::ShowSheet;
      break;
    case Option::TrimEnd:
      candidate.resize(thai::lastClusterStart(candidate));
      step = Step::ShowSheet;
      break;
    case Option::Cancel:
      step = Step::Close;
      break;
  }
}

void ThaiAddWordActivity::commit() {
  StrId message = StrId::STR_THAI_WORD_INVALID;
  switch (THAI_WORDS.add(candidate)) {
    case ThaiUserWordStore::AddResult::Added:
      close(true);
      return;
    case ThaiUserWordStore::AddResult::Duplicate:
      message = StrId::STR_THAI_WORD_DUPLICATE;
      break;
    case ThaiUserWordStore::AddResult::KnownWord:
      message = StrId::STR_THAI_WORD_KNOWN;
      break;
    case ThaiUserWordStore::AddResult::Full:
      message = StrId::STR_THAI_WORD_LIST_FULL;
      break;
    case ThaiUserWordStore::AddResult::StorageError:
      message = StrId::STR_THAI_WORD_SAVE_FAILED;
      break;
    case ThaiUserWordStore::AddResult::OutOfMemory:
      message = StrId::STR_MEMORY_ERROR;
      break;
    case ThaiUserWordStore::AddResult::Invalid:
      break;
  }
  RenderLock lock;
  showMessage(message);
}

void ThaiAddWordActivity::close(const bool added) {
  ActivityResult result;
  result.isCancelled = !added;
  setResult(std::move(result));
  finish();
}

void ThaiAddWordActivity::loop() {
  // Runs without the render lock, which THAI_WORDS.add() takes to install the list.
  const Step pending = step;
  step = Step::None;
  switch (pending) {
    case Step::ShowSheet: {
      RenderLock lock;
      showSheet();
      return;
    }
    case Step::Commit:
      commit();
      return;
    case Step::Close:
      close(false);
      return;
    case Step::None:
      break;
  }
  if (popup.handleInput(mappedInput, [this] {
        if (popup.isActive()) requestUpdate();
      })) {
    return;
  }
  // Dismissed without a choice (Back or a tap outside the sheet).
  if (!popup.isActive() && step == Step::None) close(false);
}

void ThaiAddWordActivity::render(RenderLock&&) {
  // A new sheet starts from the page, so a smaller sheet never leaves an old one
  // behind; highlight moves only redraw the sheet over the frame.
  if (redrawPage) {
    redrawPage = false;
    renderer.clearScreen();
    if (page) {
      auto scope = renderer.getFontCacheManager()->createPrewarmScope();
      page->render(renderer, fontId, marginLeft, marginTop);
      scope.endScanAndPrewarm();
      page->render(renderer, fontId, marginLeft, marginTop);
    }
  }
  if (popup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
