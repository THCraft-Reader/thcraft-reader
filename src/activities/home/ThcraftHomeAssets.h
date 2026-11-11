#pragma once

#include <string>

#include "RecentBooksStore.h"

// SD-side helpers for the THCraft home: cover thumbs at the exact drawn size
// and widget images converted once into 1-bit BMPs. The caller owns the popup,
// SdCardFontSystem::CoverDecodeScope and a FrameBufferLoan per generation, the
// same batch protocol as HomeActivity::loadRecentCovers.
//
// resolveCoverPath and generateThumb mirror HomeActivity::resolveCoverPaths and
// HomeActivity::loadRecentCover; carry upstream fixes to those over on merges.
namespace thcraft_home {

// Fills book.coverBmpPath from the book's cache path; no parsing or decoding.
void resolveCoverPath(RecentBook& book);
// Thumb path for a height, or empty when the book has no cover cache.
std::string thumbPath(const RecentBook& book, int height);
bool needsThumb(const RecentBook& book, int height);
void generateThumb(RecentBook& book, int height);
// False when the book's saved position is gone (cache cleared or never
// written), so progress stored in recents no longer applies. True when the
// cache directory is unknown.
bool hasSavedPosition(const RecentBook& book);

// Cached 1-bit BMP for `fileName` (in widget_images::SOURCE_DIR) at w x h, or
// empty when the source is missing. A BMP source is drawn directly, so its own
// path is returned. The name encodes the source byte size and the slot size: a
// different-size file or a layout change regenerates it; a same-size
// replacement keeps the old conversion. A failed conversion is not retried
// until restart.
std::string widgetImagePath(const char* fileName, int width, int height);
bool needsWidgetImage(const std::string& cachePath);
bool generateWidgetImage(const char* fileName, const std::string& cachePath, int width, int height);
// Removes cached widget images other than the ones still in use.
void pruneWidgetImages(const std::string& keepA, const std::string& keepB);

}  // namespace thcraft_home
