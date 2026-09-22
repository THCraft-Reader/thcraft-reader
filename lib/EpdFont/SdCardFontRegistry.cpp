#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
#include <ArduinoJson.h>

#include <cmath>
#include <string_view>

namespace {
constexpr const char* NATIVE_STYLES[] = {"Regular", "Bold", "Italic", "BoldItalic"};
constexpr const char* JSON_STYLES[] = {"regular", "bold", "italic", "boldItalic"};

bool nativeSize(uint8_t size) {
  return size == 8 || size == 10 || size == 12 || size == 14 || size == 16 || size == 18;
}

bool nativeFilename(std::string_view filename, std::string_view family, uint8_t& style) {
  if (filename.size() < 5 || filename[filename.size() - 4] != '.') return false;
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
  const auto extension = filename.substr(filename.size() - 3);
  if (!((lower(extension[0]) == 't' || lower(extension[0]) == 'o') && lower(extension[1]) == 't' &&
        lower(extension[2]) == 'f'))
    return false;
  const auto stem = filename.substr(0, filename.size() - 4);
  if (stem.size() <= family.size() || stem.substr(0, family.size()) != family || stem[family.size()] != '-')
    return false;
  for (uint8_t i = 0; i < 4; ++i) {
    if (stem.substr(family.size() + 1) == NATIVE_STYLES[i]) {
      style = i;
      return true;
    }
  }
  return false;
}

class NativeJsonAllocator final : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t bytes) override { return native_text_malloc(bytes); }
  void* reallocate(void* data, size_t bytes) override { return native_text_realloc(data, bytes); }
  void deallocate(void* data) override { native_text_free(data); }
};

struct NativeJsonReader {
  const char* data;
  size_t size, position = 0;
  int read() { return position < size ? static_cast<uint8_t>(data[position++]) : -1; }
  size_t readBytes(char* output, size_t count) {
    count = std::min(count, size - position);
    memcpy(output, data + position, count);
    position += count;
    return count;
  }
};

TextStatus readNativeSidecar(const char* dirPath, SdCardFontFamilyInfo& family) {
  const size_t pathBytes = strlen(dirPath) + sizeof("/native-font.json");
  if (pathBytes > 1024) return TextStatus::CapacityExceeded;
  NativeBuffer<char> path;
  if (!path.resize(pathBytes)) return TextStatus::OutOfMemory;
  snprintf(path.data(), pathBytes, "%s/native-font.json", dirPath);
  if (!Storage.exists(path.data())) return TextStatus::Ok;
  HalFile file;
  if (!Storage.openFileForRead("SDREG", path.data(), file)) return TextStatus::StorageError;
  const uint64_t bytes = file.fileSize64();
  if (!bytes || bytes > 8192) return TextStatus::InvalidFont;
  NativeBuffer<char> json;
  if (!json.resize(static_cast<size_t>(bytes) + 1)) return TextStatus::OutOfMemory;
  if (file.read(json.data(), static_cast<size_t>(bytes)) != static_cast<int>(bytes)) return TextStatus::StorageError;
  json[bytes] = '\0';
  NativeJsonAllocator allocator;
  JsonDocument doc(&allocator);
  NativeJsonReader reader{json.data(), static_cast<size_t>(bytes)};
  const auto error = deserializeJson(doc, reader, DeserializationOption::NestingLimit(5));
  if (error) return error == DeserializationError::NoMemory ? TextStatus::OutOfMemory : TextStatus::InvalidFont;
  for (size_t i = reader.position; i < reader.size; ++i)
    if (reader.data[i] != ' ' && reader.data[i] != '\t' && reader.data[i] != '\r' && reader.data[i] != '\n')
      return TextStatus::InvalidFont;
  if (!doc["version"].is<int>() || doc["version"].as<int>() != 1 || !doc["styles"].is<JsonObject>())
    return TextStatus::InvalidFont;
  for (JsonPairConst pair : doc["styles"].as<JsonObjectConst>()) {
    uint8_t style = 4;
    for (uint8_t i = 0; i < 4; ++i)
      if (!strcmp(pair.key().c_str(), JSON_STYLES[i])) style = i;
    if (style == 4 || !pair.value().is<JsonObjectConst>()) return TextStatus::InvalidFont;
    auto description = pair.value().as<JsonObjectConst>();
    if (!description["file"].is<const char*>()) return TextStatus::InvalidFont;
    const char* filename = description["file"].as<const char*>();
    uint8_t filenameStyle = 4;
    if (!nativeFilename(filename, family.name, filenameStyle) || filenameStyle != style) return TextStatus::InvalidFont;
    SdCardFontFileInfo* target = nullptr;
    for (auto& candidate : family.files) {
      if (candidate.style == style &&
          std::string_view(candidate.path).substr(candidate.path.find_last_of('/') + 1) == filename)
        target = &candidate;
    }
    if (!target) return TextStatus::InvalidFont;
    if (!description["axes"].isNull() && !description["axes"].is<JsonObjectConst>()) return TextStatus::InvalidFont;
    for (JsonPairConst axis : description["axes"].as<JsonObjectConst>()) {
      const char* tag = axis.key().c_str();
      if (strlen(tag) != 4 || target->axisCount == 8 || !axis.value().is<double>()) return TextStatus::InvalidFont;
      uint32_t packed = 0;
      for (unsigned i = 0; i < 4; ++i) {
        if (tag[i] < 0x20 || tag[i] > 0x7e) return TextStatus::InvalidFont;
        packed = (packed << 8) | static_cast<uint8_t>(tag[i]);
      }
      const float value = axis.value().as<float>();
      if (!std::isfinite(value)) return TextStatus::InvalidFont;
      target->axes[target->axisCount++] = {packed, value};
    }
  }
  return TextStatus::Ok;
}
}  // namespace
#endif

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  if (nativeStatus != TextStatus::Ok || !nativeSize(size)) return nullptr;
  for (const auto& file : files)
    if (file.style == style) return &file;
  return nullptr;
#else
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
#endif
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findNearestSize(const uint8_t pointSize, const uint8_t style) const {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  (void)pointSize;
  return findFile(12, style);
#else
  // The reader stores an actual point size, so an exact match is the norm and
  // falls out of the delta search below (delta 0). The search only matters when
  // the size was carried over from a family that ships different sizes; the
  // caller then persists the snapped size (SdCardFontSystem::ensureLoaded).
  const SdCardFontFileInfo* best = nullptr;
  uint8_t bestDelta = 255;
  for (const auto& f : files) {
    if (f.style != style) continue;
    const uint8_t delta = f.pointSize > pointSize ? f.pointSize - pointSize : pointSize - f.pointSize;
    // Ties resolve to the smaller size, matching snapToNearestPointSize().
    if (!best || delta < bestDelta || (delta == bestDelta && f.pointSize < best->pointSize)) {
      best = &f;
      bestDelta = delta;
    }
  }
  return best;
#endif
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  return findFile(size) != nullptr;
#else
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
#endif
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  return findFile(12) ? std::vector<uint8_t>{12, 14, 16, 18} : std::vector<uint8_t>{};
#else
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
#endif
}

// --- SdCardFontRegistry ---

#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  // V4 .cpfont files bundle every style (regular/bold/italic/bold-italic) into
  // one file, so style is always 0 at the registry level. The per-style
  // bitstream is selected later by SdCardFont::getEpdFont(style). The `style`
  // field in SdCardFontFileInfo is reserved for future formats that split
  // styles across files; scanDirectory() defends against accidental
  // (pointSize, style) collisions in that scenario.
  style = 0;
  return true;
}
#endif

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  family.files.reserve(4);
#endif
  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Ignore dot-prefixed metadata files, including macOS resource forks.
    if (nameBuffer[0] == '.') continue;
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
    if (nameBuffer[0] == '_') continue;
#endif

    uint8_t size = 0, style = 0;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    if (!nativeFilename(nameBuffer, family.name, style)) continue;
#else
    if (!parseFilename(nameBuffer, size, style)) continue;
#endif

    // Reject duplicate (pointSize, style) entries in the same family. With
    // v4's bundle-everything design parseFilename always returns style=0, so
    // two files at the same size in the same family would silently shadow
    // each other in findFile(). Skip the duplicate and warn.
    bool duplicate = false;
    for (const auto& existing : family.files) {
      if (existing.pointSize == size && existing.style == style) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
      family.nativeStatus = TextStatus::InvalidFont;
#endif
      continue;
    }

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    info.pointSize = size;
    info.style = style;
    family.files.push_back(std::move(info));
  }
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  bool hasRegular = false;
  for (const auto& file : family.files)
    if (file.style == 0) hasRegular = true;
  if (!hasRegular) family.nativeStatus = TextStatus::InvalidFont;
  if (family.nativeStatus == TextStatus::Ok) family.nativeStatus = readNativeSidecar(dirPath, family);
  if (family.nativeStatus != TextStatus::Ok && !family.files.empty())
    LOG_ERR("SDREG", "Rejected native family metadata: %s", family.name.c_str());
#endif
}

// Scan a single root (e.g. "/.fonts") and append its families to `out`.
// Skips families whose names already exist in `out` (de-duplicates between
// the hidden and visible roots — first scan wins).
void SdCardFontRegistry::scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories inside the root (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.') continue;
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
      if (nameBuffer[0] == '_') continue;
#endif

      // De-dup by family name across roots.
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == nameBuffer) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      std::string subDirPath = std::string(rootPath) + "/" + nameBuffer;
      SdCardFontRegistry::scanDirectory(subDirPath.c_str(), family);

      if (!family.files.empty()) {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
        // Keep the same alphabetic 128-family policy without unbounded discovery growth.
        if (out.size() == MAX_SD_FAMILIES) {
          auto last =
              std::max_element(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
          if (family.name < last->name) *last = std::move(family);
          continue;
        }
#endif
        out.push_back(std::move(family));
        LOG_DBG("SDREG", "Found family: %s (%d files) in %s", out.back().name.c_str(),
                static_cast<int>(out.back().files.size()), rootPath);
      }
    } else {
      entry.close();
    }
  }
}

bool SdCardFontRegistry::discover() {
  families_.clear();
  families_.reserve(MAX_SD_FAMILIES);

  // Hidden root is scanned first so it wins on name collisions, matching the
  // sleep-folder pattern (/.sleep preferred over /sleep).
  scanRoot(FONTS_DIR_HIDDEN, families_);
  scanRoot(FONTS_DIR_VISIBLE, families_);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!familyName || !*familyName) return nullptr;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  // If exactly one of the roots already exists, keep using it. Otherwise
  // (neither exists, or both exist) prefer the hidden root for new installs.
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
