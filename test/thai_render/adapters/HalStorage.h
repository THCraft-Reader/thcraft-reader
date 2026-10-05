#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include "Arduino.h"

namespace probe {
inline uint64_t readCalls = 0, readBytes = 0;
// Companion-only HAL requests, including failed attempts; not physical SD transactions.
inline uint64_t shapeReadCalls = 0, shapeReadBytes = 0, shapeSeekCalls = 0;
inline uint64_t shapeReadFailures = 0, shapeSeekFailures = 0;
// Zero disables an ordinal failure; ordinals are absolute companion request counts.
inline uint64_t shapeFailSeekAt = 0, shapeFailReadAt = 0, shapeZeroReadAt = 0, shapeShortReadAt = 0;
inline size_t shapeTruncateAt = std::numeric_limits<size_t>::max();
inline void resetShapeFaults() {
  shapeFailSeekAt = shapeFailReadAt = shapeZeroReadAt = shapeShortReadAt = 0;
  shapeTruncateAt = std::numeric_limits<size_t>::max();
}
// Empty for the ordinary render probe; UI discovery mounts a host SD root.
inline std::filesystem::path sdRoot;
inline std::filesystem::path hostPath(const char* path) {
  const std::string value(path);
  if (!sdRoot.empty() &&
      (value == "/fonts" || value == "/.fonts" || value.rfind("/fonts/", 0) == 0 || value.rfind("/.fonts/", 0) == 0)) {
    return sdRoot / value.substr(1);
  }
  return std::filesystem::path(path);
}
}  // namespace probe
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept
      : file_(std::exchange(other.file_, nullptr)),
        shape_(std::exchange(other.shape_, false)),
        path_(std::move(other.path_)),
        directory_(std::exchange(other.directory_, false)),
        next_(std::move(other.next_)) {}
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) {
      close();
      file_ = std::exchange(other.file_, nullptr);
      shape_ = std::exchange(other.shape_, false);
      path_ = std::move(other.path_);
      directory_ = std::exchange(other.directory_, false);
      next_ = std::move(other.next_);
    }
    return *this;
  }
  bool open(const char* path, const char* mode) {
    close();
    path_ = probe::hostPath(path);
    shape_ = path_.extension() == ".cpshape";
    std::error_code error;
    if (std::filesystem::is_directory(path_, error)) {
      next_ = std::filesystem::directory_iterator(path_, error);
      directory_ = !error;
      return directory_;
    }
    file_ = std::fopen(path_.string().c_str(), mode);
    return file_ != nullptr;
  }
  bool isDirectory() const { return directory_; }
  HalFile openNextFile() {
    HalFile result;
    if (!directory_ || next_ == std::filesystem::directory_iterator{}) return result;
    const auto path = next_->path().string();
    ++next_;
    result.open(path.c_str(), "rb");
    return result;
  }
  void getName(char* buffer, size_t capacity) const {
    if (capacity) std::snprintf(buffer, capacity, "%s", path_.filename().string().c_str());
  }
  int available() const { return file_ ? static_cast<int>(size() - position()) : 0; }
  int read(void* buffer, size_t count) {
    ++probe::readCalls;
    const bool shape = isShape();
    if (shape) {
      ++probe::shapeReadCalls;
      if (probe::shapeReadCalls == probe::shapeFailReadAt) {
        ++probe::shapeReadFailures;
        return -1;
      }
    }
    size_t request = count;
    if (shape) {
      const size_t pos = position();
      request = pos >= probe::shapeTruncateAt ? 0 : std::min(request, probe::shapeTruncateAt - pos);
      if (probe::shapeReadCalls == probe::shapeShortReadAt && request) --request;
      if (probe::shapeReadCalls == probe::shapeZeroReadAt) request = 0;
    }
    const size_t got = file_ ? std::fread(buffer, 1, request, file_) : 0;
    probe::readBytes += got;
    if (shape) {
      probe::shapeReadBytes += got;
      if (got != count) ++probe::shapeReadFailures;
    }
    return static_cast<int>(got);
  }
  int read() {
    uint8_t byte;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  size_t write(const void* buffer, size_t count) { return file_ ? std::fwrite(buffer, 1, count, file_) : 0; }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  bool flush() { return file_ && std::fflush(file_) == 0; }
  bool seekSet(size_t offset) {
    if (isShape()) {
      ++probe::shapeSeekCalls;
      if (probe::shapeSeekCalls == probe::shapeFailSeekAt) {
        ++probe::shapeSeekFailures;
        return false;
      }
    }
    return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0;
  }
  bool seek(size_t offset) { return seekSet(offset); }
  bool seekCur(size_t offset) { return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_CUR) == 0; }
  bool close() {
    const bool wasOpen = isOpen();
    directory_ = false;
    shape_ = false;
    next_ = {};
    const bool ok = !file_ || std::fclose(file_) == 0;
    file_ = nullptr;
    return wasOpen && ok;
  }
  bool isOpen() const { return file_ != nullptr || directory_; }
  explicit operator bool() const { return isOpen(); }
  size_t position() const { return file_ ? static_cast<size_t>(std::ftell(file_)) : 0; }
  size_t size() const {
    if (!file_) return 0;
    const long pos = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, pos, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }

 private:
  bool isShape() const { return shape_; }
  std::FILE* file_ = nullptr;
  bool shape_ = false;
  std::filesystem::path path_;
  bool directory_ = false;
  std::filesystem::directory_iterator next_;
};
class HalStorage {
 public:
  HalFile open(const char* path) {
    HalFile result;
    result.open(path, "rb");
    return result;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "wb"); }
  bool exists(const char* path) const { return std::filesystem::exists(probe::hostPath(path)); }
  bool remove(const std::string& path) { return std::filesystem::remove(probe::hostPath(path.c_str())); }
  bool rename(const char* from, const char* to) {
    std::error_code error;
    std::filesystem::rename(probe::hostPath(from), probe::hostPath(to), error);
    return !error;
  }
};
inline HalStorage Storage;
