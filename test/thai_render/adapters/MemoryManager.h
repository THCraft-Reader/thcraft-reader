#pragma once
#include <cstddef>
#include <functional>
namespace freeink {
enum class MemPool : unsigned char { Internal, Psram, Default };
class MemoryManager {
 public:
  struct Sink { const char* name; int priority; std::function<size_t(size_t)> release; };
  static MemoryManager& instance() { static MemoryManager instance; return instance; }
  bool ensureFree(size_t, MemPool = MemPool::Default) { return true; }
  void registerSink(Sink) {}
  size_t freeBytes() const { return 256U * 1024 * 1024; }
};
}
