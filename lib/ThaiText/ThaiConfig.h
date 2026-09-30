#pragma once

#include <cstddef>

#ifndef THAI_WORD_BREAKING
#define THAI_WORD_BREAKING 1
#endif
#ifndef THAI_DICTIONARY
#define THAI_DICTIONARY 1
#endif
#ifndef THAI_CLUSTER_DEBUG
#define THAI_CLUSTER_DEBUG 0
#endif
#ifndef THAI_ENGINE_STATS
#define THAI_ENGINE_STATS 0
#endif
#ifndef THAI_SHAPING
#define THAI_SHAPING 1
#endif

namespace thai {
inline constexpr size_t MAX_CLUSTER_CODEPOINTS = 32;
}  // namespace thai
