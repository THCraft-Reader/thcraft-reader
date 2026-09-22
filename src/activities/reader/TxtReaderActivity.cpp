#include "TxtReaderActivity.h"

#if !defined(CROSSPOINT_NATIVE_TEXT)
#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <Serialization.h>
#include <Utf8.h>

#include "ProgressFile.h"
#endif
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#if defined(CROSSPOINT_NATIVE_TEXT)
#include <NativeTextTypes.h>
#endif

#include "CrossPointSettings.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

#if !defined(CROSSPOINT_NATIVE_TEXT)
namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes
}  // namespace
#endif

bool TxtReaderActivity::loadBook() {
  txt = makeUniqueNoThrow<Txt>(bookPath, "/.crosspoint");
  if (!txt) {
    LOG_ERR("TRS", "Failed to allocate TXT object");
    return false;
  }
  if (!txt->load()) {
    LOG_ERR("TRS", "Failed to load TXT");
    return false;
  }
  txt->setupCacheDir();
  return true;
}

void TxtReaderActivity::onExit() {
#if defined(CROSSPOINT_NATIVE_TEXT)
  // ActivityManager already owns RenderLock while calling onExit.
  currentNativePage.reset();
  nativeCache.close();
  initialized = false;
  loadedNativePage = UINT32_MAX;
  hasDisplayedSource = false;
#endif
  ReaderActivity::onExit();
}

#if defined(CROSSPOINT_NATIVE_TEXT)
bool TxtReaderActivity::failNativeReader(TextStatus status) {
  nativeRenderFailed = true;
  currentNativePage.reset();
  loadedNativePage = UINT32_MAX;
  renderer.recordTextFailure(status == TextStatus::Ok ? TextStatus::InvalidText : status);
  return false;
}

bool TxtReaderActivity::initializeNativeReader(GfxRenderer& renderer) {
  if (!renderer.usesNativeText()) return failNativeReader(TextStatus::InvalidFont);

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int statusBarHeight = UITheme::getStatusBarHeight();
  top += SETTINGS.screenMargin;
  right += SETTINGS.screenMargin;
  left += SETTINGS.screenMargin;
  bottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarHeight);
  const int width = renderer.getScreenWidth() - left - right;
  const int height = renderer.getScreenHeight() - top - bottom;
  if (width <= 0 || height <= 0 || width > INT16_MAX || height > INT16_MAX) {
    return failNativeReader(TextStatus::CapacityExceeded);
  }
  if (initialized && cachedOrientation == SETTINGS.orientation && cachedStatusBarHeight == statusBarHeight &&
      cachedOrientedMarginTop == top && cachedOrientedMarginRight == right && cachedOrientedMarginBottom == bottom &&
      cachedOrientedMarginLeft == left && nativeCache.matches(renderer, width, height, SETTINGS.paragraphAlignment)) {
    return true;
  }

  initialized = false;
  currentNativePage.reset();
  loadedNativePage = UINT32_MAX;
  nativeCache.close();
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedOrientation = SETTINGS.orientation;
  cachedStatusBarHeight = statusBarHeight;
  cachedOrientedMarginTop = top;
  cachedOrientedMarginRight = right;
  cachedOrientedMarginBottom = bottom;
  cachedOrientedMarginLeft = left;
  viewportWidth = width;
  viewportHeight = height;

  const auto result = nativeCache.open(*txt, renderer, width, height, cachedParagraphAlignment);
  if (result == NativeTxtCache::Result::Error) return failNativeReader(nativeCache.lastStatus());
  if (result == NativeTxtCache::Result::Rebuild && !buildNativePageCache(renderer)) return false;
  if (nativeCache.pageCount() > INT32_MAX) return failNativeReader(TextStatus::CapacityExceeded);
  totalPages = static_cast<int>(nativeCache.pageCount());

  uint32_t restoredPage = 0;
  if (hasDisplayedSource) {
    if (displayedSourceSize != nativeCache.sourceSize() || displayedSourceHash != nativeCache.sourceHash()) {
      LOG_INF("TRS", "TXT source changed; restoring the displayed byte position best-effort");
    }
    if (!nativeCache.pageForSource(displayedSourceStart, restoredPage)) {
      return failNativeReader(nativeCache.lastStatus());
    }
  } else if (!nativeCache.restorePage(restoredPage)) {
    return failNativeReader(nativeCache.lastStatus());
  }
  currentPage = static_cast<int>(restoredPage);
  initialized = true;
  return true;
}

bool TxtReaderActivity::buildNativePageCache(GfxRenderer& renderer) {
  currentNativePage.reset();
  loadedNativePage = UINT32_MAX;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  if (renderer.lastTextStatus() != TextStatus::Ok) return failNativeReader(renderer.lastTextStatus());
  if (!nativeCache.build(renderer)) return failNativeReader(nativeCache.lastStatus());
  if (nativeCache.pageCount() > INT32_MAX) return failNativeReader(TextStatus::CapacityExceeded);
  totalPages = static_cast<int>(nativeCache.pageCount());
  return true;
}

bool TxtReaderActivity::loadNativePage(uint32_t pageIndex) {
  if (currentNativePage && loadedNativePage == pageIndex) return true;
  currentNativePage.reset();
  loadedNativePage = UINT32_MAX;
  nativeSourceStart = nativeSourceEnd = 0;
  auto result = nativeCache.loadPage(pageIndex, currentNativePage, nativeSourceStart, nativeSourceEnd);
  if (result == NativeTxtCache::Result::Rebuild) {
    // A valid LUT may still locate a corrupt page body. Prefer that requested
    // position; otherwise retain the last page actually displayed.
    const bool hasRequestedSource = nativeSourceEnd > nativeSourceStart;
    const uint32_t sourceByte = hasRequestedSource ? nativeSourceStart : displayedSourceStart;
    if (!buildNativePageCache(renderer)) return false;
    if (hasRequestedSource || hasDisplayedSource) {
      if (!nativeCache.pageForSource(sourceByte, pageIndex)) return failNativeReader(nativeCache.lastStatus());
    } else if (!nativeCache.restorePage(pageIndex)) {
      return failNativeReader(nativeCache.lastStatus());
    }
    currentPage = static_cast<int>(pageIndex);
    if (totalPages == 0) return true;
    result = nativeCache.loadPage(pageIndex, currentNativePage, nativeSourceStart, nativeSourceEnd);
  }
  if (result != NativeTxtCache::Result::Ready || !currentNativePage) {
    return failNativeReader(nativeCache.lastStatus());
  }
  loadedNativePage = pageIndex;
  return true;
}

bool TxtReaderActivity::renderNativePage(GfxRenderer& renderer) {
  if (!currentNativePage) return failNativeReader(TextStatus::InvalidText);
  if (!currentNativePage->warmNativeText(renderer, cachedFontId) || renderer.lastTextStatus() != TextStatus::Ok) {
    return failNativeReader(renderer.lastTextStatus());
  }

  const auto renderContent = [&]() {
    if (renderer.lastTextStatus() != TextStatus::Ok) return;
    int clipX, clipY, clipWidth, clipHeight;
    renderer.getClipRect(clipX, clipY, clipWidth, clipHeight);
    const int left = std::max(clipX, cachedOrientedMarginLeft);
    const int top = std::max(clipY, cachedOrientedMarginTop);
    const int right = std::min(clipX + clipWidth, cachedOrientedMarginLeft + viewportWidth);
    const int bottom = std::min(clipY + clipHeight, cachedOrientedMarginTop + viewportHeight);
    renderer.setClipRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
    currentNativePage->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop);
    renderer.setClipRect(clipX, clipY, clipWidth, clipHeight);
  };

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  renderContent();
  if (renderer.lastTextStatus() != TextStatus::Ok) return failNativeReader(renderer.lastTextStatus());
  renderStatusBar();
  if (renderer.lastTextStatus() != TextStatus::Ok) return failNativeReader(renderer.lastTextStatus());

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
    unsigned grayPasses = 0;
    ReaderUtils::renderAntiAliased(renderer, [&]() {
      ++grayPasses;
      renderContent();
    });
    if (renderer.lastTextStatus() != TextStatus::Ok) return failNativeReader(renderer.lastTextStatus());
    // The helper returns without invoking the callback when its BW snapshot
    // allocation fails. A partial AA display must not advance saved progress.
    if (grayPasses != 2) return failNativeReader(TextStatus::OutOfMemory);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  if (!nativeCache.matches(renderer, viewportWidth, viewportHeight, cachedParagraphAlignment)) {
    initialized = false;
    return failNativeReader(TextStatus::InvalidFont);
  }
  forcedRefreshPending = false;
  return true;
}
#else
void TxtReaderActivity::initializeReader(GfxRenderer& renderer) {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex(renderer);
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex(GfxRenderer& renderer) {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(renderer, offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(const GfxRenderer& renderer, size_t offset, std::vector<std::string>& outLines,
                                         size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);
    size_t lineBytePos = 0;

    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, line.substr(0, breakPos).c_str(),
                                                      EpdFontFamily::REGULAR) > viewportWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));

      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    if (line.empty()) {
      pos = lineEnd + 1;
    } else {
      pos = pos + lineBytePos;
      break;
    }
  }

  if (pos == 0 && !outLines.empty()) {
    pos = 1;
  }

  nextOffset = offset + pos;
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);
  return !outLines.empty();
}
#endif

void TxtReaderActivity::renderBook() {
  if (!txt) {
    return;
  }

#if defined(CROSSPOINT_NATIVE_TEXT)
  if (!initializeNativeReader(renderer)) return;
  if (totalPages > 0) {
    currentPage = std::max(0, std::min(currentPage, totalPages - 1));
    if (!loadNativePage(static_cast<uint32_t>(currentPage))) return;
  }
  if (totalPages == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    if (renderer.lastTextStatus() != TextStatus::Ok) {
      failNativeReader(renderer.lastTextStatus());
      return;
    }
    renderer.displayBuffer();
    nativeRenderFailed = false;
    return;
  }
  if (!renderNativePage(renderer)) return;
  nativeRenderFailed = false;
  displayedSourceStart = nativeSourceStart;
  displayedSourceSize = nativeCache.sourceSize();
  displayedSourceHash = nativeCache.sourceHash();
  hasDisplayedSource = true;
  if (!nativeCache.saveProgress(displayedSourceStart)) {
    LOG_ERR("TRS", "Failed to save native TXT source position: %lu", static_cast<unsigned long>(displayedSourceStart));
    failNativeReader(nativeCache.lastStatus());
  }
#else
  if (!initialized) {
    initializeReader(renderer);
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(renderer, offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage(renderer);

  // Save progress
  saveProgress();
#endif
}

#if !defined(CROSSPOINT_NATIVE_TEXT)
void TxtReaderActivity::renderPage(GfxRenderer& renderer) {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();      // scan pass
  renderStatusBar();  // scan: a CJK title joins the batch prewarm
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}
#endif

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

bool TxtReaderActivity::pageTurn(bool isForward) {
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (totalPages == 0 || nativeRenderFailed) return false;
#endif
  // Ignore paging until initializeReader has established the page index
  if (!initialized) {
    return false;
  }
  if (isForward) {
    if (currentPage < totalPages) {
      currentPage++;
      return true;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      return true;
    }
  }
  return false;
}

bool TxtReaderActivity::skipPages(int amount) {
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (totalPages == 0 || nativeRenderFailed) return false;
#endif
  if (!initialized) {
    return false;
  }
#if defined(CROSSPOINT_NATIVE_TEXT)
  int newPage =
      static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(static_cast<int64_t>(currentPage) + amount, totalPages)));
#else
  int newPage = currentPage + amount;
  if (newPage < 0) newPage = 0;
#endif
  // Clamp to totalPages, not totalPages - 1: pageTurn() lets currentPage reach
  // totalPages and isAtEndOfBook() treats that as the end-of-book sentinel, so
  // a forward skip must be able to reach it too.
  if (newPage > totalPages) newPage = totalPages;
  if (newPage != currentPage) {
    currentPage = newPage;
    return true;
  }
  return false;
}

bool TxtReaderActivity::isAtEndOfBook() const {
#if defined(CROSSPOINT_NATIVE_TEXT)
  if (totalPages == 0 || nativeRenderFailed) return false;
#endif
  return initialized && currentPage >= totalPages;
}

void TxtReaderActivity::onReturnFromEndOfBook() { currentPage = totalPages > 0 ? totalPages - 1 : 0; }

#if !defined(CROSSPOINT_NATIVE_TEXT)
void TxtReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = 0;
  data[3] = 0;
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: page %d", currentPage);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}
#endif

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
#if defined(CROSSPOINT_NATIVE_TEXT)
  info.currentPage = totalPages > 0 ? std::min(currentPage, totalPages - 1) + 1 : 0;
#else
  info.currentPage = currentPage + 1;
#endif
  info.totalPages = totalPages;
#if defined(CROSSPOINT_NATIVE_TEXT)
  info.progressPercent = totalPages > 0 ? static_cast<int>(info.currentPage * 100.0f / totalPages + 0.5f) : 0;
#else
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
#endif
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
