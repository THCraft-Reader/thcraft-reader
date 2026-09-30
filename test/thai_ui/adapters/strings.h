#pragma once
// MSVC spells the POSIX case-insensitive comparisons differently. The root
// host CMake project provides strcasecmp/strncasecmp compatibility definitions.
#include <string.h>
