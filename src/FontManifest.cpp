#include "FontManifest.h"

#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "FontInstaller.h"

namespace {
// ArduinoJson's document is temporary; cap its complete allocation footprint,
// including its strings, before constructing the compact retained arena.
class ManifestJsonAllocator final : public ArduinoJson::Allocator {
  struct alignas(std::max_align_t) Header {
    size_t size;
  };
  static constexpr size_t LIMIT = 1024 * 1024;
  size_t used_ = 0;

 public:
  void* allocate(size_t size) override {
    if (size > SIZE_MAX - sizeof(Header) || size + sizeof(Header) > LIMIT - used_) return nullptr;
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!header) return nullptr;
    header->size = size;
    used_ += sizeof(Header) + size;
    return header + 1;
  }
  void deallocate(void* pointer) override {
    if (!pointer) return;
    auto* header = static_cast<Header*>(pointer) - 1;
    used_ -= sizeof(Header) + header->size;
    std::free(header);
  }
  void* reallocate(void* pointer, size_t size) override {
    if (!pointer) return allocate(size);
    auto* old = static_cast<Header*>(pointer) - 1;
    const size_t previous = old->size;
    if (size > LIMIT - (used_ - previous) || size > SIZE_MAX - sizeof(Header)) return nullptr;
    auto* header = static_cast<Header*>(std::realloc(old, sizeof(Header) + size));
    if (!header) return nullptr;
    header->size = size;
    used_ = used_ - previous + size;
    return header + 1;
  }
};

class ManifestReader {
  const char* memory_ = nullptr;
  HalFile* file_ = nullptr;
  size_t remaining_;
  uint8_t buffer_[512]{};
  size_t position_ = 0, buffered_ = 0;

 public:
  bool failed = false;
  ManifestReader(const char* data, size_t size) : memory_(data), remaining_(size) {}
  explicit ManifestReader(HalFile& file) : file_(&file), remaining_(file.fileSize()) {}
  int read() {
    if (memory_) return remaining_ ? (--remaining_, static_cast<uint8_t>(*memory_++)) : -1;
    if (position_ == buffered_) {
      if (!remaining_ || failed) return -1;
      const size_t wanted = std::min(remaining_, sizeof(buffer_));
      const int count = file_->read(buffer_, wanted);
      if (count <= 0 || static_cast<size_t>(count) > wanted) {
        failed = true;
        return -1;
      }
      buffered_ = static_cast<size_t>(count);
      remaining_ -= buffered_;
      position_ = 0;
    }
    return buffer_[position_++];
  }
  size_t readBytes(char* data, size_t size) {
    size_t count = 0;
    for (; count < size; ++count) {
      const int byte = read();
      if (byte < 0) break;
      data[count] = static_cast<char>(byte);
    }
    return count;
  }
  bool atEnd() {
    for (int byte = read(); byte >= 0; byte = read()) {
      if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t') return false;
    }
    return !failed;
  }
};

bool text(JsonVariantConst value, size_t maximum, bool allowEmpty = false) {
  if (!value.is<const char*>()) return false;
  const JsonString string = value.as<JsonString>();
  return (allowEmpty || string.size()) && string.size() <= maximum && std::strlen(string.c_str()) == string.size();
}

bool sameName(const char* left, const char* right) {
  for (; *left && *right; ++left, ++right) {
    const char a = *left >= 'A' && *left <= 'Z' ? *left + ('a' - 'A') : *left;
    const char b = *right >= 'A' && *right <= 'Z' ? *right + ('a' - 'A') : *right;
    if (a != b) return false;
  }
  return *left == *right;
}

bool httpsUrl(const char* url) {
  if (std::strncmp(url, "https://", 8)) return false;
  const char* host = url + 8;
  const char* end = std::strchr(host, '/');
  if (!end || end == host) return false;
  for (const char* p = host; p != end; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
      return false;
  }
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(end); *p; ++p) {
    if (*p <= 0x20 || *p >= 0x7f || *p == '\\' || *p == '#') return false;
  }
  return true;
}

FontManifest::Error jsonError(DeserializationError error) {
  return error == DeserializationError::NoMemory ? FontManifest::Error::OutOfMemory : FontManifest::Error::Invalid;
}
}  // namespace

void FontManifest::clear() {
  arena_.reset();
  families_.reset();
  files_.reset();
  arenaUsed_ = arenaCapacity_ = familyCount_ = fileCount_ = baseUrl_ = groupCount_ = 0;
}

FontManifest::Error FontManifest::load(const char* json, const size_t length) {
  if (!json || !length || length > MAX_DOCUMENT_BYTES) return Error::Invalid;
  ManifestJsonAllocator allocator;
  JsonDocument document(&allocator);
  ManifestReader input(json, length);
  const auto error = deserializeJson(document, input, DeserializationOption::NestingLimit(8));
  if (error) return jsonError(error);
  if (!input.atEnd()) return Error::Invalid;
  return parseDocument(document.as<JsonVariantConst>());
}

FontManifest::Error FontManifest::load(HalFile& file) {
  if (!file || !file.fileSize() || file.fileSize() > MAX_DOCUMENT_BYTES) return Error::Invalid;
  ManifestJsonAllocator allocator;
  JsonDocument document(&allocator);
  ManifestReader input(file);
  const auto error = deserializeJson(document, input, DeserializationOption::NestingLimit(8));
  if (input.failed) return Error::StorageError;
  if (error) return jsonError(error);
  if (!input.atEnd()) return input.failed ? Error::StorageError : Error::Invalid;
  return parseDocument(document.as<JsonVariantConst>());
}

bool FontManifest::intern(const char* value, StrRef& ref) {
  if (!value || !*value) {
    ref = 0;
    return true;
  }
  const size_t bytes = std::strlen(value) + 1;
  if (bytes > arenaCapacity_ - arenaUsed_) return false;
  ref = arenaUsed_;
  std::memcpy(arena_.get() + arenaUsed_, value, bytes);
  arenaUsed_ += static_cast<uint32_t>(bytes);
  return true;
}

FontManifest::Error FontManifest::parseDocument(JsonVariantConst document) {
  if (!document.is<JsonObjectConst>() || !document["version"].is<uint32_t>() ||
      document["version"].as<uint32_t>() != VERSION)
    return Error::Invalid;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  if (!text(document["format"], 16) || std::strcmp(document["format"], "opentype")) return Error::Invalid;
  constexpr size_t MAX_FILES_PER_FAMILY = 4;
#else
  if ((!document["format"].isNull() && (!text(document["format"], 16) || std::strcmp(document["format"], "cpfont"))) ||
      !text(document["baseUrl"], 2048))
    return Error::Invalid;
  constexpr size_t MAX_FILES_PER_FAMILY = 32;
#endif
  if (!document["families"].is<JsonArrayConst>() ||
      (!document["scriptGroups"].isNull() && !document["scriptGroups"].is<JsonArrayConst>()))
    return Error::Invalid;
  const auto groups = document["scriptGroups"].as<JsonArrayConst>();
  const auto families = document["families"].as<JsonArrayConst>();
  if (groups.size() > MAX_SCRIPT_GROUPS || families.size() > MAX_FAMILIES) return Error::Invalid;
  size_t arenaBytes = 1;
  size_t fileCount = 0;
  uint32_t catalogueBytes = 0;
  auto countString = [&arenaBytes](const char* string) {
    const size_t bytes = std::strlen(string) + 1;
    if (bytes > MAX_DOCUMENT_BYTES - arenaBytes) return false;
    arenaBytes += bytes;
    return true;
  };
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
  if (!countString(document["baseUrl"])) return Error::Invalid;
#endif
  for (size_t index = 0; index < groups.size(); ++index) {
    const auto group = groups[index];
    if (!text(group["tag"], 32) || !text(group["label"], 128) || !countString(group["label"])) return Error::Invalid;
    for (size_t previous = 0; previous < index; ++previous) {
      if (!std::strcmp(groups[previous]["tag"], group["tag"])) return Error::Invalid;
    }
  }
  for (size_t index = 0; index < families.size(); ++index) {
    const auto family = families[index];
    if (!text(family["name"], 127) || !FontInstaller::isValidFamilyName(family["name"]) ||
        (!family["description"].isNull() && !text(family["description"], 512, true)) ||
        !family["files"].is<JsonArrayConst>() ||
        (!family["scripts"].isNull() && !family["scripts"].is<JsonArrayConst>()))
      return Error::Invalid;
    for (size_t previous = 0; previous < index; ++previous) {
      if (sameName(families[previous]["name"], family["name"])) return Error::Invalid;
    }
    if (!countString(family["name"]) || !countString(family["description"] | "")) return Error::Invalid;
    uint32_t scriptMask = 0;
    for (JsonVariantConst script : family["scripts"].as<JsonArrayConst>()) {
      if (!text(script, 32)) return Error::Invalid;
      bool found = false;
      for (size_t group = 0; group < groups.size(); ++group) {
        if (std::strcmp(script, groups[group]["tag"])) continue;
        const uint32_t bit = uint32_t{1} << group;
        if (scriptMask & bit) return Error::Invalid;
        scriptMask |= bit;
        found = true;
        break;
      }
      if (!found) return Error::Invalid;
    }
    const auto files = family["files"].as<JsonArrayConst>();
    if (files.size() == 0 || files.size() > MAX_FILES_PER_FAMILY) return Error::Invalid;
    fileCount += files.size();
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    uint8_t styleMask = 0;
#endif
    for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
      const auto file = files[fileIndex];
      if (!text(file["name"], 127) || !FontInstaller::isValidFontFilename(file["name"]) ||
          !file["size"].is<uint32_t>() || !file["size"].as<uint32_t>() || !file["crc32"].is<uint32_t>() ||
          !countString(file["name"]))
        return Error::Invalid;
      const uint32_t size = file["size"].as<uint32_t>();
      if (size > UINT32_MAX - catalogueBytes) return Error::Invalid;
      catalogueBytes += size;
      for (size_t previous = 0; previous < fileIndex; ++previous) {
        if (sameName(files[previous]["name"], file["name"])) return Error::Invalid;
      }
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
      char parsedFamily[32], canonical[64];
      uint8_t style = 0;
      if (size < 12 ||
          !FontInstaller::parseNativeFilename(file["name"], parsedFamily, sizeof(parsedFamily), style, canonical,
                                              sizeof(canonical)) ||
          std::strcmp(parsedFamily, family["name"]) || (styleMask & (1u << style)) || !text(file["url"], 2048) ||
          !httpsUrl(file["url"]) || !countString(file["url"]))
        return Error::Invalid;
      styleMask |= 1u << style;
      if (!file["axes"].isNull() && !file["axes"].is<JsonObjectConst>()) return Error::Invalid;
      const auto axes = file["axes"].as<JsonObjectConst>();
      if (axes.size() > 8) return Error::Invalid;
      for (JsonPairConst axis : axes) {
        if (axis.key().size() != 4 || !axis.value().is<double>() || !std::isfinite(axis.value().as<float>()))
          return Error::Invalid;
        for (size_t letter = 0; letter < 4; ++letter) {
          const unsigned char c = axis.key().c_str()[letter];
          if (c < 0x20 || c > 0x7e) return Error::Invalid;
        }
      }
#endif
    }
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    if (!(styleMask & 1)) return Error::Invalid;
#endif
  }
  // All counts and byte sums are bounded before any retained allocation. The
  // old published catalogue remains usable after malformed input or OOM.
  FontManifest next;
  next.arena_ = makeUniqueNoThrow<char[]>(arenaBytes);
  if (!next.arena_) return Error::OutOfMemory;
  if (families.size()) {
    next.families_ = makeUniqueNoThrow<Family[]>(families.size());
    if (!next.families_) return Error::OutOfMemory;
  }
  if (fileCount) {
    next.files_ = makeUniqueNoThrow<File[]>(fileCount);
    if (!next.files_) return Error::OutOfMemory;
  }
  next.arena_[0] = 0;
  next.arenaUsed_ = 1;
  next.arenaCapacity_ = static_cast<uint32_t>(arenaBytes);
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
  if (!next.intern(document["baseUrl"], next.baseUrl_)) return Error::Invalid;
#endif
  for (JsonVariantConst group : groups) {
    if (!next.intern(group["label"], next.groups_[next.groupCount_++])) return Error::Invalid;
  }
  for (JsonVariantConst sourceFamily : families) {
    auto& family = next.families_[next.familyCount_++];
    if (!next.intern(sourceFamily["name"], family.name) ||
        !next.intern(sourceFamily["description"] | "", family.description))
      return Error::Invalid;
    for (JsonVariantConst script : sourceFamily["scripts"].as<JsonArrayConst>()) {
      for (size_t group = 0; group < groups.size(); ++group) {
        if (!std::strcmp(script, groups[group]["tag"])) family.scriptMask |= uint32_t{1} << group;
      }
    }
    family.fileStart = next.fileCount_;
    for (JsonVariantConst sourceFile : sourceFamily["files"].as<JsonArrayConst>()) {
      auto& file = next.files_[next.fileCount_++];
      file.size = sourceFile["size"].as<uint32_t>();
      file.crc32 = sourceFile["crc32"].as<uint32_t>();
      family.totalSize += file.size;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
      char parsedFamily[32], canonical[64];
      if (!FontInstaller::parseNativeFilename(sourceFile["name"], parsedFamily, sizeof(parsedFamily), file.style,
                                              canonical, sizeof(canonical)) ||
          !next.intern(canonical, file.name) || !next.intern(sourceFile["url"], file.url))
        return Error::Invalid;
      for (JsonPairConst axis : sourceFile["axes"].as<JsonObjectConst>()) {
        uint32_t tag = 0;
        for (size_t letter = 0; letter < 4; ++letter)
          tag = (tag << 8) | static_cast<uint8_t>(axis.key().c_str()[letter]);
        file.axes[file.axisCount++] = {tag, axis.value().as<float>()};
      }
#else
      if (!next.intern(sourceFile["name"], file.name)) return Error::Invalid;
#endif
    }
    family.fileCount = next.fileCount_ - family.fileStart;
  }
  *this = std::move(next);
  return Error::Ok;
}
