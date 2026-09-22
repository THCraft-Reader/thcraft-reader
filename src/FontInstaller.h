#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>

/// Shared utility for font installation (device download + browser upload).
/// Handles directory creation, file validation, deletion, and registry refresh.
class FontInstaller {
 public:
  enum class Error {
    OK,
    INVALID_FAMILY_NAME,
    INVALID_FILE,
    SD_WRITE_ERROR,
    MAX_FAMILIES_REACHED,
    OUT_OF_MEMORY,
  };

  explicit FontInstaller(SdCardFontRegistry& registry);

  /// Validate a family name: alphanumeric + hyphen + underscore only, no path traversal.
  static bool isValidFamilyName(const char* name);

  /// Validate the target's font filename and reject path traversal.
  static bool isValidFontFilename(const char* name);

  /// Ensure /<root>/<family>/ exists, where <root> is /.fonts (preferred) or /fonts.
  /// Re-uses the existing root if the family is already installed; otherwise
  /// creates it under SdCardFontRegistry::defaultWriteRoot().
  bool ensureFamilyDir(const char* familyName);

  /// Validate font contents using legacy magic or the native SFNT/FreeType validator.
  bool validateFontFile(const char* path);

  /// Build the full SD path for a font file.
  /// Writes "/<root>/<family>/<filename>" to outBuf, choosing <root> the same
  /// way ensureFamilyDir does (existing install dir, else default-write root).
  static void buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize);

  /// Delete the target's font files. Native deletion preserves unrelated files and
  /// the saved family selection; legacy deletion clears the active selection.
  Error deleteFamily(const char* familyName);

  /// Re-run registry discovery to pick up new/removed fonts.
  void refreshRegistry();

  /// Check whether a family name already exists in the registry.
  bool isFamilyInstalled(const char* familyName) const;

  Error lastError() const { return lastError_; }

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  struct NativeStyleFile {
    const char* filename;
    uint8_t style;
    std::span<const NativeVariation> axes;
    uint32_t size = 0, crc32 = 0;
    bool verifyChecksum = false;
  };

  static bool parseNativeFilename(const char* filename, char* family, size_t familyCapacity, uint8_t& style,
                                  char* canonical, size_t canonicalCapacity);
  static bool buildStagingFontPath(const char* family, const char* canonical, char* output, size_t capacity);
  /// Publish one complete request; staging files must already be closed.
  /// replaceFamily replaces all native styles; false merges untouched styles/axes.
  Error commitNativeFamily(const char* family, std::span<const NativeStyleFile> files, bool replaceFamily);
  /// Only removes this request's .part files, never published files or backups.
  void discardNativeStaging(const char* family, std::span<const NativeStyleFile> files);
#endif

 private:
  SdCardFontRegistry& registry_;
  Error lastError_ = Error::OK;

  static constexpr const char* CPFONT_MAGIC = "CPFONT\0";
  static constexpr size_t CPFONT_MAGIC_LEN = 8;
};
