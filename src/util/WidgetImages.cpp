#include "WidgetImages.h"

#include <FsHelpers.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace widget_images {

std::vector<std::string> list() {
  constexpr size_t MAX_IMAGES = 32;
  std::vector<std::string> files;
  auto dir = Storage.open(SOURCE_DIR);
  if (!dir || !dir.isDirectory()) return files;
  files.reserve(8);
  char name[48];
  for (auto entry = dir.openNextFile(); entry && files.size() < MAX_IMAGES; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    // A truncated name could not be opened again; skip it like macOS metadata.
    if (entry.getName(name, sizeof(name)) >= sizeof(name) - 1 || strncmp(name, "._", 2) == 0) continue;
    const std::string_view view(name);
    if (!FsHelpers::hasJpgExtension(view) && !FsHelpers::hasPngExtension(view) && !FsHelpers::hasGifExtension(view) &&
        !FsHelpers::hasBmpExtension(view)) {
      continue;
    }
    files.emplace_back(name);
  }
  std::sort(files.begin(), files.end());
  return files;
}

bool isCachePath(const std::string& path) { return path.rfind(CACHE_DIR, 0) == 0; }

}  // namespace widget_images
