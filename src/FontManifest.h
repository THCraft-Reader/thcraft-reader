#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
#include <NativeTextTypes.h>
#endif

class HalFile;

// Shared parser for the compiled native catalogue and the legacy remote manifest.
// Data is published only after the complete document has passed validation.
class FontManifest {
 public:
  static constexpr uint32_t VERSION = 1;
  static constexpr size_t MAX_SCRIPT_GROUPS = 32;
  static constexpr size_t MAX_FAMILIES = 128;
  static constexpr size_t MAX_DOCUMENT_BYTES = 512 * 1024;
  using StrRef = uint32_t;
  enum class Error { Ok, Invalid, OutOfMemory, StorageError };

  struct File {
    StrRef name = 0;
    uint32_t size = 0;
    uint32_t crc32 = 0;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    StrRef url = 0;
    NativeVariation axes[8]{};
    uint8_t axisCount = 0;
    uint8_t style = 0;
#endif
  };
  struct Family {
    StrRef name = 0;
    StrRef description = 0;
    uint32_t fileStart = 0;
    uint32_t fileCount = 0;
    uint32_t totalSize = 0;
    uint32_t scriptMask = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  Error load(const char* json, size_t length);
  Error load(HalFile& file);
  void clear();
  const char* str(StrRef ref) const { return arena_ && ref < arenaUsed_ ? arena_.get() + ref : ""; }
  const char* baseUrl() const { return str(baseUrl_); }
  std::span<Family> families() { return {families_.get(), familyCount_}; }
  std::span<const Family> families() const { return {families_.get(), familyCount_}; }
  std::span<const File> files() const { return {files_.get(), fileCount_}; }
  std::span<const StrRef> groups() const { return {groups_, groupCount_}; }

 private:
  Error parseDocument(JsonVariantConst document);
  bool intern(const char* text, StrRef& ref);
  std::unique_ptr<char[]> arena_;
  std::unique_ptr<Family[]> families_;
  std::unique_ptr<File[]> files_;
  StrRef groups_[MAX_SCRIPT_GROUPS]{};
  StrRef baseUrl_ = 0;
  uint32_t arenaUsed_ = 0;
  uint32_t arenaCapacity_ = 0;
  uint32_t familyCount_ = 0;
  uint32_t fileCount_ = 0;
  uint8_t groupCount_ = 0;
};
