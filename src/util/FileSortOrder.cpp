#include "FileSortOrder.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <utility>

namespace file_sort {
bool sort(std::vector<std::string>& names, const uint32_t* keys, const Order order) {
  const bool keyed = keys && (usesDate(order) || usesSize(order));
  if (order == NAME_ASC || (!keyed && order != NAME_DESC)) {
    FsHelpers::sortFileList(names);
    return true;
  }

  const size_t count = names.size();
  auto index = makeUniqueNoThrow<uint32_t[]>(count);
  if (!index) {
    LOG_ERR("FileSort", "OOM: sort index (%u entries)", static_cast<unsigned>(count));
    FsHelpers::sortFileList(names);
    return false;
  }
  for (size_t i = 0; i < count; ++i) index[i] = static_cast<uint32_t>(i);

  const bool descending = order == NAME_DESC || order == NEWEST || order == LARGEST;
  std::sort(index.get(), index.get() + count, [&](const uint32_t a, const uint32_t b) {
    const std::string& left = names[a];
    const std::string& right = names[b];
    const bool leftDir = left.back() == '/';
    const bool rightDir = right.back() == '/';
    if (leftDir != rightDir) return leftDir;
    if (keyed && !leftDir && keys[a] != keys[b]) return descending ? keys[a] > keys[b] : keys[a] < keys[b];
    // Name order; Z-A reverses it, date and size ties read A-Z.
    return order == NAME_DESC ? FsHelpers::naturalLess(right, left) : FsHelpers::naturalLess(left, right);
  });

  // Apply the permutation in place by following its cycles: swaps move string
  // handles only, so no second list of names is allocated.
  for (size_t start = 0; start < count; ++start) {
    if (index[start] == start) continue;
    size_t current = start;
    while (index[current] != start) {
      const size_t next = index[current];
      std::swap(names[current], names[next]);
      index[current] = static_cast<uint32_t>(current);
      current = next;
    }
    index[current] = static_cast<uint32_t>(current);
  }
  return true;
}
}  // namespace file_sort
