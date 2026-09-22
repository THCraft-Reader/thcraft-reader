#include "FontDownloadActivity.h"

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
#include <NativeFontCatalogue.generated.h>
#include <NativeTextEngine.h>
#else
#include <FontCacheManager.h>
#endif
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

void FontDownloadActivity::activateIndex(const int index) {
  switch (state_) {
    case GROUP_LIST:
      app.clearTapFlash();
      enterGroup(index);
      requestUpdate();
      return;
    case FAMILY_LIST:
      nav.selected = index;
      // Activation starts a download or opens the delete prompt; a lingering
      // flash would gray an unrelated row.
      app.clearTapFlash();
      activateSelected();  // ends with requestUpdateAndWait itself
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return;
  }
}

fui::ListNav& FontDownloadActivity::activeNav() { return state_ == GROUP_LIST ? groupNav_ : nav; }

void FontDownloadActivity::onBackButton() {
  if (state_ != FAMILY_LIST || !hasGroupScreen()) {
    finish();
    return;
  }

  closeRouting();
  {
    RenderLock lock(*this);
    state_ = GROUP_LIST;
    rowsDirty_ = true;
  }
  requestUpdate();
}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    // Drop whatever was parsed before the failure: it would otherwise sit in
    // the heap behind the error screen, and leave the retry path pointing at a
    // half-built family table.
    clearManifest();
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  if (!hasGroupScreen()) buildFilteredIndices(0);

  {
    RenderLock lock(*this);
    rowsDirty_ = true;  // manifest just loaded
    if (hasGroupScreen()) {
      groupNav_.reset();
      state_ = GROUP_LIST;
    } else {
      nav.reset();
      state_ = FAMILY_LIST;
    }
  }
}

// --- Manifest fetching ---

void FontDownloadActivity::clearManifest() {
  std::vector<int>().swap(filteredIndices_);
  manifest_.clear();
}

bool FontDownloadActivity::fetchAndParseManifest() {
  static_assert(FontManifest::VERSION == FONTS_MANIFEST_VERSION);
  clearManifest();
  FontManifest::Error parsed;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  // The immutable catalogue ships with the firmware: no network connection,
  // TLS allocation or SD temporary file is needed to read it.
  parsed = manifest_.load(native_text::assets::fontCatalogue, native_text::assets::fontCatalogueSize);
#else
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }
  const auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = tr(STR_FONT_LIST_FETCH_FAILED);
    Storage.remove(MANIFEST_TMP);
    return false;
  }
  HalFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = tr(STR_FONT_STORAGE_ERROR);
    return false;
  }
  parsed = manifest_.load(manifestFile);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);
#endif
  if (parsed != FontManifest::Error::Ok) {
    errorMessage_ = parsed == FontManifest::Error::OutOfMemory    ? tr(STR_MEMORY_ERROR)
                    : parsed == FontManifest::Error::StorageError ? tr(STR_FONT_STORAGE_ERROR)
                                                                  : tr(STR_INVALID_FONT_MANIFEST);
    return false;
  }
  fontInstaller_.refreshRegistry();
  for (auto& family : manifest_.families()) {
    family.installed = fontInstaller_.isFamilyInstalled(str(family.name));
    if (!family.installed) continue;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    const auto* installed = sdFontSystem.registry().findFamily(str(family.name));
    family.hasUpdate =
        !installed || installed->nativeStatus != TextStatus::Ok || installed->files.size() != family.fileCount;
#endif
    for (uint32_t i = 0; i < family.fileCount && !family.hasUpdate; ++i) {
      const auto& file = manifest_.files()[family.fileStart + i];
      char path[128];
      FontInstaller::buildFontPath(str(family.name), str(file.name), path, sizeof(path));
      HalFile existing;
      if (!Storage.openFileForRead("FONT", path, existing)) {
        family.hasUpdate = true;
        break;
      }
      family.hasUpdate = existing.fileSize() != file.size;
      existing.close();
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
      const auto* style = installed->findFile(0, file.style);
      family.hasUpdate = family.hasUpdate || !style || style->axisCount != file.axisCount;
      for (uint8_t axis = 0; axis < file.axisCount && !family.hasUpdate; ++axis) {
        bool matches = false;
        for (uint8_t saved = 0; saved < style->axisCount; ++saved) {
          if (style->axes[saved].tag == file.axes[axis].tag && style->axes[saved].value == file.axes[axis].value)
            matches = true;
        }
        if (!matches) family.hasUpdate = true;
      }
      uint32_t actualCrc = 0;
      if (!family.hasUpdate) family.hasUpdate = !computeFileCrc32(path, actualCrc) || actualCrc != file.crc32;
#endif
    }
  }
  filteredIndices_.reserve(manifest_.families().size());
  const size_t rowCapacity = std::max(manifest_.families().size() + 2, manifest_.groups().size() + 1);
  rowLabels_.reserve(rowCapacity);
  rowItems_.reserve(rowCapacity);
  LOG_DBG("FONT", "Manifest loaded: %zu families, %zu script groups", manifest_.families().size(),
          manifest_.groups().size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  cancelRequested_ = false;
  for (const int familyIndex : filteredIndices_) {
    if (manifest_.families()[familyIndex].installed) continue;
    downloadFamily(manifest_.families()[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  for (const int familyIndex : filteredIndices_) {
    if (!manifest_.families()[familyIndex].hasUpdate) continue;
    downloadFamily(manifest_.families()[familyIndex]);
    if (state_ == ERROR || cancelRequested_) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (!manifest_.families()[familyIndex].installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const int familyIndex : filteredIndices_) {
    if (manifest_.families()[familyIndex].hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return filteredIndices_.empty() ? 0 : static_cast<int>(filteredIndices_.size()) + specialRowCount();
}

int FontDownloadActivity::listCount() const {
  switch (state_) {
    case GROUP_LIST:
      return groupListItemCount();
    case FAMILY_LIST:
      return listItemCount();
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      return 0;
  }
  return 0;
}

int FontDownloadActivity::familyIndexFromList(const int listIndex) const {
  const int filteredIndex = listIndex - specialRowCount();
  if (filteredIndex < 0 || filteredIndex >= static_cast<int>(filteredIndices_.size())) return -1;
  return filteredIndices_[filteredIndex];
}

int FontDownloadActivity::groupMemberCount(const int scriptGroupIndex) const {
  if (scriptGroupIndex < 0 || scriptGroupIndex >= static_cast<int>(manifest_.groups().size())) return 0;
  const uint32_t groupBit = uint32_t{1} << scriptGroupIndex;
  int count = 0;
  for (const auto& family : manifest_.families()) {
    if (family.scriptMask & groupBit) count++;
  }
  return count;
}

void FontDownloadActivity::buildFilteredIndices(const int groupListIndex) {
  filteredIndices_.clear();
  filteredIndices_.reserve(manifest_.families().size());
  if (groupListIndex <= 0) {
    for (int familyIndex = 0; familyIndex < static_cast<int>(manifest_.families().size()); familyIndex++) {
      filteredIndices_.push_back(familyIndex);
    }
    return;
  }

  const uint32_t groupBit = uint32_t{1} << (groupListIndex - 1);
  for (int familyIndex = 0; familyIndex < static_cast<int>(manifest_.families().size()); familyIndex++) {
    if (manifest_.families()[familyIndex].scriptMask & groupBit) filteredIndices_.push_back(familyIndex);
  }
}

void FontDownloadActivity::enterGroup(const int groupListIndex) {
  closeRouting();
  buildFilteredIndices(groupListIndex);
  {
    RenderLock lock(*this);
    nav.reset();
    state_ = FAMILY_LIST;
    rowsDirty_ = true;
  }
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (!manifest_.families()[familyIndex].installed) total += manifest_.families()[familyIndex].totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const int familyIndex : filteredIndices_) {
    if (manifest_.families()[familyIndex].hasUpdate) total += manifest_.families()[familyIndex].totalSize;
  }
  return total;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  const size_t expected = f.fileSize();
  size_t remaining = expected;
  while (remaining) {
    const size_t wanted = std::min(remaining, BUF_SIZE);
    const int n = f.read(buf, wanted);
    if (n <= 0 || static_cast<size_t>(n) > wanted) return false;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
    remaining -= static_cast<size_t>(n);
  }
  if (f.fileSize() != expected) return false;
  outCrc = crc;
  return true;
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - manifest_.families().data());
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
  }
  requestUpdateAndWait();

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  // No external StorageLock is held while releasing the engine's SD handles.
  if (auto* engine = renderer.nativeTextEngine()) {
    engine->clearCaches();
    engine->releaseSdFaces();
  }
  FontInstaller::NativeStyleFile nativeFiles[4]{};
  for (uint32_t i = 0; i < family.fileCount; ++i) {
    const auto& file = manifest_.files()[family.fileStart + i];
    nativeFiles[i] = {str(file.name), file.style, {file.axes, file.axisCount}, file.size, file.crc32, true};
  }
  size_t stagedCount = 0;
  auto discardTransfer = [&] {
    fontInstaller_.discardNativeStaging(str(family.name), {nativeFiles, stagedCount});
    stagedCount = 0;
    // installed/hasUpdate are deliberately unchanged until the atomic commit.
  };
  ScopedCleanup stagingCleanup{discardTransfer};
  constexpr bool nativeTransfer = true;
#else
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  constexpr bool nativeTransfer = false;
  auto discardTransfer = [&] {
    fontInstaller_.deleteFamily(str(family.name));
    family.installed = false;
    family.hasUpdate = false;
  };
#endif

  // Check before touching the family directory so a failed update leaves the
  // installed family unchanged.
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("FONT", "Low heap for download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return;
  }

  if (!fontInstaller_.ensureFamilyDir(str(family.name))) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_STORAGE_ERROR);
    return;
  }

  for (uint32_t i = 0; i < family.fileCount; i++) {
    const ManifestFile& file = manifest_.files()[family.fileStart + i];

    {
      RenderLock lock(*this);
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[128];
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    if (!FontInstaller::buildStagingFontPath(str(family.name), str(file.name), destPath, sizeof(destPath))) {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_INVALID_FONT_MANIFEST);
      return;
    }
    downloadUrl_.assign(str(file.url));
    stagedCount = i + 1;
    // The progress repaint may have reopened SD faces or warmed native caches.
    // Release them again immediately before each independent TLS transfer.
    if (auto* engine = renderer.nativeTextEngine()) {
      engine->clearCaches();
      engine->releaseSdFaces();
    }
    if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
        ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_MEMORY_ERROR);
      return;
    }
#else
    FontInstaller::buildFontPath(str(family.name), str(file.name), destPath, sizeof(destPath));
    downloadUrl_.assign(manifest_.baseUrl()).append(str(file.name));
#endif

    auto result = HttpDownloader::downloadToFile(
        downloadUrl_, destPath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          if (total) fileTotal_ = total;
          mappedInput.update(true);
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          // Home cancels immediately; other configured actions are deferred to
          // the next main-loop pass by the transfer input pump.
          if (mappedInput.wasHomeGesture()) {
            cancelRequested_ = true;
            goHomeRequested_ = true;
          }
          requestUpdate(true);
        },
        // Only legacy CRC-protected release assets use the low-memory HTTP redirect policy.
        &cancelRequested_, "", "", /*downgradeRedirectsToHttp=*/!nativeTransfer, /*requireHttps=*/nativeTransfer);

    if (result == HttpDownloader::ABORTED || cancelRequested_) {
      discardTransfer();
      if (goHomeRequested_) {
        onGoHome();
        return;
      }
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;
      }
      return;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", str(file.name), result);
      discardTransfer();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = result == HttpDownloader::FILE_ERROR ? tr(STR_FONT_STORAGE_ERROR) : tr(STR_DOWNLOAD_FAILED);
      return;
    }

#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath, actualCrc)) {
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", destPath);
      discardTransfer();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_FONT_STORAGE_ERROR);
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", str(file.name), actualCrc, file.crc32);
      discardTransfer();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_FONT_CHECKSUM_MISMATCH);
      return;
    }
    LOG_DBG("FONT", "Downloaded %s (size=%u crc32=%08x)", str(file.name), file.size, actualCrc);

    if (!fontInstaller_.validateFontFile(destPath)) {
      LOG_ERR("FONT", "Invalid font: %s", destPath);
      discardTransfer();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = tr(STR_FONT_INSTALL_FAILED);
      return;
    }
#endif
    currentFileIndex_++;
  }

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  // The installer verifies actual reads, byte sizes, CRCs, FT faces and axes
  // for every staged style before publishing any file or the axes sidecar.
  const auto committed = fontInstaller_.commitNativeFamily(str(family.name), {nativeFiles, family.fileCount}, true);
  if (committed != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = committed == FontInstaller::Error::OUT_OF_MEMORY    ? tr(STR_MEMORY_ERROR)
                    : committed == FontInstaller::Error::INVALID_FILE   ? tr(STR_NATIVE_FONT_INVALID)
                    : committed == FontInstaller::Error::SD_WRITE_ERROR ? tr(STR_FONT_STORAGE_ERROR)
                                                                        : tr(STR_FONT_INSTALL_FAILED);
    return;
  }
  stagedCount = 0;
#endif
  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(nav.selected);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(manifest_.families().size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = manifest_.families()[pendingDeleteFamilyIndex];
  std::string body = str(family.name);
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  const int familyIndex = familyIndexFromList(nav.selected);
  if (familyIndex < 0) {
    requestUpdate();
    return;
  }
  auto& family = manifest_.families()[familyIndex];

  if (fontInstaller_.deleteFamily(str(family.name)) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = tr(STR_FONT_DELETE_FAILED);
  } else {
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    // Unlike the other family_ mutations, this one stays in FAMILY_LIST (no
    // state_ transition to hang the rebuild off), so it must set the flag
    // directly.
    rowsDirty_ = true;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(nav.selected) || isUpdateAllRow(nav.selected)) return false;
  if (nav.selected < specialRowCount() || nav.selected >= listItemCount()) return false;
  const auto& family = manifest_.families()[familyIndexFromList(nav.selected)];
  return family.installed && !family.hasUpdate;
}

void FontDownloadActivity::activateSelected() {
  if (filteredIndices_.empty()) return;
  if (isDownloadAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (!manifest_.families()[familyIndex].installed)
        currentFileTotal_ += manifest_.families()[familyIndex].fileCount;
    }
    downloadAll();
  } else if (isUpdateAllRow(nav.selected)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const int familyIndex : filteredIndices_) {
      if (manifest_.families()[familyIndex].hasUpdate) currentFileTotal_ += manifest_.families()[familyIndex].fileCount;
    }
    updateAll();
  } else {
    // The special rows disappear when a download starts, so a stale selection
    // can map past the family table.
    const int familyIndex = familyIndexFromList(nav.selected);
    if (familyIndex < 0 || familyIndex >= static_cast<int>(manifest_.families().size())) return;
    auto& family = manifest_.families()[familyIndex];
    if (!family.installed || family.hasUpdate) {
      currentFileIndex_ = 0;
      currentFileTotal_ = family.fileCount;
      downloadFamily(family);
    } else {
      promptDeleteSelectedFamily();
      return;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state_ == FAMILY_LIST && filteredIndices_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  if (rowsDirty_) {
    rebuildRowItems();
    rowsDirty_ = false;
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the status and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

void FontDownloadActivity::rebuildRowItems() {
  switch (state_) {
    case GROUP_LIST:
      rebuildGroupRowItems();
      return;
    case FAMILY_LIST:
      rebuildFamilyRowItems();
      return;
    case WIFI_SELECTION:
    case LOADING_MANIFEST:
    case DOWNLOADING:
    case COMPLETE:
    case ERROR:
      rowLabels_.clear();
      rowItems_.clear();
      return;
  }
}

void FontDownloadActivity::rebuildGroupRowItems() {
  const int listSize = groupListItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int rowIndex = 0; rowIndex < listSize; rowIndex++) {
    fui::ListItem item;
    item.label = rowIndex == 0 ? tr(STR_ALL_FONTS) : str(manifest_.groups()[rowIndex - 1]);
    const int memberCount =
        rowIndex == 0 ? static_cast<int>(manifest_.families().size()) : groupMemberCount(rowIndex - 1);
    rowLabels_[rowIndex] = std::to_string(memberCount);
    item.value = rowLabels_[rowIndex].c_str();
    item.actionValue = static_cast<int16_t>(rowIndex);
    rowItems_.push_back(item);
  }
}

void FontDownloadActivity::rebuildFamilyRowItems() {
  const int listSize = listItemCount();
  rowLabels_.assign(listSize, std::string());
  rowItems_.clear();
  rowItems_.reserve(listSize);
  for (int i = 0; i < listSize; i++) {
    fui::ListItem item;
    if (isDownloadAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else if (isUpdateAllRow(i)) {
      rowLabels_[i] = std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
      item.label = rowLabels_[i].c_str();
    } else {
      const auto& family = manifest_.families()[familyIndexFromList(i)];
      item.label = str(family.name);
      if (family.description != 0) item.subtitle = str(family.description);
      if (family.hasUpdate) {
        item.value = tr(STR_UPDATE_AVAILABLE);
      } else if (family.installed) {
        item.value = tr(STR_INSTALLED);
        // Dimmed but still tappable (opens the delete prompt): visual-only
        // disabled state, the row stays enabled for hit registration.
        item.state = fui::StateDisabled;
      }
    }
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

// --- Input handling ---

bool FontDownloadActivity::handleCustomInput() {
  if (state_ == GROUP_LIST || state_ == FAMILY_LIST) {
    // The base list protocol (Back/Confirm, touch routing, swipe scroll,
    // button navigation) handles both list states.
    return false;
  }

  if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the completed download changed installed/hasUpdate
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
        rowsDirty_ = true;  // the failed download reset installed/hasUpdate
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(manifest_.families().size())) {
        downloadFamily(manifest_.families()[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return true;
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(manifest_.families().size())) {
          downloadFamily(manifest_.families()[downloadingFamilyIndex_]);
          requestUpdateAndWait();
          return true;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          rowsDirty_ = true;
        }
        requestUpdate();
      }
    }
  }

  return true;
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* headerSubtitle = nullptr;
  if (state_ == FAMILY_LIST && hasGroupScreen()) {
    const int scriptGroupIndex = groupNav_.selected - 1;
    headerSubtitle = scriptGroupIndex >= 0 && scriptGroupIndex < static_cast<int>(manifest_.groups().size())
                         ? str(manifest_.groups()[scriptGroupIndex])
                         : tr(STR_ALL_FONTS);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER),
                 headerSubtitle);

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == GROUP_LIST) {
    renderUi();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == FAMILY_LIST) {
    renderUi();

    const bool hasVisibleFamilies = !filteredIndices_.empty();
    const char* confirmLabel = !hasVisibleFamilies            ? ""
                               : isSelectedFamilyDeletable()  ? tr(STR_DELETE)
                               : isUpdateAllRow(nav.selected) ? tr(STR_UPDATE)
                                                              : tr(STR_DOWNLOAD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, hasVisibleFamilies ? tr(STR_DIR_UP) : "",
                                              hasVisibleFamilies ? tr(STR_DIR_DOWN) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& family = manifest_.families()[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + str(family.name) + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
