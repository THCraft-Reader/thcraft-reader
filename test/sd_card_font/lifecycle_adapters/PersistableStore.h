#pragma once

#include <string>

// Native persistence boundary: production settings fields remain unchanged, but
// tests can detect accidental writes without depending on Arduino JSON/SD stores.
namespace probe {
inline unsigned settingsWrites = 0;
}
template <typename T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
  bool saveToFile() const {
    ++probe::settingsWrites;
    return true;
  }
};
