#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstring>
#include <iterator>

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
#include <I18n.h>
#include <NativeTextEngine.h>
#endif

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
namespace {
class NativeFontLock {
 public:
  explicit NativeFontLock(NativeTextEngine* engine) : engine_(engine) {
    if (engine_) engine_->lock();
  }
  ~NativeFontLock() {
    if (engine_) engine_->unlock();
  }

 private:
  NativeTextEngine* engine_;
};

int nativeFontId(const char* family, uint8_t points) {
  // IDs name the logical family/size; content and axes live in the layout fingerprint.
  uint32_t hash = 2166136261u;
  for (const char* p = "native-sd-font:"; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
  for (; *family; ++family) hash = (hash ^ static_cast<uint8_t>(*family)) * 16777619u;
  hash = (hash ^ points) * 16777619u;
  return hash ? static_cast<int>(hash) : 1;
}
}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  nativeEngine_ = renderer.nativeTextEngine();
  NativeFontLock lock(nativeEngine_);
  registry_.discover();
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* name, uint8_t points) {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(name, points);
  };
  SETTINGS.sdFontResolverCtx = this;
  nativeReloadNeeded_ = true;
  ensureLoaded(renderer);
  LOG_DBG("SDFS", "Native SD font system ready (%d families)", registry_.getFamilyCount());
}

void SdCardFontSystem::releaseNativeFonts() {
  NativeFontLock lock(nativeEngine_);
  if (nativeEngine_) nativeEngine_->clearCustomFonts();
  loadedFamily_[0] = '\0';
  for (auto& id : readerIds_) id = 0;
  nativeReloadNeeded_ = true;
}

TextStatus SdCardFontSystem::validateNativeFontFile(std::string_view path, std::span<const NativeVariation> axes) {
  NativeFontLock lock(nativeEngine_);
  return nativeEngine_ ? nativeEngine_->validateFontFile(path, axes) : TextStatus::InvalidFont;
}

void SdCardFontSystem::refreshIfDirty() {
  NativeFontLock lock(nativeEngine_);
  if (!registryDirty_.exchange(false, std::memory_order_acquire)) return;
  releaseNativeFonts();
  registry_.discover();
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  if (!nativeEngine_) nativeEngine_ = renderer.nativeTextEngine();
  NativeFontLock lock(nativeEngine_);
  refreshIfDirty();
  const char* wanted = SETTINGS.sdFontFamilyName;
  const bool serif = SETTINGS.fontFamily != CrossPointSettings::NOTOSANS;
  const bool changed = strcmp(attemptedFamily_, wanted) != 0 || attemptedSerif_ != serif;
  if (!changed && !nativeReloadNeeded_) return;
  if (changed) {
    noticeIssued_ = false;
    notice_ = Notice::None;
  }
  releaseNativeFonts();
  memcpy(attemptedFamily_, SETTINGS.sdFontFamilyName, sizeof(attemptedFamily_));
  attemptedFamily_[sizeof(attemptedFamily_) - 1] = '\0';
  attemptedSerif_ = serif;
  nativeReloadNeeded_ = false;
  if (!*wanted) return;

  const auto* family = registry_.findFamily(wanted);
  TextStatus status = family ? family->nativeStatus : TextStatus::InvalidFont;
  if (!nativeEngine_ || !nativeEngine_->ready()) status = TextStatus::InvalidFont;
  if (family && status == TextStatus::Ok) {
    NativeFontFile files[4];
    size_t count = 0;
    for (const auto& file : family->files) {
      if (count == 4) {
        status = TextStatus::InvalidFont;
        break;
      }
      files[count++] = {file.path, file.style, {file.axes, file.axisCount}};
    }
    if (status == TextStatus::Ok) {
      for (size_t i = 0; i < std::size(readerIds_); ++i)
        readerIds_[i] = nativeFontId(wanted, BUILTIN_READER_POINT_SIZES[i]);
      status = nativeEngine_->registerCustomFont(readerIds_[0], 12, {files, count}, serif);
      for (size_t i = 1; status == TextStatus::Ok && i < std::size(readerIds_); ++i)
        status = nativeEngine_->registerFontAlias(readerIds_[i], readerIds_[0], BUILTIN_READER_POINT_SIZES[i]);
      if (status == TextStatus::Ok) status = nativeEngine_->setUiFallback(readerIds_[0]);
    }
  }
  if (status == TextStatus::Ok) {
    memcpy(loadedFamily_, attemptedFamily_, sizeof(loadedFamily_));
    LOG_DBG("SDFS", "Loaded native family %s at all reader/UI sizes", loadedFamily_);
    return;
  }
  // A failed optional style/axis or any alias failure rejects the complete family.
  releaseNativeFonts();
  nativeReloadNeeded_ = false;
  if (!noticeIssued_) {
    notice_ = !family                             ? Notice::Migration
              : status == TextStatus::OutOfMemory ? Notice::Memory
              : status == TextStatus::InvalidFont ? Notice::Invalid
                                                  : Notice::Render;
    noticeIssued_ = true;
  }
  LOG_ERR("SDFS", "Native family %s unavailable (status %u); retaining saved selection", wanted,
          static_cast<unsigned>(status));
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t pointSize) const {
  NativeFontLock lock(nativeEngine_);
  if (!familyName || strcmp(loadedFamily_, familyName) != 0 || !loadedFamily_[0]) return 0;
  const uint8_t points =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), pointSize);
  for (size_t i = 0; i < std::size(readerIds_); ++i)
    if (BUILTIN_READER_POINT_SIZES[i] == points) return readerIds_[i];
  return 0;
}

const char* SdCardFontSystem::takeNotice() {
  NativeFontLock lock(nativeEngine_);
  const Notice notice = notice_;
  notice_ = Notice::None;
  switch (notice) {
    case Notice::Migration:
      return tr(STR_NATIVE_FONT_MIGRATION_REQUIRED);
    case Notice::Invalid:
      return tr(STR_NATIVE_FONT_INVALID);
    case Notice::Memory:
      return tr(STR_MEMORY_ERROR);
    case Notice::Render:
      return tr(STR_TEXT_RENDER_ERROR);
    default:
      return nullptr;
  }
}
#else

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so UI text
// in scripts the built-ins lack (CJK, Greek, Cyrillic, ...) matches the
// surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on codepoints the built-in UI fonts lack,
  // so a family with no coverage beyond theirs can never act as a fallback and
  // its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script the built-in fonts may lack:
  // Han, Hiragana, Katakana, Hangul, Greek, Cyrillic, Hebrew, Arabic, Thai,
  // Devanagari.
  static constexpr uint32_t kFallbackProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00, 0x03B1,
                                                 0x0430, 0x05D0, 0x0627, 0x0E01, 0x0905};
  bool hasFallbackScript = false;
  for (const uint32_t cp : kFallbackProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasFallbackScript = true;
      break;
    }
  }
  if (!hasFallbackScript) {
    LOG_DBG("SDFS", "%s has no fallback-script coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

void SdCardFontSystem::refreshIfDirty() {
  if (registryDirty_.exchange(false, std::memory_order_acquire)) registry_.discover();
}
#endif
