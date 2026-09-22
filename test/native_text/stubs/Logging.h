#pragma once
#include <cstdio>
#define LOG_ERR(tag, format, ...)                            \
  do {                                                       \
    std::fprintf(stderr, "%s: ", tag);                       \
    std::fprintf(stderr, format __VA_OPT__(, ) __VA_ARGS__); \
    std::fputc('\n', stderr);                                \
  } while (false)
#define LOG_INF(tag, format, ...)                            \
  do {                                                       \
    std::fprintf(stderr, "%s: ", tag);                       \
    std::fprintf(stderr, format __VA_OPT__(, ) __VA_ARGS__); \
    std::fputc('\n', stderr);                                \
  } while (false)
#define LOG_DBG(tag, format, ...)                              \
  do {                                                         \
    if (false) {                                               \
      std::fprintf(stderr, "%s: ", tag);                       \
      std::fprintf(stderr, format __VA_OPT__(, ) __VA_ARGS__); \
    }                                                          \
  } while (false)
