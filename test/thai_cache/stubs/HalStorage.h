#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

// Real host files, not an in-memory cache imitation. Resource use is host-only.
class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool open(const char* path, const char* mode) { close(); file_ = std::fopen(path, mode); return file_ != nullptr; }
  int available() const { return file_ ? static_cast<int>(size() - position()) : 0; }
  size_t read(void* buffer, size_t count) { return file_ ? std::fread(buffer, 1, count, file_) : 0; }
  int read() { return file_ ? std::fgetc(file_) : -1; }
  size_t write(const void* buffer, size_t count) { return file_ ? std::fwrite(buffer, 1, count, file_) : 0; }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  bool flush() { return file_ && std::fflush(file_) == 0; }
  bool seek(size_t offset) { return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0; }
  bool seekCur(size_t offset) { return file_ && std::fseek(file_, static_cast<long>(offset), SEEK_CUR) == 0; }
  bool close() { if (!file_) return false; const bool ok = std::fclose(file_) == 0; file_ = nullptr; return ok; }
  explicit operator bool() const { return file_ != nullptr; }
  bool isOpen() const { return file_ != nullptr; }
  size_t position() const { return file_ ? static_cast<size_t>(std::ftell(file_)) : 0; }
  size_t size() const {
    if (!file_) return 0;
    const long offset = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, offset, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }
 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "rb"); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), "w+b"); }
  bool exists(const char* path) const { return std::filesystem::exists(path); }
  bool remove(const std::string& path) { return std::filesystem::remove(path); }
  bool rename(const char* from, const char* to) { return std::rename(from, to) == 0; }
  bool mkdir(const char* path) { return std::filesystem::create_directories(path) || exists(path); }
};
inline HalStorage Storage;
inline uint32_t micros() {
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline uint32_t millis() { return micros() / 1000; }
inline void delay(uint32_t) {}
