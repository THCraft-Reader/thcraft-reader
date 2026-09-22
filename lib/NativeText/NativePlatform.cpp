#include "NativePlatform.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "NativeUtf8.h"

#if defined(ARDUINO) || defined(NATIVE_TEXT_HOST_STORAGE)
#include <HalStorage.h>
#endif
#ifdef ARDUINO
#include <Logging.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#else
#include <cstdio>
#include <mutex>
#endif

namespace {
#ifdef ARDUINO
StaticSemaphore_t nativeMutexStorage;
SemaphoreHandle_t nativeMutex = nullptr;
portMUX_TYPE nativeMutexCreation = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t mutexHandle() {
  portENTER_CRITICAL(&nativeMutexCreation);
  if (!nativeMutex) nativeMutex = xSemaphoreCreateRecursiveMutexStatic(&nativeMutexStorage);
  portEXIT_CRITICAL(&nativeMutexCreation);
  return nativeMutex;
}
#else
std::recursive_mutex nativeMutex;
#endif
}  // namespace

void nativeTextLock() {
#ifdef ARDUINO
  xSemaphoreTakeRecursive(mutexHandle(), portMAX_DELAY);
#else
  nativeMutex.lock();
#endif
}

void nativeTextUnlock() {
#ifdef ARDUINO
  xSemaphoreGiveRecursive(mutexHandle());
#else
  nativeMutex.unlock();
#endif
}

void nativeTextLogError(const char* message) {
#ifdef ARDUINO
  LOG_ERR("TEXT", "%s", message);
#else
  std::fprintf(stderr, "TEXT: %s\n", message);
#endif
}

void nativeTextYield() {
#ifdef ARDUINO
  // Called under the recursive engine mutex; yielding does not release it or
  // expose in-flight face/buffer state to a second user.
  static int64_t lastYield = esp_timer_get_time();
  const int64_t now = esp_timer_get_time();
  if (now - lastYield >= 16000) {
    vTaskDelay(1);
    lastYield = esp_timer_get_time();
  }
#endif
}

namespace native_text::detail {
struct FontStream {
#if defined(ARDUINO) || defined(NATIVE_TEXT_HOST_STORAGE)
  HalFile file;
#else
  FILE* file = nullptr;
#endif
  uint8_t* cache = nullptr;
  uint32_t size = 0;
  uint32_t cacheOffset = 0;
  size_t cacheBytes = 0;
  bool failed = false;
};

void closeFontStream(FontStream* stream) {
  if (!stream) return;
#if defined(ARDUINO) || defined(NATIVE_TEXT_HOST_STORAGE)
  stream->file.close();
#else
  if (stream->file) std::fclose(stream->file);
#endif
  native_text_free(stream->cache);
  stream->~FontStream();
  native_text_free(stream);
}

TextStatus openFontStream(const char* path, FontStream*& stream) {
  stream = nullptr;
  void* memory = native_text_malloc(sizeof(FontStream));
  if (!memory) return TextStatus::OutOfMemory;
  auto* opened = new (memory) FontStream;
  opened->cache = static_cast<uint8_t*>(native_text_malloc(4096));
  if (!opened->cache) {
    closeFontStream(opened);
    return TextStatus::OutOfMemory;
  }
#if defined(ARDUINO) || defined(NATIVE_TEXT_HOST_STORAGE)
  if (!Storage.openFileForRead("TEXT", path, opened->file)) {
    closeFontStream(opened);
    return TextStatus::StorageError;
  }
  const uint64_t length = opened->file.fileSize64();
#else
#ifdef _WIN32
  const std::string_view utf8Path(path);
  NativeBuffer<wchar_t> widePath;
  if (!widePath.resize(utf8Path.size() + 1)) {
    closeFontStream(opened);
    return TextStatus::OutOfMemory;
  }
  size_t offset = 0, units = 0;
  uint32_t codepoint = 0;
  while (offset < utf8Path.size()) {
    if (!native_text::nextUtf8(utf8Path, offset, codepoint)) {
      closeFontStream(opened);
      return TextStatus::InvalidText;
    }
    if (codepoint <= 0xffff) {
      widePath[units++] = static_cast<wchar_t>(codepoint);
    } else {
      codepoint -= 0x10000;
      widePath[units++] = static_cast<wchar_t>(0xd800 + (codepoint >> 10));
      widePath[units++] = static_cast<wchar_t>(0xdc00 + (codepoint & 0x3ff));
    }
  }
  widePath[units] = L'\0';
  opened->file = _wfopen(widePath.data(), L"rb");
#else
  opened->file = std::fopen(path, "rb");
#endif
  if (!opened->file) {
    closeFontStream(opened);
    return TextStatus::StorageError;
  }
  std::setvbuf(opened->file, nullptr, _IONBF, 0);
#ifdef _WIN32
  const bool seekOk = _fseeki64(opened->file, 0, SEEK_END) == 0;
  const auto length = seekOk ? _ftelli64(opened->file) : -1;
#else
  const bool seekOk = fseeko(opened->file, 0, SEEK_END) == 0;
  const auto length = seekOk ? ftello(opened->file) : -1;
#endif
  if (length < 0) {
    closeFontStream(opened);
    return TextStatus::StorageError;
  }
#endif
  if (static_cast<uint64_t>(length) > std::numeric_limits<uint32_t>::max()) {
    closeFontStream(opened);
    return TextStatus::CapacityExceeded;
  }
  opened->size = static_cast<uint32_t>(length);
  stream = opened;
  return TextStatus::Ok;
}

uint32_t fontStreamSize(const FontStream* stream) { return stream->size; }
bool fontStreamFailed(const FontStream* stream) { return stream && stream->failed; }

size_t readFontStream(FontStream* stream, uint32_t offset, void* data, size_t bytes) {
  if (offset > stream->size || bytes > stream->size - offset || (bytes && !data)) return 0;
  size_t copied = 0;
  while (copied < bytes) {
    const uint32_t position = offset + static_cast<uint32_t>(copied);
    if (!stream->cacheBytes || position < stream->cacheOffset || position - stream->cacheOffset >= stream->cacheBytes) {
      stream->cacheOffset = position & ~uint32_t{4095};
      const size_t request = std::min<size_t>(4096, stream->size - stream->cacheOffset);
#if defined(ARDUINO) || defined(NATIVE_TEXT_HOST_STORAGE)
      const bool sought = stream->file.seekSet(stream->cacheOffset);
      const int read = sought ? stream->file.read(stream->cache, request) : -1;
      stream->cacheBytes = read > 0 ? static_cast<size_t>(read) : 0;
#else
#ifdef _WIN32
      const bool sought = _fseeki64(stream->file, stream->cacheOffset, SEEK_SET) == 0;
#else
      const bool sought = fseeko(stream->file, stream->cacheOffset, SEEK_SET) == 0;
#endif
      stream->cacheBytes = sought ? std::fread(stream->cache, 1, request, stream->file) : 0;
#endif
      if (stream->cacheBytes != request) {
        stream->cacheBytes = 0;
        stream->failed = true;
        return copied;
      }
    }
    const size_t within = position - stream->cacheOffset;
    const size_t chunk = std::min(bytes - copied, stream->cacheBytes - within);
    if (static_cast<uint8_t*>(data) + copied != stream->cache + within) {
      std::memcpy(static_cast<uint8_t*>(data) + copied, stream->cache + within, chunk);
    }
    copied += chunk;
  }
  return copied;
}

TextStatus fontStreamFingerprint(FontStream* stream, uint64_t& fingerprint) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (uint32_t offset = 0; offset < stream->size;) {
    const size_t count = std::min<size_t>(4096, stream->size - offset);
    if (readFontStream(stream, offset, stream->cache, count) != count) return TextStatus::StorageError;
    for (size_t i = 0; i < count; ++i) hash = (hash ^ stream->cache[i]) * UINT64_C(1099511628211);
    offset += static_cast<uint32_t>(count);
    nativeTextYield();
  }
  fingerprint = hash;
  return TextStatus::Ok;
}
}  // namespace native_text::detail
