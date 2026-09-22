#include "FontWebApi.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "SdCardFontSystem.h"

namespace {
bool copyName(std::string_view source, char* target, size_t capacity) {
  if (source.empty() || source.size() >= capacity || source.find('\0') != std::string_view::npos) return false;
  std::memcpy(target, source.data(), source.size());
  target[source.size()] = 0;
  return true;
}
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
bool legacyFilename(const char* name, char* family, size_t capacity, char* canonical, uint8_t& pointSize) {
  const size_t length = std::strlen(name);
  if (length < 8) return false;
  std::strcpy(canonical, name);  // caller has checked both filename capacities
  const char* extension = name + length - 7;
  for (size_t i = 0; i < 7; ++i) {
    char c = extension[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != ".cpfont"[i]) return false;
  }
  std::memcpy(canonical + length - 7, ".cpfont", 7);
  if (!FontInstaller::isValidFontFilename(canonical)) return false;
  std::string_view stem(name, length - 7);
  const auto separator = stem.find_last_of("-_");
  if (separator == std::string_view::npos || !copyName(stem.substr(0, separator), family, capacity)) return false;
  const auto size = stem.substr(separator + 1);
  if (size.empty() || size.size() > 3) return false;
  unsigned value = 0;
  for (char c : size) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + c - '0';
  }
  if (!value || value > 255) return false;
  pointSize = static_cast<uint8_t>(value);
  return true;
}
#endif
}  // namespace

FontWebApi::FontWebApi(SdCardFontSystem& fonts) : fonts_(fonts), installer_(fonts.registry()) {}
FontWebApi::~FontWebApi() { discardStaging(); }

FontWebApi::Response FontWebApi::failure(int status, const char* message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  Response response{status, {}};
  serializeJson(doc, response.body);
  return response;
}

FontWebApi::Response FontWebApi::installerFailure(FontInstaller::Error error) const {
  switch (error) {
    case FontInstaller::Error::INVALID_FAMILY_NAME:
      return failure(400, "Invalid font family name.");
    case FontInstaller::Error::INVALID_FILE:
      return failure(400, "This font cannot be used. Check its format and include a Regular style.");
    case FontInstaller::Error::MAX_FAMILIES_REACHED:
      return failure(409, "The font family limit has been reached. Delete a family first.");
    case FontInstaller::Error::OUT_OF_MEMORY:
      return failure(503, "Not enough memory to install this font.");
    default:
      return failure(500, "Unable to update fonts on the SD card.");
  }
}

FontWebApi::Response FontWebApi::list() {
  fonts_.refreshIfDirty();
  JsonDocument doc;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  doc["format"] = "opentype";
  auto extensions = doc["extensions"].to<JsonArray>();
  extensions.add("ttf");
  extensions.add("otf");
#else
  doc["format"] = "cpfont";
  doc["extensions"].to<JsonArray>().add("cpfont");
#endif
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;
  doc["maxFamilyLength"] = MAX_FAMILY_LENGTH;
  doc["maxFiles"] = MAX_FILES;
  auto families = doc["families"].to<JsonArray>();
  for (const auto& family : fonts_.registry().getFamilies()) {
    auto item = families.add<JsonObject>();
    item["name"] = family.name;
    auto sizes = item["sizes"].to<JsonArray>();
    for (uint8_t size : family.availableSizes()) sizes.add(size);
    auto files = item["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      auto entry = files.add<JsonObject>();
      const char* basename = std::strrchr(file.path.c_str(), '/');
      entry["name"] = basename ? basename + 1 : file.path.c_str();
      HalFile input;
      if (!Storage.openFileForRead("WEB", file.path.c_str(), input)) {
        return failure(500, "Unable to read fonts from the SD card.");
      }
      entry["size"] = static_cast<uint32_t>(input.size());
      input.close();
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
      entry["style"] = file.style;
#endif
    }
  }
  if (doc.overflowed()) return failure(503, "Not enough memory to list fonts.");
  Response response{200, {}};
  serializeJson(doc, response.body);
  return response;
}

FontWebApi::Response FontWebApi::remove(std::string_view body) {
  if (active_) return failure(409, "Finish or cancel the current font upload first.");
  if (body.size() > 256) return failure(400, "Invalid request.");
  JsonDocument doc;
  if (deserializeJson(doc, body.data(), body.size(), DeserializationOption::NestingLimit(2)) ||
      !doc["family"].is<const char*>())
    return failure(400, "Invalid request.");
  const auto family = doc["family"].as<JsonString>();
  if (!family.size() || family.size() > MAX_FAMILY_LENGTH || std::strlen(family.c_str()) != family.size() ||
      !FontInstaller::isValidFamilyName(family.c_str())) {
    return failure(400, "Invalid font family name.");
  }
  const auto result = installer_.deleteFamily(family.c_str());
  if (result != FontInstaller::Error::OK) return installerFailure(result);
  fonts_.markRegistryDirty();
  return {200, "{\"ok\":true}"};
}

bool FontWebApi::beginUpload(std::string_view manifest) {
  discardStaging();
  for (auto& file : files_) file = {};
  family_[0] = 0;
  fileCount_ = 0;
  current_ = MAX_FILES;
  active_ = false;
  error_ = nullptr;
  if (manifest.empty() || manifest.size() > MAX_MANIFEST_BYTES)
    return fail(400, "Missing or oversized upload manifest.");
  JsonDocument doc;
  if (deserializeJson(doc, manifest.data(), manifest.size(), DeserializationOption::NestingLimit(4)) ||
      !doc["family"].is<const char*>() || !doc["files"].is<JsonArray>())
    return fail(400, "Invalid upload manifest.");
  const auto family = doc["family"].as<JsonString>();
  if (!copyName({family.c_str(), family.size()}, family_, sizeof(family_)) ||
      !FontInstaller::isValidFamilyName(family_))
    return fail(400, "Invalid font family name.");
  const auto entries = doc["files"].as<JsonArray>();
  if (entries.size() == 0 || entries.size() > MAX_FILES) return fail(400, "Too many or no font files selected.");
  uint64_t total = 0;
  for (JsonObject entry : entries) {
    if (!entry["name"].is<const char*>() || !entry["size"].is<uint32_t>())
      return fail(400, "Invalid font file metadata.");
    auto& file = files_[fileCount_];
    const auto name = entry["name"].as<JsonString>();
    if (!copyName({name.c_str(), name.size()}, file.requested, sizeof(file.requested)))
      return fail(400, "Invalid font filename.");
    char parsedFamily[MAX_FAMILY_LENGTH + 1]{};
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
    if (!FontInstaller::parseNativeFilename(file.requested, parsedFamily, sizeof(parsedFamily), file.style,
                                            file.canonical, sizeof(file.canonical)))
      return fail(400, "Use Family-Regular.ttf/.otf, with optional Bold, Italic and BoldItalic styles.");
#else
    if (!legacyFilename(file.requested, parsedFamily, sizeof(parsedFamily), file.canonical, file.style)) {
      return fail(400, "Use Family_12.cpfont filenames with a point-size suffix.");
    }
#endif
    if (std::strcmp(parsedFamily, family_) != 0) return fail(400, "Select files from one font family only.");
    for (size_t i = 0; i < fileCount_; ++i) {
      if (files_[i].style == file.style) return fail(400, "Select each font style or size only once.");
    }
    file.size = entry["size"].as<uint32_t>();
    if (file.size < sizeof(header_)) return fail(400, "The font file is truncated.");
    total += file.size;
    if (total > UINT32_MAX) return fail(400, "The font selection is too large.");
    ++fileCount_;
  }
  fonts_.refreshIfDirty();
  if (!installer_.isFamilyInstalled(family_) &&
      fonts_.registry().getFamilyCount() >= SdCardFontRegistry::MAX_SD_FAMILIES) {
    return fail(409, "The font family limit has been reached. Delete a family first.");
  }
  if (!installer_.ensureFamilyDir(family_)) return fail(500, "Unable to create the font folder on the SD card.");
  active_ = true;
  return true;
}

bool FontWebApi::stagingPath(const File& file, char* path, size_t capacity) const {
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  return FontInstaller::buildStagingFontPath(family_, file.canonical, path, capacity);
#else
  char finalPath[160];
  FontInstaller::buildFontPath(family_, file.canonical, finalPath, sizeof(finalPath));
  const int length = std::snprintf(path, capacity, "%s.part", finalPath);
  return length > 0 && static_cast<size_t>(length) < capacity;
#endif
}

bool FontWebApi::beginFile(std::string_view filename) {
  if (error_) return false;
  if (!active_ || current_ != MAX_FILES) return fail(400, "Unexpected font file.");
  size_t index = 0;
  for (; index < fileCount_; ++index)
    if (filename == files_[index].requested) break;
  if (index == fileCount_ || files_[index].staged)
    return fail(400, "The upload contains an undeclared or duplicate file.");
  char path[192];
  if (!stagingPath(files_[index], path, sizeof(path))) return fail(400, "Invalid font path.");
  // Do not overwrite another owner or an interrupted transaction's stage.
  if (Storage.exists(path)) return fail(409, "A staged file already exists. Remove that .part file before retrying.");
  files_[index].staged = true;
  if (!Storage.openFileForWrite("WEB", path, output_)) return fail(500, "Unable to write the font to the SD card.");
  current_ = index;
  buffered_ = headerBytes_ = received_ = written_ = 0;
  return true;
}

bool FontWebApi::flushBuffer() {
  if (!buffered_) return true;
  const size_t count = output_.write(buffer_, buffered_);
  written_ += static_cast<uint32_t>(count);
  if (count != buffered_) return fail(500, "The SD card could not write the complete font.");
  buffered_ = 0;
  return true;
}

bool FontWebApi::writeChunk(const uint8_t* bytes, size_t count) {
  if (error_) return false;
  if (!active_ || current_ == MAX_FILES || (!bytes && count)) return fail(400, "Unexpected font data.");
  if (count > files_[current_].size - received_) return fail(400, "The font is larger than its declared size.");
  const size_t headerCount = std::min(count, sizeof(header_) - headerBytes_);
  if (headerCount) std::memcpy(header_ + headerBytes_, bytes, headerCount);
  headerBytes_ += headerCount;
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  if (headerBytes_ >= 4 && std::memcmp(header_, "\0\1\0\0", 4) != 0 && std::memcmp(header_, "OTTO", 4) != 0) {
    return fail(400, "This file is not an SFNT TTF/OTF font.");
  }
#else
  if (headerBytes_ == sizeof(header_) && std::memcmp(header_, "CPFONT\0\0", 8) != 0)
    return fail(400, "This file is not a .cpfont font.");
#endif
  received_ += static_cast<uint32_t>(count);
  while (count) {
    const size_t chunk = std::min(count, sizeof(buffer_) - buffered_);
    std::memcpy(buffer_ + buffered_, bytes, chunk);
    buffered_ += chunk;
    bytes += chunk;
    count -= chunk;
    if (buffered_ == sizeof(buffer_) && !flushBuffer()) return false;
  }
  return true;
}

bool FontWebApi::endFile() {
  if (error_) return false;
  if (!active_ || current_ == MAX_FILES) return fail(400, "Unexpected end of font file.");
  if (received_ != files_[current_].size || headerBytes_ < sizeof(header_))
    return fail(400, "The font upload is incomplete.");
  if (!flushBuffer()) return false;
  const bool sized = written_ == files_[current_].size && output_.size() == files_[current_].size;
  const bool closed = output_.close();
  if (!sized || !closed) return fail(500, "The SD card could not save the complete font.");
  files_[current_].complete = true;
  current_ = MAX_FILES;
  return true;
}

bool FontWebApi::fail(int status, const char* message) {
  if (!error_) {
    errorStatus_ = status;
    error_ = message;
  }
  discardStaging();
  active_ = false;
  return false;
}

void FontWebApi::discardStaging() {
  if (output_.isOpen()) output_.close();
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  FontInstaller::NativeStyleFile staged[MAX_FILES];
  size_t count = 0;
  for (size_t i = 0; i < fileCount_; ++i) {
    if (!files_[i].staged) continue;
    staged[count++] = {files_[i].canonical, files_[i].style, {}, files_[i].size};
    files_[i].staged = false;
  }
  if (count) installer_.discardNativeStaging(family_, {staged, count});
#else
  for (size_t i = 0; i < fileCount_; ++i) {
    if (!files_[i].staged) continue;
    char path[192];
    if (stagingPath(files_[i], path, sizeof(path))) Storage.remove(path);
    files_[i].staged = false;
  }
#endif
  current_ = MAX_FILES;
  buffered_ = 0;
}

void FontWebApi::abortUpload() {
  if (!error_) {
    errorStatus_ = 400;
    error_ = "The font upload was cancelled.";
  }
  discardStaging();
  active_ = false;
}

#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
bool FontWebApi::commitLegacy() {
  for (size_t i = 0; i < fileCount_; ++i) {
    char path[192];
    if (!stagingPath(files_[i], path, sizeof(path)) || !installer_.validateFontFile(path)) {
      return fail(400, "This file is not a valid .cpfont font.");
    }
  }
  // Legacy packs also stage the whole selection, then keep backups until every
  // rename succeeds. No directory recursion and no unrelated file deletion.
  bool backedUp[MAX_FILES]{}, published[MAX_FILES]{};
  size_t index = 0;
  for (; index < fileCount_; ++index) {
    char finalPath[160], staged[192], backup[192];
    FontInstaller::buildFontPath(family_, files_[index].canonical, finalPath, sizeof(finalPath));
    stagingPath(files_[index], staged, sizeof(staged));
    std::snprintf(backup, sizeof(backup), "%s.web-backup", finalPath);
    if (Storage.exists(backup)) break;
    if (Storage.exists(finalPath)) {
      if (!Storage.rename(finalPath, backup)) break;
      backedUp[index] = true;
    }
    if (!Storage.rename(staged, finalPath)) break;
    published[index] = true;
  }
  if (index != fileCount_) {
    for (size_t i = fileCount_; i-- > 0;) {
      char finalPath[160], backup[192];
      FontInstaller::buildFontPath(family_, files_[i].canonical, finalPath, sizeof(finalPath));
      std::snprintf(backup, sizeof(backup), "%s.web-backup", finalPath);
      if (published[i]) Storage.remove(finalPath);
      if (backedUp[i]) Storage.rename(backup, finalPath);
    }
    return fail(500, "Unable to publish the font selection on the SD card.");
  }
  for (size_t i = 0; i < fileCount_; ++i) {
    if (!backedUp[i]) continue;
    char finalPath[160], backup[192];
    FontInstaller::buildFontPath(family_, files_[i].canonical, finalPath, sizeof(finalPath));
    std::snprintf(backup, sizeof(backup), "%s.web-backup", finalPath);
    Storage.remove(backup);
  }
  return true;
}
#endif

FontWebApi::Response FontWebApi::finishUpload() {
  if (!error_ && (!active_ || current_ != MAX_FILES)) fail(400, "The font upload is incomplete.");
  for (size_t i = 0; !error_ && i < fileCount_; ++i) {
    if (!files_[i].complete) fail(400, "The font selection is incomplete.");
  }
  if (error_) return failure(errorStatus_, error_);
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  FontInstaller::NativeStyleFile styles[MAX_FILES];
  for (size_t i = 0; i < fileCount_; ++i) styles[i] = {files_[i].canonical, files_[i].style, {}, files_[i].size};
  const auto result = installer_.commitNativeFamily(family_, {styles, fileCount_}, false);
  discardStaging();
  active_ = false;
  if (result != FontInstaller::Error::OK) return installerFailure(result);
#else
  if (!commitLegacy()) return failure(errorStatus_, error_);
  discardStaging();
  active_ = false;
#endif
  fonts_.markRegistryDirty();
  return {200, "{\"ok\":true}"};
}
