#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "FontInstaller.h"

class SdCardFontSystem;

// Font HTTP protocol shared by the firmware transport and the host HTTP adapter.
// A request declares every file before the first byte is written. File callbacks
// only stage data; finishUpload publishes the whole selection once.
class FontWebApi {
 public:
  struct Response {
    int status;
    std::string body;
  };
#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
  static constexpr size_t MAX_FILES = 4;
#else
  static constexpr size_t MAX_FILES = 32;
#endif
  static constexpr size_t MAX_FAMILY_LENGTH = 31;
  static constexpr size_t MAX_MANIFEST_BYTES = 8192;

  explicit FontWebApi(SdCardFontSystem& fonts);
  ~FontWebApi();
  FontWebApi(const FontWebApi&) = delete;
  FontWebApi& operator=(const FontWebApi&) = delete;

  Response list();
  Response remove(std::string_view body);
  // manifest: {"family":"Name","files":[{"name":"Name-Regular.ttf","size":123}]}
  bool beginUpload(std::string_view manifest);
  bool beginFile(std::string_view filename);
  bool writeChunk(const uint8_t* bytes, size_t count);
  bool endFile();
  Response finishUpload();
  void abortUpload();

 private:
  struct File {
    char requested[64]{};
    char canonical[64]{};
    uint32_t size = 0;
    uint8_t style = 0;
    bool staged = false;
    bool complete = false;
  };
  SdCardFontSystem& fonts_;
  FontInstaller installer_;
  File files_[MAX_FILES]{};
  char family_[MAX_FAMILY_LENGTH + 1]{};
  size_t fileCount_ = 0;
  size_t current_ = MAX_FILES;
  HalFile output_;
  uint8_t buffer_[4096]{};
  uint8_t header_[8]{};
  size_t buffered_ = 0;
  size_t headerBytes_ = 0;
  uint32_t received_ = 0;
  uint32_t written_ = 0;
  bool active_ = false;
  int errorStatus_ = 400;
  const char* error_ = "No font upload was received.";

  static Response failure(int status, const char* message);
  Response installerFailure(FontInstaller::Error error) const;
  bool fail(int status, const char* message);
  bool flushBuffer();
  bool stagingPath(const File& file, char* path, size_t capacity) const;
  void discardStaging();
#if !defined(CROSSPOINT_NATIVE_TEXT) || !CROSSPOINT_NATIVE_TEXT
  bool commitLegacy();
#endif
};
