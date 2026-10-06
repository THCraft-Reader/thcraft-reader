#include "ReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Memory.h>
#include <TrustedTime.h>

#if THAI_ENGINE_STATS
#include <HalMemory.h>
#include <ThaiDictionary.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "XtcReaderActivity.h"
#include "util/BookCoverPreparation.h"
#include "util/PluginEvents.h"

#if THAI_ENGINE_STATS
void ReaderActivity::beginThaiStatsWindow() {
  thaiStatsPrevious = thai::statsSnapshot();
  const auto reads = HalStorage::readStats();
  thaiSdCallsPrevious = reads.calls;
  thaiSdBytesPrevious = reads.bytes;
  thaiWindowStartedMs = static_cast<uint32_t>(millis());
  thaiMinimumInternal =
      std::min(thaiMinimumInternal, static_cast<uint32_t>(HalMemory::getInternal8BitHeap().freeBytes));
}

void ReaderActivity::logThaiStatsPhase(const char* phase) {
  if (!thaiStatsActive) return;
  const auto stats = thai::statsSnapshot();
  const auto reads = HalStorage::readStats();
  const auto internal = HalMemory::getInternal8BitHeap();
  const auto psram = HalMemory::getPsramHeap();
  const uint32_t now = static_cast<uint32_t>(millis());
  thaiMinimumInternal = std::min(thaiMinimumInternal, static_cast<uint32_t>(internal.freeBytes));
  const unsigned sample = ++thaiSample;
  // Logging has a 256-byte record cap. Join these bounded records by sample;
  // all measurements precede logging so transport latency is not timed here.
  LOG_INF("THAI", "phase=%s sample=%u layout_us=%u segment_us=%u draw_us=%u refresh_ms=%u event_ms=%u open_ms=%u",
          phase, sample, static_cast<unsigned>(stats.layout_us - thaiStatsPrevious.layout_us),
          static_cast<unsigned>(stats.segment_us - thaiStatsPrevious.segment_us),
          static_cast<unsigned>(stats.draw_us - thaiStatsPrevious.draw_us),
          static_cast<unsigned>(stats.refresh_ms - thaiStatsPrevious.refresh_ms),
          static_cast<unsigned>(now - thaiWindowStartedMs), static_cast<unsigned>(now - thaiOpenStartedMs));
  LOG_INF("THAI", "phase=%s sample=%u input_bytes=%u clusters=%u words=%u unknown_clusters=%u max_pending_bytes=%u",
          phase, sample, static_cast<unsigned>(stats.input_bytes - thaiStatsPrevious.input_bytes),
          static_cast<unsigned>(stats.clusters - thaiStatsPrevious.clusters),
          static_cast<unsigned>(stats.words - thaiStatsPrevious.words),
          static_cast<unsigned>(stats.unknown_clusters - thaiStatsPrevious.unknown_clusters),
          static_cast<unsigned>(stats.max_pending_bytes));
  LOG_INF("THAI",
          "phase=%s sample=%u internal_free=%u internal_largest=%u internal_min_observed=%u internal_min_since_boot=%u",
          phase, sample, static_cast<unsigned>(internal.freeBytes), static_cast<unsigned>(internal.largestBlockBytes),
          static_cast<unsigned>(thaiMinimumInternal), static_cast<unsigned>(internal.minFreeBytes));
  if (psram.totalBytes) {
    LOG_INF("THAI", "phase=%s sample=%u psram_available=1 psram_free=%u psram_largest=%u", phase, sample,
            static_cast<unsigned>(psram.freeBytes), static_cast<unsigned>(psram.largestBlockBytes));
  } else {
    LOG_INF("THAI", "phase=%s sample=%u psram_available=0 psram_free=null psram_largest=null", phase, sample);
  }
  LOG_INF("THAI", "phase=%s sample=%u sd_read_calls=%llu sd_read_bytes=%llu dictionary_id=%u font_id=%u", phase, sample,
          static_cast<unsigned long long>(reads.calls - thaiSdCallsPrevious),
          static_cast<unsigned long long>(reads.bytes - thaiSdBytesPrevious),
          static_cast<unsigned>(thai::dictionaryDataId()), static_cast<unsigned>(SETTINGS.getReaderFontId()));
  thaiStatsPrevious = stats;
  thaiSdCallsPrevious = reads.calls;
  thaiSdBytesPrevious = reads.bytes;
  thaiWindowStartedMs = now;
}

ReaderActivity::~ReaderActivity() {
  // Derived members (including the section/parser) have already been destroyed.
  // The app intentionally retains globally loaded fonts between reader sessions.
  logThaiStatsPhase("reader_exit");
  if (thaiStatsActive) LOG_INF("THAI", "phase=reader_exit scope=after_derived_teardown_global_fonts_retained");
}
#endif

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh)
    : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)) {
  if (allowFastInitialRefresh) {
    const int refreshFrequency = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = refreshFrequency > 1 ? refreshFrequency : 2;
  }
}

std::unique_ptr<ReaderActivity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string path, const bool allowFastInitialRefresh) {
  // ActivityManager requires heap ownership; each branch allocates exactly one screen-lifetime object.
  std::unique_ptr<ReaderActivity> activity;
  if (FsHelpers::hasXtcExtension(path)) {
    activity = makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  } else {
    activity = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (!activity) {
    LOG_ERR("READER", "OOM: reader activity");
  }
  return activity;
}

void ReaderActivity::applyInitialOrientation() { ReaderUtils::applyOrientation(renderer, SETTINGS.orientation); }

void ReaderActivity::disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }

void ReaderActivity::notePageTurn(const bool forward, const bool succeeded) {
  RenderLock lock(*this);
  readerSession.noteTurn(forward, succeeded);
}

void ReaderActivity::onEnter() {
  Activity::onEnter();
#if THAI_ENGINE_STATS
  thaiStatsActive = true;
  thaiOpenStartedMs = static_cast<uint32_t>(millis());
  beginThaiStatsWindow();
  logThaiStatsPhase("open_start");
  LOG_INF(
      "THAI",
      "scope=device_cumulative_snapshot_deltas heap_scope=INTERNAL|8BIT,SPIRAM min_scope=lifecycle_and_turn_samples");
  LOG_INF("THAI",
          "segment_scope=nextSegment draw_scope=glyph_loops_excludes_scan_and_batch_prewarm "
          "sd_scope=HAL_requests_not_sectors");
  LOG_INF("THAI",
          "refresh_scope=HAL_refresh_gray_service_start_to_observed_completion_includes_transfer_settle_async_"
          "observation_delay_not_BUSY_edge");
#endif

  // Heap ledger for field crash reports: free vs largest block distinguishes a
  // leak (free falls) from fragmentation (free stable, largest collapses).
  LOG_INF("MEM", "reader enter: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  // Clear remembered book after opening it
  if (!APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

  {
    RenderLock lock(*this);
    if (bookcovers::prepareForReader(renderer, bookPath)) {
      // Preparation already restored the selection; do not retry a failed
      // temporary restore through the normal loader's clearing policy.
      disableFastInitialRefresh();
    } else {
      sdFontSystem.ensureLoaded(renderer);
    }
  }
  applyInitialOrientation();

  if (!loadBook()) {
    if (!handleLoadFailure()) finish();
    return;
  }
#if THAI_ENGINE_STATS
  logThaiStatsPhase("metadata_ready");
#endif

  requestUpdate();
}

void ReaderActivity::rememberBookOnceRendered() {
  if (bookRemembered || !pageRendered.load(std::memory_order_acquire)) return;
  bookRemembered = true;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
  const pluginevents::Var openVars[] = {{"book", bookPath.c_str()}};
  pluginevents::emit(pluginevents::Event::ReaderOpen, openVars, 1);
}

void ReaderActivity::onExit() {
  Activity::onExit();

  // Keep rebuildable font buffers from pinning the heap between reading sessions.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  LOG_INF("MEM", "reader exit: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // Flush BEFORE the ReaderExit event: the session's final progress must be
  // durable before a subscriber can act on the exit notification.
  flushReaderSession();

  if (pluginevents::anySubscriber(pluginevents::Event::ReaderExit)) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d", getScreenshotInfo().progressPercent);
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()}, {"percent", percent}};
    pluginevents::emit(pluginevents::Event::ReaderExit, vars, 2);
  }

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

void ReaderActivity::prepareForSleep() { flushReaderSession(); }

void ReaderActivity::flushReaderSession() {
  if (!readerSession.isEmitWorthy() || !pluginevents::anySubscriber(pluginevents::Event::ReaderSession)) {
    readerSession.reset();
    return;
  }

  const std::string document = KOReaderDocumentId::calculate(bookPath);
  const bool validDocument =
      document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      });
  if (validDocument) {
    char startTime[24];
    char endTime[24];
    char duration[16];
    char startProgress[8];
    char endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(readerSession.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld", static_cast<long long>(readerSession.endTime()));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(readerSession.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", readerSession.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", readerSession.endProgressBp());
    const pluginevents::Var vars[] = {{"book", bookPath.c_str()},       {"document", document.c_str()},
                                      {"start_time", startTime},        {"end_time", endTime},
                                      {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                                      {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    pluginevents::emit(pluginevents::Event::ReaderSession, vars, 8);
  }
  readerSession.reset();
}

bool ReaderActivity::handleBackNavigation() {
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, bookPath.c_str(),
                                           {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;

  RenderLock lock(*this);
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::endOfBookMenuActive() const {
  return isAtEndOfBook() && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (suppressConfirmRelease || !endOfBookMenuActive()) {
    return false;
  }

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }

  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;

  if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
    return true;
  }
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

void ReaderActivity::loop() {
  rememberBookOnceRendered();
  clearEndOfBookOptionsIfNeeded();
  if (handleEndOfBookMenu()) return;
  if (handleFormatInput()) return;
  if (handleBackNavigation()) return;

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) return;
  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool skip =
      !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP && heldMs >= ReaderUtils::SKIP_HOLD_MS;

  const bool changed = skip ? skipPages(prevTriggered ? -10 : 10) : pageTurn(!prevTriggered);
  // A skip is navigation, not reading: it never counts toward session dwell.
  notePageTurn(!skip && !prevTriggered, changed);
  if (changed && (touch.prev || touch.next)) haptic_feedback::touchAction(skip);
  requestUpdate();
}

void ReaderActivity::render(RenderLock&&) {
  if (isAtEndOfBook()) {
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("READER", "OOM: EndOfBookOptions");
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(bookPath);
      // Release-publish AFTER loadOnce() so the main task's acquire load can't
      // observe an object whose names/selector are still being populated.
      endOfBookOptionsReady.store(true, std::memory_order_release);
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    onEndOfBookRendered();
    markPageRendered();
    readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
    return;
  }

  renderBook();
  if (renderer.hasThaiShapeError()) {
    readerSession.noteTurn(false, false);
    return;
  }
  readerSession.onRenderComplete(millis(), trustedtime::trustedNow(), getProgressBasisPoints());
}

bool ReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}
