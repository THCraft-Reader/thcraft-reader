#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
#include <ArduinoJson.h>
#include <NativePlatform.h>

#include <algorithm>
#include <cmath>
#ifdef ARDUINO
#include <esp_rom_crc.h>
#endif
#include "SdCardFontSystem.h"

namespace {
constexpr size_t kFamilyBytes = sizeof(SETTINGS.sdFontFamilyName);
constexpr size_t kFilenameBytes = kFamilyBytes + sizeof("-BoldItalic.ttf");
constexpr size_t kPathBytes = 112;
constexpr const char* kStyles[] = {"Regular", "Bold", "Italic", "BoldItalic"};
constexpr const char* kJsonStyles[] = {"regular", "bold", "italic", "boldItalic"};
constexpr const char* kSidecar = "native-font.json";
// Four styles, two formats, and every case combination of the three extension letters.
constexpr size_t kOwnedFiles = 4 * 2 * 8 + 1;

class NativeMutationLock {
 public:
  NativeMutationLock() { nativeTextLock(); }
  ~NativeMutationLock() { nativeTextUnlock(); }
};
class NativeJsonAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t bytes) override { return native_text_malloc(bytes); }
  void* reallocate(void* data, size_t bytes) override { return native_text_realloc(data, bytes); }
  void deallocate(void* data) override { native_text_free(data); }
};

FontInstaller::Error installError(TextStatus status) {
  if (status == TextStatus::Ok) return FontInstaller::Error::OK;
  if (status == TextStatus::OutOfMemory) return FontInstaller::Error::OUT_OF_MEMORY;
  if (status == TextStatus::StorageError) return FontInstaller::Error::SD_WRITE_ERROR;
  return FontInstaller::Error::INVALID_FILE;
}

bool makePath(const char* directory, const char* filename, const char* suffix, char* output, size_t capacity) {
  const int count = snprintf(output, capacity, "%s/%s%s", directory, filename, suffix);
  return count >= 0 && static_cast<size_t>(count) < capacity;
}

bool validNativePath(const char* path) {
  if (!path) return false;
  const char* family = nullptr;
  for (const char* root : {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE}) {
    const size_t length = strlen(root);
    if (!strncmp(path, root, length) && path[length] == '/') family = path + length + 1;
  }
  if (!family) return false;
  const char* slash = strchr(family, '/');
  if (!slash || static_cast<size_t>(slash - family) >= kFamilyBytes) return false;
  char requested[kFamilyBytes];
  memcpy(requested, family, slash - family);
  requested[slash - family] = '\0';
  if (!FontInstaller::isValidFamilyName(requested)) return false;
  const char* filename = slash + 1;
  size_t bytes = strlen(filename);
  if (bytes > 5 && !strcmp(filename + bytes - 5, ".part")) bytes -= 5;
  if (bytes >= kFilenameBytes) return false;
  char name[kFilenameBytes], parsed[kFamilyBytes], canonical[kFilenameBytes];
  memcpy(name, filename, bytes);
  name[bytes] = '\0';
  uint8_t style;
  return FontInstaller::parseNativeFilename(name, parsed, sizeof(parsed), style, canonical, sizeof(canonical)) &&
         !strcmp(parsed, requested);
}

struct NativeStyle {
  char filename[kFilenameBytes];
  NativeVariation axes[8];
  uint8_t axisCount;
  bool present, staged, published;
};
struct NativeBackup {
  char filename[kFilenameBytes];
  uint8_t style;
  bool replace, backed;
};

// Directory handles close before the caller mutates any entries.
FontInstaller::Error collectNativeFiles(const char* directory, const char* family, NativeBuffer<NativeBackup>& files) {
  files.clear();
  if (!Storage.exists(directory)) return FontInstaller::Error::OK;
  HalFile dir = Storage.open(directory);
  if (!dir || !dir.isDirectory()) return FontInstaller::Error::SD_WRITE_ERROR;
  char name[128], parsed[kFamilyBytes], canonical[kFilenameBytes];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool directoryEntry = entry.isDirectory();
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    entry.close();
    uint8_t style = 4;
    if (directoryEntry ||
        (strcmp(name, kSidecar) &&
         (!FontInstaller::parseNativeFilename(name, parsed, sizeof(parsed), style, canonical, sizeof(canonical)) ||
          strcmp(parsed, family))))
      continue;
    const size_t count = files.size();
    if (count == kOwnedFiles) return FontInstaller::Error::INVALID_FILE;
    if (!files.resize(count + 1)) return FontInstaller::Error::OUT_OF_MEMORY;
    auto& item = files[count];
    item = {};
    strcpy(item.filename, name);
    item.style = style;
  }
  return FontInstaller::Error::OK;
}

FontInstaller::Error checkStagedBytes(const char* path, const FontInstaller::NativeStyleFile& expected,
                                      NativeBuffer<uint8_t>& buffer) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) return FontInstaller::Error::SD_WRITE_ERROR;
  if (!expected.size || file.fileSize64() != expected.size) return FontInstaller::Error::INVALID_FILE;
  if (!buffer.resize(4096)) return FontInstaller::Error::OUT_OF_MEMORY;
  uint32_t remaining = expected.size, crc = 0xffffffffu;
  while (remaining) {
    const size_t wanted = std::min<size_t>(remaining, buffer.size());
    const int count = file.read(buffer.data(), wanted);
    if (count <= 0 || static_cast<size_t>(count) > wanted) return FontInstaller::Error::SD_WRITE_ERROR;
    if (expected.verifyChecksum) {
#ifdef ARDUINO
      crc = ~esp_rom_crc32_le(~crc, buffer.data(), static_cast<uint32_t>(count));
#else
      for (int i = 0; i < count; ++i) {
        crc ^= buffer[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
      }
#endif
    }
    remaining -= static_cast<uint32_t>(count);
    nativeTextYield();
  }
  if (file.read() >= 0 || file.fileSize64() != expected.size) return FontInstaller::Error::INVALID_FILE;
  if (!file.close()) return FontInstaller::Error::SD_WRITE_ERROR;
  return expected.verifyChecksum && (crc ^ 0xffffffffu) != expected.crc32 ? FontInstaller::Error::INVALID_FILE
                                                                          : FontInstaller::Error::OK;
}

FontInstaller::Error writeSidecar(const char* path, std::span<const NativeStyle> styles) {
  NativeJsonAllocator allocator;
  JsonDocument document(&allocator);
  document["version"] = 1;
  JsonObject entries = document["styles"].to<JsonObject>();
  for (uint8_t style = 0; style < styles.size(); ++style) {
    const auto& file = styles[style];
    if (!file.present || !file.axisCount) continue;
    JsonObject entry = entries[kJsonStyles[style]].to<JsonObject>();
    entry["file"] = file.filename;
    JsonObject axes = entry["axes"].to<JsonObject>();
    for (uint8_t i = 0; i < file.axisCount; ++i) {
      char tag[5];
      for (unsigned j = 0; j < 4; ++j) tag[j] = static_cast<char>(file.axes[i].tag >> (24 - j * 8));
      tag[4] = '\0';
      axes[tag] = file.axes[i].value;
    }
  }
  if (document.overflowed()) return FontInstaller::Error::OUT_OF_MEMORY;
  HalFile output;
  if (!Storage.openFileForWrite("FONT", path, output)) return FontInstaller::Error::SD_WRITE_ERROR;
  const size_t expected = measureJson(document);
  const size_t written = serializeJson(document, output);
  output.flush();
  const bool complete = written == expected && output.fileSize64() == expected;
  const bool closed = output.close();
  return complete && closed ? FontInstaller::Error::OK : FontInstaller::Error::SD_WRITE_ERROR;
}
}  // namespace
#endif

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

bool FontInstaller::isValidFamilyName(const char* name) {
  if (!name || !*name) return false;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  size_t length = 0;
#endif
  for (const char* p = name; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    if (++length >= kFamilyBytes) return false;
#endif
  }
  return true;
}

bool FontInstaller::isValidFontFilename(const char* name) {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  char family[kFamilyBytes], canonical[kFilenameBytes];
  uint8_t style;
  return parseNativeFilename(name, family, sizeof(family), style, canonical, sizeof(canonical));
#else
  if (!name || !*name) return false;
  const size_t length = strlen(name);
  static constexpr size_t extensionLength = sizeof(".cpfont") - 1;
  if (length <= extensionLength || strcmp(name + length - extensionLength, ".cpfont")) return false;
  for (size_t i = 0; i < length - extensionLength; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (!std::isalnum(c) && c != '-' && c != '_') return false;
  }
  return true;
#endif
}

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  lastError_ = Error::OK;
  if (!isValidFamilyName(familyName)) {
    lastError_ = Error::INVALID_FAMILY_NAME;
    return false;
  }
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  NativeMutationLock lock;
  if (!isFamilyInstalled(familyName) && registry_.getFamilyCount() >= SdCardFontRegistry::MAX_SD_FAMILIES) {
    lastError_ = Error::MAX_FAMILIES_REACHED;
    return false;
  }
  sdFontSystem.releaseNativeFonts();
#endif
  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  char directory[160];
  const int count = snprintf(directory, sizeof(directory), "%s/%s", root, familyName);
  if (count < 0 || static_cast<size_t>(count) >= sizeof(directory)) {
    lastError_ = Error::INVALID_FAMILY_NAME;
    return false;
  }
  if ((!Storage.exists(root) && !Storage.mkdir(root)) || (!Storage.exists(directory) && !Storage.mkdir(directory))) {
    lastError_ = Error::SD_WRITE_ERROR;
    return false;
  }
  HalFile dir = Storage.open(directory);
  if (!dir || !dir.isDirectory()) {
    lastError_ = Error::SD_WRITE_ERROR;
    return false;
  }
  return true;
}

bool FontInstaller::validateFontFile(const char* path) {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  lastError_ = validNativePath(path) ? installError(sdFontSystem.validateNativeFontFile(path)) : Error::INVALID_FILE;
  return lastError_ == Error::OK;
#else
  HalFile file;
  if (!path || !Storage.openFileForRead("FONT", path, file)) {
    lastError_ = Error::SD_WRITE_ERROR;
    return false;
  }
  uint8_t magic[CPFONT_MAGIC_LEN];
  const int count = file.read(magic, sizeof(magic));
  file.close();
  lastError_ = count == sizeof(magic) && !memcmp(magic, "CPFONT\0\0", sizeof(magic)) ? Error::OK : Error::INVALID_FILE;
  return lastError_ == Error::OK;
#endif
}

void FontInstaller::buildFontPath(const char* family, const char* filename, char* output, size_t capacity) {
  if (!output || !capacity) return;
  output[0] = '\0';
  if (!isValidFamilyName(family) || !isValidFontFilename(filename)) return;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  char parsed[kFamilyBytes], canonical[kFilenameBytes];
  uint8_t style;
  if (!parseNativeFilename(filename, parsed, sizeof(parsed), style, canonical, sizeof(canonical)) ||
      strcmp(parsed, family))
    return;
  filename = canonical;
#endif
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  const int count = snprintf(output, capacity, "%s/%s/%s", root, family, filename);
  if (count < 0 || static_cast<size_t>(count) >= capacity) output[0] = '\0';
}

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
bool FontInstaller::parseNativeFilename(const char* filename, char* family, size_t familyCapacity, uint8_t& style,
                                        char* canonical, size_t canonicalCapacity) {
  if (!filename || !family || !canonical || !familyCapacity || !canonicalCapacity) return false;
  size_t length = 0;
  while (length < kFilenameBytes && filename[length]) ++length;
  if (length < 6 || length == kFilenameBytes || filename[length - 4] != '.') return false;
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; };
  const char ext = lower(filename[length - 3]);
  if ((ext != 't' && ext != 'o') || lower(filename[length - 2]) != 't' || lower(filename[length - 1]) != 'f')
    return false;
  for (uint8_t candidate = 0; candidate < 4; ++candidate) {
    const size_t suffix = strlen(kStyles[candidate]);
    if (length <= suffix + 5) continue;
    const size_t familyLength = length - suffix - 5;
    if (filename[familyLength] != '-' || strncmp(filename + familyLength + 1, kStyles[candidate], suffix)) continue;
    if (familyLength >= kFamilyBytes || familyLength >= familyCapacity || length >= canonicalCapacity) return false;
    char parsed[kFamilyBytes];
    memcpy(parsed, filename, familyLength);
    parsed[familyLength] = '\0';
    if (!isValidFamilyName(parsed)) return false;
    memcpy(family, parsed, familyLength + 1);
    memmove(canonical, filename, length + 1);
    canonical[length - 3] = ext;
    canonical[length - 2] = 't';
    canonical[length - 1] = 'f';
    style = candidate;
    return true;
  }
  return false;
}

bool FontInstaller::buildStagingFontPath(const char* family, const char* canonical, char* output, size_t capacity) {
  if (!output || !capacity) return false;
  buildFontPath(family, canonical, output, capacity);
  const size_t length = strlen(output);
  if (!length || length + sizeof(".part") > capacity) {
    output[0] = '\0';
    return false;
  }
  memcpy(output + length, ".part", sizeof(".part"));
  return true;
}

void FontInstaller::discardNativeStaging(const char* family, std::span<const NativeStyleFile> files) {
  if (!isValidFamilyName(family)) return;
  NativeMutationLock lock;
  sdFontSystem.releaseNativeFonts();
  char path[kPathBytes];
  for (const auto& file : files) {
    if (buildStagingFontPath(family, file.filename, path, sizeof(path)) && Storage.exists(path) &&
        !Storage.remove(path))
      LOG_ERR("FONT", "Unable to discard staged font: %s", path);
  }
}

FontInstaller::Error FontInstaller::commitNativeFamily(const char* family, std::span<const NativeStyleFile> files,
                                                       bool replaceFamily) {
  lastError_ = Error::OK;
  if (!isValidFamilyName(family)) return lastError_ = Error::INVALID_FAMILY_NAME;
  if (files.empty() || files.size() > 4) return lastError_ = Error::INVALID_FILE;
  NativeMutationLock lock;
  NativeBuffer<NativeStyle> styles;
  if (!styles.resize(4)) return lastError_ = Error::OUT_OF_MEMORY;
  for (auto& style : styles.span()) style = {};
  char parsed[kFamilyBytes], canonical[kFilenameBytes];
  // Reject an invalid request before touching any existing family or staged file.
  for (const auto& file : files) {
    uint8_t style;
    if (!parseNativeFilename(file.filename, parsed, sizeof(parsed), style, canonical, sizeof(canonical)) ||
        strcmp(parsed, family) || style != file.style || styles[style].present || file.axes.size() > 8 || !file.size)
      return lastError_ = Error::INVALID_FILE;
    for (size_t i = 0; i < file.axes.size(); ++i) {
      if (!std::isfinite(file.axes[i].value)) return lastError_ = Error::INVALID_FILE;
      for (unsigned byte = 0; byte < 4; ++byte) {
        const uint8_t c = static_cast<uint8_t>(file.axes[i].tag >> (byte * 8));
        if (c < 0x20 || c > 0x7e) return lastError_ = Error::INVALID_FILE;
      }
      for (size_t j = 0; j < i; ++j)
        if (file.axes[i].tag == file.axes[j].tag) return lastError_ = Error::INVALID_FILE;
    }
    auto& target = styles[style];
    target.present = target.staged = true;
    strcpy(target.filename, canonical);
    target.axisCount = static_cast<uint8_t>(file.axes.size());
    if (!file.axes.empty()) memcpy(target.axes, file.axes.data(), file.axes.size_bytes());
  }

  sdFontSystem.releaseNativeFonts();
  registry_.discover();
  if (!ensureFamilyDir(family)) {
    discardNativeStaging(family, files);
    return lastError_;
  }
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  char directory[kPathBytes], path[kPathBytes], other[kPathBytes];
  snprintf(directory, sizeof(directory), "%s/%s", root, family);
  NativeBuffer<NativeBackup> backups;
  lastError_ = collectNativeFiles(directory, family, backups);
  NativeBuffer<uint8_t> readBuffer;
  bool stagedSidecar = false, publishedSidecar = false;

  const auto perform = [&]() -> Error {
    if (lastError_ != Error::OK) return lastError_;
    const auto* previous = registry_.findFamily(family);
    for (auto& old : backups.span()) {
      old.replace = old.style == 4 || replaceFamily || styles[old.style].staged;
      if (old.replace) {
        makePath(directory, old.filename, ".bak", path, sizeof(path));
        if (Storage.exists(path)) return Error::SD_WRITE_ERROR;
        continue;
      }
      auto& retained = styles[old.style];
      if (retained.present || !previous || previous->nativeStatus != TextStatus::Ok) return Error::INVALID_FILE;
      const auto* metadata = previous->findFile(12, old.style);
      if (!metadata) return Error::INVALID_FILE;
      retained.present = true;
      strcpy(retained.filename, old.filename);
      retained.axisCount = metadata->axisCount;
      memcpy(retained.axes, metadata->axes, metadata->axisCount * sizeof(NativeVariation));
    }
    if (!styles[0].present) return Error::INVALID_FILE;
    for (const auto& requested : files) {
      makePath(directory, styles[requested.style].filename, ".part", path, sizeof(path));
      const Error result = checkStagedBytes(path, requested, readBuffer);
      if (result != Error::OK) return result;
    }
    for (const auto& style : styles.span()) {
      if (!style.present) continue;
      makePath(directory, style.filename, style.staged ? ".part" : "", path, sizeof(path));
      const auto result = installError(sdFontSystem.validateNativeFontFile(path, {style.axes, style.axisCount}));
      if (result != Error::OK) return result;
    }
    readBuffer.reset();
    for (const auto& style : styles.span())
      if (style.present && style.axisCount) stagedSidecar = true;
    if (stagedSidecar) {
      makePath(directory, kSidecar, ".part", path, sizeof(path));
      const Error result = writeSidecar(path, styles.span());
      if (result != Error::OK) return result;
    }
    // All new and retained sources have passed actual-byte and FT/axis validation.
    // No native font or directory handle remains open during the rename phase.
    for (auto& old : backups.span()) {
      if (!old.replace) continue;
      makePath(directory, old.filename, "", path, sizeof(path));
      makePath(directory, old.filename, ".bak", other, sizeof(other));
      if (!Storage.rename(path, other)) return Error::SD_WRITE_ERROR;
      old.backed = true;
    }
    for (auto& style : styles.span()) {
      if (!style.staged) continue;
      makePath(directory, style.filename, ".part", path, sizeof(path));
      makePath(directory, style.filename, "", other, sizeof(other));
      if (!Storage.rename(path, other)) return Error::SD_WRITE_ERROR;
      style.published = true;
    }
    if (stagedSidecar) {
      makePath(directory, kSidecar, ".part", path, sizeof(path));
      makePath(directory, kSidecar, "", other, sizeof(other));
      if (!Storage.rename(path, other)) return Error::SD_WRITE_ERROR;
      publishedSidecar = true;
    }
    return Error::OK;
  };
  lastError_ = perform();
  if (lastError_ != Error::OK) {
    if (publishedSidecar) {
      makePath(directory, kSidecar, "", path, sizeof(path));
      Storage.remove(path);
    }
    for (const auto& style : styles.span()) {
      if (!style.published) continue;
      makePath(directory, style.filename, "", path, sizeof(path));
      if (!Storage.remove(path)) LOG_ERR("FONT", "Cannot remove failed publication: %s", path);
    }
    for (size_t i = backups.size(); i > 0; --i) {
      const auto& old = backups[i - 1];
      if (!old.backed) continue;
      makePath(directory, old.filename, ".bak", path, sizeof(path));
      makePath(directory, old.filename, "", other, sizeof(other));
      if (!Storage.rename(path, other)) LOG_ERR("FONT", "Cannot restore backup; preserved at %s", path);
    }
    discardNativeStaging(family, files);
    if (stagedSidecar) {
      makePath(directory, kSidecar, ".part", path, sizeof(path));
      Storage.remove(path);
    }
    sdFontSystem.markRegistryDirty();
    return lastError_;
  }
  for (const auto& old : backups.span()) {
    if (!old.backed) continue;
    makePath(directory, old.filename, ".bak", path, sizeof(path));
    if (!Storage.remove(path)) LOG_ERR("FONT", "Installed font; could not remove backup: %s", path);
  }
  refreshRegistry();
  return lastError_;
}
#endif

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  lastError_ = Error::OK;
  if (!isValidFamilyName(familyName)) return lastError_ = Error::INVALID_FAMILY_NAME;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  NativeMutationLock lock;
  sdFontSystem.releaseNativeFonts();
  NativeBuffer<NativeBackup> files;
#else
  bool removedAny = false;
#endif
  for (const char* root : {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE}) {
    char directory[160];
    snprintf(directory, sizeof(directory), "%s/%s", root, familyName);
    if (!Storage.exists(directory)) continue;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    const auto result = collectNativeFiles(directory, familyName, files);
    if (result != Error::OK) {
      lastError_ = result;
      break;
    }
    char path[kPathBytes];
    for (const auto& file : files.span()) {
      makePath(directory, file.filename, "", path, sizeof(path));
      if (!Storage.remove(path)) {
        lastError_ = Error::SD_WRITE_ERROR;
        break;
      }
    }
    if (lastError_ != Error::OK) break;
    // rmdir is intentionally nonrecursive: cpfont files, other files and children survive.
    Storage.rmdir(directory);
#else
    if (!Storage.removeDir(directory)) return lastError_ = Error::SD_WRITE_ERROR;
    removedAny = true;
#endif
  }
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  refreshRegistry();
#else
  if (removedAny && !strcmp(SETTINGS.sdFontFamilyName, familyName)) SETTINGS.clearSdFontFamily();
#endif
  return lastError_;
}

void FontInstaller::refreshRegistry() {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  NativeMutationLock lock;
  sdFontSystem.markRegistryDirty();
  if (&registry_ == &sdFontSystem.registry())
    sdFontSystem.refreshIfDirty();
  else
    registry_.discover();
#else
  registry_.discover();
#endif
}

bool FontInstaller::isFamilyInstalled(const char* familyName) const {
  return isValidFamilyName(familyName) && registry_.findFamily(familyName) != nullptr;
}
