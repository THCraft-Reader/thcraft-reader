#pragma once

#include <string>
#include <vector>

// SD locations of THCraft home widget images, shared by settings and home.
namespace widget_images {

// User images offered for a widget slot.
constexpr const char* SOURCE_DIR = "/widgets";
// 1-bit BMPs converted from them at a slot's size.
constexpr const char* CACHE_DIR = "/.crosspoint/widgets";

// Image files in SOURCE_DIR, sorted by name; at most 32, names under 47 bytes.
std::vector<std::string> list();
// True when `path` names a converted image in CACHE_DIR.
bool isCachePath(const std::string& path);

}  // namespace widget_images
