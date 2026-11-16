#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Browse Files sort order (CrossPointSettings::fileSortOrder). Folders stay
// above files and sort by name; files with equal keys sort by name.
namespace file_sort {
enum Order : uint8_t { NAME_ASC, NAME_DESC, NEWEST, OLDEST, LARGEST, SMALLEST, COUNT };

inline constexpr StrId LABELS[] = {StrId::STR_SORT_NAME_ASC, StrId::STR_SORT_NAME_DESC, StrId::STR_SORT_NEWEST,
                                   StrId::STR_SORT_OLDEST,   StrId::STR_SORT_LARGEST,   StrId::STR_SORT_SMALLEST};
static_assert(sizeof(LABELS) / sizeof(LABELS[0]) == COUNT);

inline Order fromSetting(const uint8_t value) { return value < COUNT ? static_cast<Order>(value) : NAME_ASC; }
// Date and size orders need a key per entry, gathered during the folder scan.
inline bool usesDate(const Order order) { return order == NEWEST || order == OLDEST; }
inline bool usesSize(const Order order) { return order == LARGEST || order == SMALLEST; }

// Sorts names ("dir/" for folders) in place. keys[i] belongs to names[i] and is
// read only for date and size orders; without keys those fall back to the name.
// False when the temporary index (4 bytes per entry) could not be allocated; the
// names are then in the default order.
bool sort(std::vector<std::string>& names, const uint32_t* keys, Order order);
}  // namespace file_sort
