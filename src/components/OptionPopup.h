#pragma once
#include <BoardConfig.h>
#include <I18n.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

// Modal option picker drawn over the current screen (no clear) via
// fui::optionDialog. Touch hit-testing is the SDK's InteractionBuffer: each
// render registers the option buttons (plus a chrome guard rect) on the render
// task, and handleInput routes touch snapshots against that table on the loop
// task, gated by the uiReady handshake (same pattern as UiListActivity).
// render() builds into InteractionBuffer's non-published generation
// (beginPublishCycle()) and publishes it only once every hit() call for the
// frame is done (publish()), so handleInput()'s routePublished()/
// publishedData() reads on the loop task always see a complete table, never
// one render is mid-rebuilding. uiReady closes when show() replaces the
// popup's data, then stays open across ordinary repaints after the first
// publication so a release cannot be dropped during a highlight repaint.
class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    headline.clear();
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    activate(currentIndex, std::move(onSelect));
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    headline.clear();
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    activate(currentIndex, std::move(onSelect));
  }

  // As above, plus a subject line inside the dialog (a book or event title).
  // It wraps to several lines under the caption; the dialog grows to fit.
  void show(const char* titleStr, const char* headlineStr, const char* const* options, int optionCount,
            int currentIndex, std::function<void(int)> onSelect) {
    show(titleStr, options, optionCount, currentIndex, std::move(onSelect));
    headline = headlineStr ? headlineStr : "";
  }

  // Message dialog: a wrapped body under the (optional) title, like the
  // Wi-Fi forget-network prompt. Pass an empty title for a message-only look.
  void showMessage(const char* titleStr, const char* messageStr, const char* const* options, int optionCount,
                   int currentIndex, std::function<void(int)> onSelect) {
    show(titleStr, options, optionCount, currentIndex, std::move(onSelect));
    message = messageStr ? messageStr : "";
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    headline.clear();
    ownedStrings = options;
    activate(currentIndex, std::move(onSelect));
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const freeink::ui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased || snap.touchHeld) {
      // Interactions are registered on the render task; only route once the
      // first render after show() has populated the table (uiReady handshake).
      if (uiReady) {
        const freeink::ui::ActionEvent event = interactions.routePublished(snap);
        if (event && event.action == ACTION_OPTION) {
          // Tap released on an option: select it, fire, dismiss.
          selectedIndex = event.value;
          active = false;
          if (onSelectCallback) onSelectCallback(selectedIndex);
          haptic_feedback::touchAction();
          requestUpdate();
          return true;
        }
        if (event && event.action == ACTION_CHROME) {
          // Taps on the dialog chrome (title, padding) keep the popup open.
          return true;
        }
        if (snap.touchReleased && snap.touchX >= 0) {
          // Tap released outside the dialog: dismiss without firing. Swipe-end
          // releases arrive with -1,-1 coords and fall through (no dismiss).
          active = false;
          haptic_feedback::touchAction();
          requestUpdate();
          return true;
        }
        if (snap.touchReleased) {
          // Routed above to clear the pressed option. A vertical swipe pages a
          // list taller than the screen; swipe up shows later options.
          const auto swipe = input.wasSwipe();
          if ((swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) &&
              scrollPage(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1)) {
            requestUpdate();
          }
          return true;
        }
        if (snap.touchPressed) {
          // Touch-down on an option moves the highlight (route() latched the
          // hit as the active interaction; read it back, no re-hit-testing).
          const int16_t idx = interactions.activeIndex();
          if (idx >= 0) {
            const freeink::ui::Interaction& hit = interactions.publishedData()[idx];
            if (hit.action == ACTION_OPTION && selectedIndex != hit.value) {
              selectedIndex = hit.value;
              requestUpdate();
            }
          }
        }
      }
      return true;
    }

    return handleButtons(input.wasPressed(MappedInputManager::Button::NavPrevious),
                         input.wasPressed(MappedInputManager::Button::NavNext),
                         input.wasReleased(MappedInputManager::Button::Confirm),
                         input.wasReleased(MappedInputManager::Button::Back), requestUpdate);
  }

  bool handleButtons(const bool previous, const bool next, const bool confirm, const bool back,
                     const std::function<void()>& requestUpdate) {
    if (!active) return false;
    const int count = std::min<int>(ownedStrings.size(), MAX_OPTIONS);
    if (previous) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      revealSelection();
      requestUpdate();
      return true;
    } else if (next) {
      selectedIndex = (selectedIndex + 1) % count;
      revealSelection();
      requestUpdate();
      return true;
    } else if (confirm) {
      active = false;
      if (onSelectCallback) onSelectCallback(selectedIndex);
      requestUpdate();
      return true;
    } else if (back) {
      active = false;
      requestUpdate();
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    namespace fui = freeink::ui;

    // Per-render target: a GfxRendererTarget is a renderer reference plus
    // three font ids, so rebuilding it here is trivially cheap and always
    // tracks the live orientation and uiScale fonts; a target held across
    // show() would stale-bind both after a rotation or scale change.
    fui::GfxRendererTarget target = makeUiTarget(renderer);
    const fui::ThemeTokens& theme = refreshSharedUiThemeTokens(target);
    // Frame stores a const DeviceContext&; keep it in a local that outlives
    // the frame (a deviceContext() temporary would dangle).
    const fui::DeviceContext device = target.deviceContext();
    // Routing happens on the loop task against the member buffer; the frame
    // itself never dispatches, so it gets an empty snapshot.
    const fui::InputSnapshot noInput{};

    // Builds into the generation handleInput()'s routePublished()/
    // publishedData() aren't currently reading, so the loop task never sees
    // this table mid-rebuild — see publish() below and
    // InteractionBuffer::beginPublishCycle().
    interactions.beginPublishCycle();
    fui::Frame<INTERACTION_CAPACITY> frame(target, device, noInput, interactions);

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int totalOptions = static_cast<int>(ownedStrings.size());
    const uint8_t count = static_cast<uint8_t>(totalOptions > MAX_OPTIONS ? MAX_OPTIONS : totalOptions);

    fui::DialogOption options[MAX_OPTIONS];
    for (uint8_t i = 0; i < count; ++i) {
      options[i].label = ownedStrings[i].c_str();
      options[i].action = ACTION_OPTION;
      options[i].value = static_cast<int16_t>(i);
      options[i].state = (i == selectedIndex) ? fui::StateFocused : fui::StateNormal;
    }

    fui::OptionDialogProps props;
    props.title = title.empty() ? nullptr : title.c_str();
    props.headline = headline.empty() ? nullptr : headline.c_str();
    if (!message.empty()) {
      props.message = message.c_str();
      props.messageText.font = fui::GfxRendererTarget::FONT_BODY;
      props.messageText.align = fui::TextAlign::Center;
      props.messageText.maxLines = 6;
    }
    props.options = options;
    props.optionCount = count;
    props.verticalOptions = true;
    // Touch only: physical buttons stay on the legacy wrap/confirm path above,
    // so the buffer never competes with it for focus/confirm dispatch.
    props.inputMask = fui::InputTouch;
    props.titleText.font = fui::GfxRendererTarget::FONT_BODY;
    props.titleText.bold = true;
    props.titleText.align = fui::TextAlign::Center;
    // Captions like "Remove from Recent Books?" overflow the narrow portrait
    // dialog in one line; let them wrap and the panel grow.
    props.titleText.maxLines = 2;
    props.headlineText.font = fui::GfxRendererTarget::FONT_BODY;
    props.headlineText.align = fui::TextAlign::Center;
    props.headlineText.maxLines = 3;
    props.buttonText.font = fui::GfxRendererTarget::FONT_BODY;
    const int16_t innerPadding = static_cast<int16_t>(metrics.optionPopupInnerPadding);
    props.padding = fui::Insets{innerPadding, innerPadding, innerPadding, innerPadding};
    props.gap = static_cast<int16_t>(metrics.optionPopupItemSpacing);
    // Rounded invert-fill themes use a black pill, not the default gray focus cursor.
    if (theme.listSelectionStyle == fui::SelectionStyle::InvertFill && theme.listRowRadius > 0) {
      props.buttonStyles = fui::defaultButtonStyles();
      props.buttonStyles.focused = props.buttonStyles.selected;
      fui::setStyleRadius(props.buttonStyles, theme.listRowRadius);
    }
    // defaultPopupStyles() has no border, so opt in using the per-theme frame metrics.
    props.styles = fui::defaultPopupStyles();
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = static_cast<uint8_t>(metrics.popupFrameThickness);
    props.styles.normal.radius = static_cast<uint8_t>(metrics.popupCornerRadius);
    props.styles.selected = props.styles.normal;
    props.styles.focused = props.styles.normal;
    props.styles.active = props.styles.normal;
    props.styles.disabled = props.styles.normal;
    props.buttonHeight =
        fui::clampI16(target.lineHeight(fui::GfxRendererTarget::FONT_BODY) + metrics.optionPopupSelectionVPadding * 2);

    // Fixed fraction of the screen, clamped by the theme's side margins; the
    // old max-text-width sizing is gone, long labels wrap inside the buttons.
    const fui::Rect screen = device.screen();
    const int16_t width =
        fui::clampI16(std::min<int>(screen.width * 3 / 4, screen.width - metrics.optionPopupDialogSideMargin * 2));
    // Too many options for the screen: show the largest window that fits,
    // with the SDK's scroll bar, clear of the bezel and any button hints.
    const ::Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !BoardConfig::hasTouch());
    const int reserve = std::max(
        {metrics.optionPopupDialogSideMargin, safe.y - screen.y, screen.y + screen.height - safe.y - safe.height});
    const int maxHeight = screen.height - 2 * reserve;
    uint8_t shown = count;
    int16_t height = fui::optionDialogHeight(target, props, width);
    while (height > maxHeight && shown > 1) {
      props.visibleOptions = --shown;
      height = fui::optionDialogHeight(target, props, width);
    }
    props.firstOption = static_cast<uint8_t>(std::clamp(firstOption, 0, count - shown));
    shownOptions = shown < count ? shown : 0;
    height = fui::clampI16(height, 0, screen.height);
    const fui::Rect dialogRect = fui::centeredRect(screen, fui::Size{width, height});

    // Chrome guard first, options after: route() scans newest-first, so the
    // option buttons win inside the dialog and the guard absorbs the rest.
    frame.hit(dialogRect, ACTION_CHROME, 0, fui::InputTouch);
    fui::optionDialog(frame, dialogRect, props);
    // Atomically make this generation the one handleInput() reads, now that
    // every hit() call for this frame is done.
    interactions.publish();
    uiReady = true;
  }

  bool isActive() const { return active; }

  // Close without firing the callback (the surface under the popup is going
  // away, e.g. its host screen closes from outside the popup's own input).
  void dismiss() {
    active = false;
    onSelectCallback = nullptr;
  }

 private:
  // A fixed cap keeps the DialogOption array on the stack and the interaction
  // table small. +1 slot for the chrome guard rect.
  static constexpr int MAX_OPTIONS = 16;
  static constexpr size_t INTERACTION_CAPACITY = MAX_OPTIONS + 1;
  static constexpr freeink::ui::ActionId ACTION_OPTION = 1;
  static constexpr freeink::ui::ActionId ACTION_CHROME = 2;

  void activate(int currentIndex, std::function<void(int)> onSelect) {
    const int count = std::min<int>(ownedStrings.size(), MAX_OPTIONS);
    selectedIndex = currentIndex >= 0 && currentIndex < count ? currentIndex : 0;
    // Open with the current value at the top of the window; render clamps it.
    firstOption = selectedIndex;
    shownOptions = 0;
    onSelectCallback = std::move(onSelect);
    message.clear();
    uiReady = false;
    active = count > 0;
  }

  int optionCount() const { return std::min<int>(ownedStrings.size(), MAX_OPTIONS); }

  // Moves the window by one page; false when every option is already shown.
  bool scrollPage(const int direction) {
    const int shown = shownOptions;
    const int last = optionCount() - shown;
    if (shown == 0 || last <= 0) return false;
    const int current = std::clamp(firstOption, 0, last);
    const int first = std::clamp(current + direction * shown, 0, last);
    if (first == current) return false;
    firstOption = first;
    return true;
  }

  // Scrolls the window so button navigation keeps the selection visible.
  void revealSelection() {
    const int shown = shownOptions;
    if (shown == 0) {
      firstOption = selectedIndex;
      return;
    }
    firstOption = std::clamp(firstOption, 0, std::max(0, optionCount() - shown));
    if (selectedIndex < firstOption) firstOption = selectedIndex;
    if (selectedIndex >= firstOption + shown) firstOption = selectedIndex - shown + 1;
  }

  bool active = false;
  std::string title;
  std::string headline;
  std::string message;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  // First option of the scroll window (loop task; render clamps it) and how
  // many options the last render fit, 0 when all fit (render task).
  int firstOption = 0;
  mutable std::atomic<uint8_t> shownOptions{0};
  std::function<void(int)> onSelectCallback;
  // Written by the render task (frame registration), routed by the loop task;
  // uiReady closes the rebuild window exactly like UiListActivity::uiReady.
  mutable freeink::ui::InteractionBuffer<INTERACTION_CAPACITY> interactions;
  mutable std::atomic<bool> uiReady{false};
};
