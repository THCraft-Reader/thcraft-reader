#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t byte) = 0;
  virtual size_t write(const uint8_t* bytes, size_t count) {
    size_t written = 0;
    while (written < count && write(bytes[written]) == 1) ++written;
    return written;
  }
  size_t print(const char* text) { return write(reinterpret_cast<const uint8_t*>(text), std::strlen(text)); }
  size_t println(const char* text) { return print(text) + print("\n"); }
};
