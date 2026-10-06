#include "BookCoverPreparation.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

namespace {
struct CoverRequest {
  int thumbHeight = 0;
  bool sleepCover = false;
  bool showPopup = false;
  bool framebufferChanged = false;
  std::string* sleepPath = nullptr;
};

bool originalSleepThresholds(const GfxRenderer& renderer) {
  return (renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported() ||
          renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported()) &&
         display.getController() == HalDisplay::Controller::SSD1677 &&
         SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
}

void beginDecode(GfxRenderer& renderer, SdCardFontSystem::CoverDecodeScope& fonts, CoverRequest& request) {
  if (request.showPopup) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  fonts.releaseFonts();
  request.framebufferChanged = true;
}

bool prepare(GfxRenderer& renderer, const std::string& bookPath, CoverRequest& request) {
  // Parsers and decoder scratch must die before the selected fonts are restored.
  SdCardFontSystem::CoverDecodeScope fonts(sdFontSystem, renderer);
  if (FsHelpers::hasReflowableBookExtension(bookPath)) {
    // EPUB state exceeds the small task-stack budget; one parser serves both outputs.
    auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
    if (!epub) {
      LOG_ERR("COVER", "OOM: cover EPUB");
      return false;
    }
    const bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
    const bool originalThresholds = originalSleepThresholds(renderer);
    const std::string sleepPath = request.sleepCover ? epub->getCoverBmpPath(cropped, originalThresholds) : "";
    const bool needSleep = request.sleepCover && !Storage.exists(sleepPath.c_str());
    const bool needThumb =
        request.thumbHeight > 0 && !Storage.exists(epub->getThumbBmpPath(request.thumbHeight).c_str());
    if (request.sleepPath) *request.sleepPath = sleepPath;
    if (!needSleep && !needThumb) return true;

    beginDecode(renderer, fonts, request);
    GfxRenderer::FrameBufferLoan loan(renderer);
    bool success = true;
    if (needSleep) {
      // Both outputs reuse cover metadata; CSS and reading layout are not needed.
      if (!epub->load(true, true)) {
        LOG_ERR("COVER", "Failed to load cover metadata: %s", bookPath.c_str());
        return false;
      }
      if (!epub->generateCoverBmp(cropped, originalThresholds)) success = false;
      if (needThumb && !epub->generateThumbBmp(request.thumbHeight)) success = false;
    } else if (needThumb) {
      success = epub->generateThumbBmpFromSource(request.thumbHeight);
    }
    if (!success) LOG_ERR("COVER", "Cover preparation failed: %s", bookPath.c_str());
    return success;
  }

  if (FsHelpers::hasXtcExtension(bookPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("COVER", "OOM: cover XTC");
      return false;
    }
    const std::string sleepPath = request.sleepCover ? xtc->getCoverBmpPath() : "";
    const bool needSleep = request.sleepCover && !Storage.exists(sleepPath.c_str());
    const bool needThumb =
        request.thumbHeight > 0 && !Storage.exists(xtc->getThumbBmpPath(request.thumbHeight).c_str());
    if (request.sleepPath) *request.sleepPath = sleepPath;
    if (!needSleep && !needThumb) return true;

    beginDecode(renderer, fonts, request);
    GfxRenderer::FrameBufferLoan loan(renderer);
    if (!xtc->load()) {
      LOG_ERR("COVER", "Failed to load XTC: %s", bookPath.c_str());
      return false;
    }
    bool success = true;
    if (needSleep && !xtc->generateCoverBmp()) success = false;
    if (needThumb && !xtc->generateThumbBmp(request.thumbHeight)) success = false;
    if (!success) LOG_ERR("COVER", "XTC cover preparation failed: %s", bookPath.c_str());
    return success;
  }
  return false;
}
}  // namespace

bool bookcovers::prepareForReader(GfxRenderer& renderer, const std::string& bookPath) {
  CoverRequest request;
  const auto orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // Grid slot height is known only after its layout; HomeActivity prepares those thumbs.
  if (!UITheme::hasCoverGridHome()) {
    const int themeHeight = GUI.homeCoverThumbHeight(renderer);
    request.thumbHeight = themeHeight > 0 ? themeHeight : UITheme::getInstance().getMetrics().homeCoverHeight;
  }
  request.sleepCover = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
                       SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM;
  request.showPopup = true;
  if (request.thumbHeight > 0 || request.sleepCover) prepare(renderer, bookPath, request);
  renderer.setOrientation(orientation);
  return request.framebufferChanged;
}

bool bookcovers::prepareForSleep(GfxRenderer& renderer, const std::string& bookPath, std::string& coverPath) {
  coverPath.clear();
  CoverRequest request;
  request.sleepCover = true;
  request.sleepPath = &coverPath;
  return prepare(renderer, bookPath, request);
}
