#pragma once

#include <ctime>

#ifdef _MSC_VER
inline tm* localtime_r(const time_t* value, tm* out) { return localtime_s(out, value) == 0 ? out : nullptr; }
#endif
