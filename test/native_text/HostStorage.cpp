#include <HalStorage.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <utility>

namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}
bool matches(const std::string& filter, const fs::path& path) {
  return filter.empty() || filter == utf8(path) || filter == utf8(path.filename()) ||
         Storage.resolve(filter.c_str()) == path;
}
int64_t tell(FILE* file) {
#ifdef _WIN32
  return _ftelli64(file);
#else
  return ftello(file);
#endif
}
bool seekFile(FILE* file, int64_t offset, int origin) {
#ifdef _WIN32
  return _fseeki64(file, offset, origin) == 0;
#else
  return fseeko(file, offset, origin) == 0;
#endif
}
}  // namespace

std::recursive_mutex& HalStorage::mutex() {
  static std::recursive_mutex value;
  return value;
}
HalStorage& HalStorage::getInstance() {
  static HalStorage value;
  return value;
}
HalStorage::HalStorage() {
  root_ = fs::temp_directory_path() /
          ("thcraft-native-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code error;
  fs::create_directories(root_, error);
  mounted_ = !error;
}
void HalStorage::setRoot(const fs::path& path) {
  std::lock_guard lock(mutex());
  if (openHandles_) throw std::logic_error("Cannot remount storage with open files");
  root_ = fs::absolute(path).lexically_normal();
  std::error_code error;
  fs::create_directories(root_, error);
  mounted_ = !error;
  usbOwned_ = false;
  faults_ = {};
  forbiddenAccesses_ = 0;
}
fs::path HalStorage::resolve(const char* input) const {
  fs::path path(reinterpret_cast<const char8_t*>(input));
  if (path.has_root_name()) return path;
  if (path.has_root_directory()) {
    const auto relative = path.lexically_relative(root_);
    if (!relative.empty() && *relative.begin() != "..") return path;
    return root_ / path.relative_path();
  }
  return root_ / path;
}
bool HalStorage::accessAllowed() {
  if (usbOwned_ || !mounted_) {
    ++forbiddenAccesses_;
    return false;
  }
  return true;
}
bool HalStorage::begin() {
  std::lock_guard lock(mutex());
  mounted_ = true;
  usbOwned_ = false;
  return true;
}
bool HalStorage::ready() const {
  std::lock_guard lock(mutex());
  return mounted_ && !usbOwned_;
}
void HalStorage::prepareForDeepSleep() {
  std::lock_guard lock(mutex());
  if (openHandles_) throw std::logic_error("Storage shutdown with open files");
  mounted_ = false;
}
bool HalStorage::beginUsbDrive() {
  std::lock_guard lock(mutex());
  if (openHandles_ || !mounted_) return false;
  usbOwned_ = true;
  return true;
}
bool HalStorage::disconnectUsbDriveHost() {
  std::lock_guard lock(mutex());
  return usbOwned_;
}
void HalStorage::endUsbDrive() {
  std::lock_guard lock(mutex());
  usbOwned_ = false;
}
UsbDriveState HalStorage::usbDriveState() const {
  return usbOwned_ ? UsbDriveState::Connected : UsbDriveState::Disconnected;
}

HalFile::~HalFile() { close(); }
HalFile::HalFile(HalFile&& other) noexcept { swap(other); }
HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    close();
    swap(other);
  }
  return *this;
}
void HalFile::swap(HalFile& other) noexcept {
  std::swap(file_, other.file_);
  std::swap(path_, other.path_);
  std::swap(iterator_, other.iterator_);
  std::swap(directory_, other.directory_);
  std::swap(opened_, other.opened_);
}
bool HalFile::open(const char* path, const char* mode) {
  return openNative(fs::path(reinterpret_cast<const char8_t*>(path)), mode);
}
bool HalFile::openNative(const fs::path& path, const char* mode) {
  std::lock_guard lock(HalStorage::mutex());
  close();
  if (!Storage.accessAllowed()) return false;
  std::error_code error;
  path_ = path;
  directory_ = fs::is_directory(path_, error);
  if (directory_) {
    iterator_ = fs::directory_iterator(path_, error);
    if (error) return false;
  } else {
#ifdef _WIN32
    wchar_t wideMode[8]{};
    if (std::strlen(mode) >= std::size(wideMode)) return false;
    for (size_t i = 0; mode[i]; ++i) wideMode[i] = mode[i];
    file_ = _wfopen(path_.c_str(), wideMode);
#else
    file_ = std::fopen(path_.c_str(), mode);
#endif
    if (!file_) return false;
    std::setvbuf(file_, nullptr, _IONBF, 0);
  }
  opened_ = true;
  ++Storage.openHandles_;
  return true;
}
bool HalFile::close() {
  std::lock_guard lock(HalStorage::mutex());
  if (!opened_) return false;
  const bool success = !file_ || std::fclose(file_) == 0;
  file_ = nullptr;
  iterator_ = {};
  opened_ = directory_ = false;
  --Storage.openHandles_;
  return success;
}
void HalFile::flush() {
  std::lock_guard lock(HalStorage::mutex());
  if (file_ && Storage.accessAllowed()) std::fflush(file_);
}
size_t HalFile::getName(char* output, size_t length) {
  if (!length) return 0;
  const auto name = utf8(path_.filename());
  const size_t count = std::min(length - 1, name.size());
  std::memcpy(output, name.data(), count);
  output[count] = 0;
  return count;
}
uint64_t HalFile::fileSize64() const {
  std::lock_guard lock(HalStorage::mutex());
  if (!file_ || !Storage.accessAllowed()) return 0;
  const auto current = tell(file_);
  if (current < 0 || !seekFile(file_, 0, SEEK_END)) return 0;
  const auto length = tell(file_);
  seekFile(file_, current, SEEK_SET);
  return length < 0 ? 0 : static_cast<uint64_t>(length);
}
size_t HalFile::size() const { return static_cast<size_t>(fileSize64()); }
uint32_t HalFile::modificationTime() const {
  std::error_code error;
  const auto time = fs::last_write_time(path_, error);
  return error ? 0 : static_cast<uint32_t>(time.time_since_epoch().count());
}
bool HalFile::seek64(uint64_t position) {
  std::lock_guard lock(HalStorage::mutex());
  return file_ && Storage.accessAllowed() && position <= INT64_MAX &&
         seekFile(file_, static_cast<int64_t>(position), SEEK_SET);
}
bool HalFile::seekCur(int64_t offset) {
  std::lock_guard lock(HalStorage::mutex());
  return file_ && Storage.accessAllowed() && seekFile(file_, offset, SEEK_CUR);
}
size_t HalFile::position() const {
  std::lock_guard lock(HalStorage::mutex());
  const auto value = file_ ? tell(file_) : -1;
  return value < 0 ? 0 : static_cast<size_t>(value);
}
int HalFile::available() const {
  const auto length = fileSize64(), offset = position();
  return static_cast<int>(std::min<uint64_t>(length > offset ? length - offset : 0, INT_MAX));
}
int HalFile::read(void* output, size_t count) {
  std::lock_guard lock(HalStorage::mutex());
  if (!file_ || !Storage.accessAllowed() || count > INT_MAX) return -1;
  auto& faults = Storage.faults_;
  const bool injected = matches(faults.readPath, path_);
  size_t wanted = injected ? std::min(count, faults.maximumRead) : count;
  if (injected) wanted = std::min(wanted, faults.readBytes);
  if (count && !wanted) return -1;
  const size_t got = std::fread(output, 1, wanted, file_);
  if (injected && faults.readBytes != std::numeric_limits<size_t>::max()) faults.readBytes -= got;
  return !got && std::ferror(file_) ? -1 : static_cast<int>(got);
}
int HalFile::read() {
  uint8_t byte;
  return read(&byte, 1) == 1 ? byte : -1;
}
size_t HalFile::write(const uint8_t* input, size_t count) {
  std::lock_guard lock(HalStorage::mutex());
  if (!file_ || !Storage.accessAllowed()) return 0;
  auto& faults = Storage.faults_;
  const bool injected = matches(faults.writePath, path_);
  const size_t wanted = injected ? std::min(count, faults.writeBytes) : count;
  const size_t written = std::fwrite(input, 1, wanted, file_);
  if (injected && faults.writeBytes != std::numeric_limits<size_t>::max()) faults.writeBytes -= written;
  return written;
}
bool HalFile::rename(const char* target) {
  std::lock_guard lock(HalStorage::mutex());
  if (!Storage.accessAllowed()) return false;
  std::error_code error;
  const auto path = Storage.resolve(target);
  fs::rename(path_, path, error);
  if (!error) path_ = path;
  return !error;
}
void HalFile::rewindDirectory() {
  std::lock_guard lock(HalStorage::mutex());
  if (directory_ && Storage.accessAllowed()) {
    std::error_code error;
    iterator_ = fs::directory_iterator(path_, error);
  }
}
HalFile HalFile::openNextFile() {
  std::lock_guard lock(HalStorage::mutex());
  HalFile next;
  if (!directory_ || !Storage.accessAllowed() || iterator_ == fs::directory_iterator{}) return next;
  const auto path = iterator_->path();
  std::error_code error;
  iterator_.increment(error);
  if (error) iterator_ = {};
  next.openNative(path, "rb");
  return next;
}

bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  return file.openNative(resolve(path), "rb");
}
bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  return file.openNative(resolve(path), "w+b");
}
HalFile HalStorage::open(const char* path, oflag_t flags) {
  HalFile file;
  const auto resolved = resolve(path);
  const bool writing = (flags & O_WRONLY) || (flags & O_RDWR);
  const char* mode = "rb";
  if (writing) {
    std::error_code error;
    if (flags & O_APPEND)
      mode = (flags & O_RDWR) ? "a+b" : "ab";
    else if ((flags & O_TRUNC) || ((flags & O_CREAT) && !fs::exists(resolved, error)))
      mode = (flags & O_RDWR) ? "w+b" : "wb";
    else
      mode = "r+b";
  }
  file.openNative(resolved, mode);
  return file;
}
bool HalStorage::mkdir(const char* path, bool parents) {
  std::lock_guard lock(mutex());
  if (!accessAllowed()) return false;
  std::error_code error;
  const auto resolved = resolve(path);
  if (fs::is_directory(resolved, error)) return true;
  if (parents)
    fs::create_directories(resolved, error);
  else
    fs::create_directory(resolved, error);
  return !error;
}
bool HalStorage::ensureDirectoryExists(const char* path) { return mkdir(path, true); }
bool HalStorage::exists(const char* path) {
  std::lock_guard lock(mutex());
  std::error_code error;
  return accessAllowed() && fs::exists(resolve(path), error);
}
bool HalStorage::remove(const char* path) {
  std::lock_guard lock(mutex());
  if (!accessAllowed()) return false;
  std::error_code error;
  return fs::remove(resolve(path), error) && !error;
}
bool HalStorage::rename(const char* from, const char* to) {
  std::lock_guard lock(mutex());
  if (!accessAllowed()) return false;
  const auto source = resolve(from);
  if (faults_.renameFailures && matches(faults_.renamePath, source)) {
    --faults_.renameFailures;
    return false;
  }
  std::error_code error;
  fs::rename(source, resolve(to), error);
  return !error;
}
bool HalStorage::rmdir(const char* path) { return remove(path); }
bool HalStorage::removeDir(const char* path) {
  std::lock_guard lock(mutex());
  if (!accessAllowed()) return false;
  std::error_code error;
  return fs::remove_all(resolve(path), error) != 0 && !error;
}
std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  std::vector<String> files;
  if (maxFiles <= 0) return files;
  files.reserve(static_cast<size_t>(maxFiles));
  auto directory = open(path);
  while (static_cast<int>(files.size()) < maxFiles) {
    auto next = directory.openNextFile();
    if (!next) break;
    files.push_back(utf8(next.path_.filename()));
  }
  return files;
}
size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t size, size_t maxBytes) {
  if (!size) return 0;
  buffer[0] = 0;
  auto file = open(path);
  if (!file) return 0;
  const size_t limit = maxBytes ? std::min(size - 1, maxBytes) : size - 1;
  const int count = file.read(buffer, limit);
  if (count < 0) return 0;
  buffer[count] = 0;
  return static_cast<size_t>(count);
}
String HalStorage::readFile(const char* path) {
  auto file = open(path);
  if (!file) return {};
  String result(file.size(), '\0');
  if (file.read(result.data(), result.size()) != static_cast<int>(result.size())) return {};
  return result;
}
bool HalStorage::readFileToStream(const char* path, Print& output, size_t chunkSize) {
  auto file = open(path);
  if (!file || !chunkSize) return false;
  std::vector<uint8_t> buffer(std::min<size_t>(chunkSize, 4096));
  while (file.available()) {
    const int count = file.read(buffer.data(), buffer.size());
    if (count <= 0 || output.write(buffer.data(), count) != static_cast<size_t>(count)) return false;
  }
  return true;
}
bool HalStorage::writeFile(const char* path, const String& content) {
  HalFile file;
  return openFileForWrite("HOST", path, file) && file.write(content.data(), content.size()) == content.size();
}
