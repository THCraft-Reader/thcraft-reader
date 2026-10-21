#include "ThcraftHomeAssets.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GifToBmpConverter.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "components/UITheme.h"
#include "util/WidgetImages.h"

namespace thcraft_home {
namespace {

constexpr uint32_t WIDGET_CACHE_VERSION = 1;

uint32_t fnv1a(const void* data, const size_t length, uint32_t hash = 2166136261u) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

// Conversions that failed this boot, one per widget slot.
std::array<std::string, 2> failedConversions;
size_t failedNext = 0;

void sourcePath(const char* fileName, char* out, const size_t size) {
  snprintf(out, size, "%s/%s", widget_images::SOURCE_DIR, fileName);
}

}  // namespace

void resolveCoverPath(RecentBook& book) {
  if (!book.coverBmpPath.empty()) return;
  // Constructors only derive cache paths; the objects stay off the task stack.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("THOME", "OOM: EPUB thumbnail path");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("THOME", "OOM: XTC thumbnail path");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
  }
}

std::string thumbPath(const RecentBook& book, const int height) {
  if (book.coverBmpPath.empty() || height <= 0) return {};
  return UITheme::getCoverThumbPath(book.coverBmpPath, height);
}

bool needsThumb(const RecentBook& book, const int height) {
  if (!FsHelpers::hasReflowableBookExtension(book.path) && !FsHelpers::hasXtcExtension(book.path)) return false;
  const std::string path = thumbPath(book, height);
  return path.empty() || !Storage.exists(path.c_str());
}

void generateThumb(RecentBook& book, const int height) {
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("THOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (!epub->generateThumbBmpFromSource(height)) {
      LOG_ERR("THOME", "Failed to generate EPUB thumbnail: %s", book.path.c_str());
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("THOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (!xtc->load() || !xtc->generateThumbBmp(height)) {
      LOG_ERR("THOME", "Failed to generate XTC thumbnail: %s", book.path.c_str());
    }
  }
}

bool hasSavedPosition(const RecentBook& book) {
  // Thumbs and progress.bin share the book's cache directory.
  // Without a cover path the cache directory is unknown; keep the stored progress.
  const size_t slash = book.coverBmpPath.rfind('/');
  if (slash == std::string::npos) return true;
  return Storage.exists((book.coverBmpPath.substr(0, slash) + "/progress.bin").c_str());
}

std::string widgetImagePath(const char* fileName, const int width, const int height) {
  if (!fileName || fileName[0] == '\0' || width <= 0 || height <= 0) return {};
  char source[96];
  sourcePath(fileName, source, sizeof(source));
  if (FsHelpers::hasBmpExtension(fileName)) return Storage.exists(source) ? std::string(source) : std::string();

  HalFile file;
  if (!Storage.openFileForRead("THOME", source, file)) return {};
  const uint32_t size = static_cast<uint32_t>(file.fileSize());
  file.close();
  const uint32_t key[] = {size, static_cast<uint32_t>(width), static_cast<uint32_t>(height), WIDGET_CACHE_VERSION};
  const uint32_t hash = fnv1a(key, sizeof(key), fnv1a(fileName, strlen(fileName)));
  char path[64];
  snprintf(path, sizeof(path), "%s/%08lx.bmp", widget_images::CACHE_DIR, static_cast<unsigned long>(hash));
  return path;
}

bool needsWidgetImage(const std::string& cachePath) {
  if (!widget_images::isCachePath(cachePath) || Storage.exists(cachePath.c_str())) return false;
  return std::find(failedConversions.begin(), failedConversions.end(), cachePath) == failedConversions.end();
}

bool generateWidgetImage(const char* fileName, const std::string& cachePath, const int width, const int height) {
  char source[96];
  sourcePath(fileName, source, sizeof(source));
  HalFile in;
  if (!Storage.openFileForRead("THOME", source, in)) return false;
  Storage.mkdir(widget_images::CACHE_DIR);
  HalFile out;
  if (!Storage.openFileForWrite("THOME", cachePath, out)) {
    in.close();
    return false;
  }
  // The converters scale to cover the slot; the draw crops the overflow.
  bool ok = false;
  const std::string_view name(fileName);
  if (FsHelpers::hasJpgExtension(name)) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(in, out, width, height);
  } else if (FsHelpers::hasPngExtension(name)) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(in, out, width, height);
  } else if (FsHelpers::hasGifExtension(name)) {
    ok = GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(in, out, width, height);
  }
  in.close();
  out.close();
  if (!ok) {
    LOG_ERR("THOME", "Failed to convert widget image %s", source);
    Storage.remove(cachePath.c_str());
    // Not retried until restart: decoding may also fail from a passing heap shortage.
    failedConversions[failedNext] = cachePath;
    failedNext = (failedNext + 1) % failedConversions.size();
  }
  return ok;
}

void pruneWidgetImages(const std::string& keepA, const std::string& keepB) {
  // Collect first: removing entries while the directory is open breaks iteration.
  constexpr int MAX_STALE = 8;
  char stale[MAX_STALE][64];
  int staleCount = 0;
  {
    auto dir = Storage.open(widget_images::CACHE_DIR);
    if (!dir || !dir.isDirectory()) return;
    char name[40];
    for (auto entry = dir.openNextFile(); entry && staleCount < MAX_STALE; entry = dir.openNextFile()) {
      if (entry.isDirectory()) continue;
      entry.getName(name, sizeof(name));
      snprintf(stale[staleCount], sizeof(stale[staleCount]), "%s/%s", widget_images::CACHE_DIR, name);
      if (keepA != stale[staleCount] && keepB != stale[staleCount]) staleCount++;
    }
  }
  for (int i = 0; i < staleCount; ++i) Storage.remove(stale[i]);
}

}  // namespace thcraft_home
