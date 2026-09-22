#pragma once

#include <Txt.h>

#if defined(CROSSPOINT_NATIVE_TEXT)
#include <Epub/Page.h>
#include <NativeTxtCache.h>
#else
#include <vector>
#endif

#include <cstdint>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "ReaderActivity.h"

class TxtReaderActivity final : public ReaderActivity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
#if defined(CROSSPOINT_NATIVE_TEXT)
  int totalPages = 0;
  NativeTxtCache nativeCache;
  std::unique_ptr<Page> currentNativePage;
  uint32_t loadedNativePage = UINT32_MAX;
  uint32_t nativeSourceStart = 0;
  uint32_t nativeSourceEnd = 0;
  uint32_t displayedSourceStart = 0;
  uint32_t displayedSourceSize = 0;
  uint64_t displayedSourceHash = 0;
  bool hasDisplayedSource = false;
  bool nativeRenderFailed = false;
  int viewportHeight = 0;
  uint8_t cachedOrientation = 0;
  int cachedStatusBarHeight = 0;
#else
  int totalPages = 1;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
#endif
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

#if defined(CROSSPOINT_NATIVE_TEXT)
  bool initializeNativeReader(GfxRenderer& renderer);
  bool buildNativePageCache(GfxRenderer& renderer);
  bool loadNativePage(uint32_t pageIndex);
  bool renderNativePage(GfxRenderer& renderer);
  bool failNativeReader(TextStatus status);
#else
  void renderPage(GfxRenderer& renderer);
  void initializeReader(GfxRenderer& renderer);
  bool loadPageAtOffset(const GfxRenderer& renderer, size_t offset, std::vector<std::string>& outLines,
                        size_t& nextOffset);
  void buildPageIndex(GfxRenderer& renderer);
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();
#endif
  void renderStatusBar() const;

  bool loadBook() override;
  std::string getBookTitle() const override { return txt ? txt->getTitle() : ""; }
  void renderBook() override;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("TxtReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~TxtReaderActivity() override = default;

  void onExit() override;
  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
