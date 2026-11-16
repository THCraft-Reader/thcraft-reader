#pragma once

#include <Epub/Page.h>

#include <array>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Adds a selected word to the Thai word list: a sheet over the reading page
// shows the word and offers Add Word, Trim Start, Trim End and Cancel. The result
// is cancelled unless the word was saved.
class ThaiAddWordActivity final : public Activity {
 public:
  ThaiAddWordActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                      int marginLeft, int marginTop, std::string selection);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  enum class Option : uint8_t { Add, TrimStart, TrimEnd, Cancel };
  // Popup choices run from loop(): a callback must not replace its own popup.
  enum class Step : uint8_t { None, ShowSheet, Commit, Close };

  // Callers hold the render lock: the render task reads the popup's strings.
  void showSheet();
  void showMessage(StrId message);
  void onOption(int index);
  void commit();
  void close(bool added);

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  std::string candidate;  // the word being offered; Arduino.h defines word()
  OptionPopup popup;
  std::array<Option, 4> options{};
  Step step = Step::None;
  // A new sheet starts from a fresh page; highlight moves draw over the frame.
  bool redrawPage = true;
};
