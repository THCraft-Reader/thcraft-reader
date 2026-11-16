#pragma once

#include <ThaiUserWords.h>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Settings > Reader > Thai Word List: the saved words, newest first.
// Selecting a word offers Delete or Delete All.
class ThaiWordsActivity final : public UiListActivity {
 public:
  ThaiWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void render(RenderLock&&) override;

 private:
  // Popup choices are applied from loop(), not inside the popup's callback,
  // which must not replace the popup or open a screen while it runs.
  enum class Pending : uint8_t { None, Delete, DeleteAll };

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void showWordOptions();
  void confirmDeleteAll();
  void afterDelete(bool removed);

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;

  // Formatted on demand for each drawn row (render task only).
  char rowWord[thai::MAX_DICTIONARY_WORD_CODEPOINTS * 3 + 1] = {};
  char countText[48] = {};
  OptionPopup popup;
  Pending pending = Pending::None;
};
