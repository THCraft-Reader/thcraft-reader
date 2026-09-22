#pragma once

#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
#include <SdCardFontManager.h>
#endif
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
class NativeTextEngine;
#endif

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty();

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  /// Close SD handles and remove custom mappings before mutation/USB handoff.
  /// Does not rediscover files or alter the saved selection; caller holds no storage lock.
  void releaseNativeFonts();
  /// Validate staged or installed SFNT bytes without registering a family.
  TextStatus validateNativeFontFile(std::string_view path, std::span<const NativeVariation> axes = {});
  /// Translated diagnostic, once per boot/selection failure; nullptr when none is pending.
  const char* takeNotice();
#endif

 private:
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched script fallback for the corresponding UI font, so book
  // titles/list rows in scripts the built-ins lack (CJK, Greek, Cyrillic, ...)
  // render at the same size as the surrounding Latin UI text. No-op when no SD
  // family is loaded. Safe to call repeatedly (sizes already loaded are
  // reused).
  void setupUiFallbacks(GfxRenderer& renderer);
#endif

  SdCardFontRegistry registry_;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  NativeTextEngine* nativeEngine_ = nullptr;
  char loadedFamily_[32]{};
  char attemptedFamily_[32]{};
  int readerIds_[4]{};
  bool attemptedSerif_ = false;
  bool noticeIssued_ = false;
  bool nativeReloadNeeded_ = true;
  enum class Notice : uint8_t { None, Migration, Invalid, Memory, Render };
  Notice notice_ = Notice::None;
#else
  SdCardFontManager manager_;
#endif
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
