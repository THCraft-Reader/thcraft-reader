#pragma once
#include "Arduino.h"
#include <cstdio>
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(tag, ...) do { std::fprintf(stderr, "[%s] ", tag); std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (false)
#define LOG_WARN(tag, ...) LOG_ERR(tag, __VA_ARGS__)
