#include "ThaiWordsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "ThaiUserWordStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

ThaiWordsActivity::ThaiWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("ThaiWords", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

int ThaiWordsActivity::listCount() const { return static_cast<int>(THAI_WORDS.count()); }

const char* ThaiWordsActivity::headerTitle() const { return tr(STR_THAI_WORDS); }

// fui::ListProps::rowProvider: the label stays valid until the next call.
void ThaiWordsActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<ThaiWordsActivity*>(ctx);
  if (!THAI_WORDS.wordAt(index, self->rowWord, sizeof(self->rowWord))) self->rowWord[0] = '\0';
  item.label = self->rowWord;
  item.actionValue = static_cast<int16_t>(index);
}

void ThaiWordsActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showWordOptions();
}

void ThaiWordsActivity::onRowLongPress(const int index) { activateIndex(index); }

void ThaiWordsActivity::showWordOptions() {
  std::string word;
  if (!THAI_WORDS.wordAt(static_cast<size_t>(nav.selected), word)) return;
  const char* options[] = {tr(STR_DELETE), tr(STR_DELETE_ALL), tr(STR_CANCEL)};
  popup.show(tr(STR_THAI_WORDS), word.c_str(), options, 3, 0, [this](const int index) {
    pending = index == 0 ? Pending::Delete : index == 1 ? Pending::DeleteAll : Pending::None;
    requestUpdate();
  });
  requestUpdate();
}

void ThaiWordsActivity::confirmDeleteAll() {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_ALL_WORDS_CONFIRM),
                                                              "", tr(STR_CANCEL), tr(STR_DELETE_ALL));
  if (!confirmation) {
    LOG_ERR("THW", "OOM: delete-all confirmation");
    return;
  }
  // The handler runs after the confirmation closes, outside the render lock.
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (!result.isCancelled) afterDelete(THAI_WORDS.removeAll());
    requestUpdate();
  });
}

void ThaiWordsActivity::afterDelete(const bool removed) {
  if (!removed) {
    LOG_ERR("THW", "Failed to delete Thai word(s)");
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    popup.show(StrId::STR_THAI_WORD_SAVE_FAILED, OPTIONS, 1, 0, [](int) {});
    return;
  }
  // The render task reads the selection while drawing the list.
  RenderLock lock;
  const int total = listCount();
  nav.selected = total == 0 ? 0 : std::min(static_cast<int>(nav.selected), total - 1);
  if (total > 0) nav.follow(total);
}

bool ThaiWordsActivity::handleCustomInput() {
  if (pending != Pending::None) {
    const Pending choice = pending;
    pending = Pending::None;
    if (choice == Pending::Delete) {
      afterDelete(THAI_WORDS.removeAt(static_cast<size_t>(nav.selected)));
      requestUpdate();
    } else {
      confirmDeleteAll();
    }
    return true;
  }
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool ThaiWordsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }
  return false;
}

void ThaiWordsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // The footer shows how to add words while the list is empty, then how full it is.
  const int total = listCount();
  const int helpHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpHeight + metrics.verticalSpacing));
  const char* help = tr(STR_THAI_WORDS_HINT);
  if (total > 0) {
    snprintf(countText, sizeof(countText), tr(STR_THAI_WORDS_COUNT_FMT), static_cast<unsigned>(total),
             static_cast<unsigned>(thai::MAX_USER_WORDS));
    help = countText;
  }
  GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpHeight}, help);

  if (total == 0) {
    screen.centeredText(tr(STR_NO_THAI_WORDS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &ThaiWordsActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(total);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void ThaiWordsActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
