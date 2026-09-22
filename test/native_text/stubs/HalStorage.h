#pragma once

#include <Arduino.h>
#include <Print.h>
#include <fcntl.h>

#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <vector>

using oflag_t = int;
#ifndef O_WRITE
#define O_WRITE O_WRONLY
#endif

enum class UsbDriveState : uint8_t { Unsupported, WaitingForHost, Connected, Ejected, Disconnected, IoError };
class HalStorage;
class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile();
  HalFile(HalFile&& other) noexcept;
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool open(const char* path, const char* mode);
  void flush();
  size_t getName(char* name, size_t length);
  size_t size() const;
  size_t fileSize() const { return size(); }
  uint64_t fileSize64() const;
  uint32_t modificationTime() const;
  bool seek(size_t position) { return seek64(position); }
  bool seek64(uint64_t position);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset) { return seek(offset); }
  int available() const;
  size_t position() const;
  int read(void* data, size_t count);
  int read();
  size_t write(const uint8_t* data, size_t count) override;
  size_t write(const void* data, size_t count) { return write(static_cast<const uint8_t*>(data), count); }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  bool rename(const char* path);
  bool isDirectory() const { return opened_ && directory_; }
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const { return opened_; }
  explicit operator bool() const { return isOpen(); }

 private:
  friend class HalStorage;
  bool openNative(const std::filesystem::path& path, const char* mode);
  void swap(HalFile& other) noexcept;
  FILE* file_ = nullptr;
  std::filesystem::path path_;
  std::filesystem::directory_iterator iterator_;
  bool directory_ = false, opened_ = false;
};

class HalStorage {
 public:
  struct Faults {
    std::string readPath, writePath;
    size_t readBytes = std::numeric_limits<size_t>::max();
    size_t writeBytes = std::numeric_limits<size_t>::max();
    size_t maximumRead = std::numeric_limits<size_t>::max();
    std::string renamePath;
    size_t renameFailures = 0;
  };
  static HalStorage& getInstance();
  bool begin();
  bool ready() const;
  void prepareForDeepSleep();
  bool beginUsbDrive();
  bool disconnectUsbDriveHost();
  void endUsbDrive();
  UsbDriveState usbDriveState() const;
  bool usbDriveHostSuspended() const { return false; }
  bool ensureDirectoryExists(const char* path);
  HalFile open(const char* path, oflag_t flags = O_RDONLY);
  bool mkdir(const char* path, bool parents = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool remove(const std::string& path) { return remove(path.c_str()); }
  bool rename(const char* from, const char* to);
  bool rmdir(const char* path);
  bool removeDir(const char* path);
  bool openFileForRead(const char*, const char* path, HalFile& file);
  bool openFileForRead(const char* module, const std::string& path, HalFile& file) {
    return openFileForRead(module, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file);
  bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  String readFile(const char* path);
  size_t readFileToBuffer(const char* path, char* buffer, size_t size, size_t maxBytes = 0);
  bool readFileToStream(const char* path, Print& output, size_t chunkSize = 256);
  bool writeFile(const char* path, const String& content);

  // Test-owned mount and I/O faults; all reads/writes remain real filesystem IO.
  void setRoot(const std::filesystem::path& path);
  const std::filesystem::path& root() const { return root_; }
  std::filesystem::path resolve(const char* path) const;
  Faults& faults() { return faults_; }
  void resetFaults() { faults_ = {}; }
  size_t openHandles() const { return openHandles_; }
  size_t forbiddenAccesses() const { return forbiddenAccesses_; }
  static std::recursive_mutex& mutex();

 private:
  friend class HalFile;
  HalStorage();
  bool accessAllowed();
  std::filesystem::path root_;
  Faults faults_;
  size_t openHandles_ = 0, forbiddenAccesses_ = 0;
  bool mounted_ = true, usbOwned_ = false;
};
#define Storage HalStorage::getInstance()
