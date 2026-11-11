#pragma once

// Host tests exercise the store's JSON contract without writing to the SD card.
template <typename T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
  bool saveToFile() const { return true; }
};
