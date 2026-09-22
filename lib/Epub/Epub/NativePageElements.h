#pragma once

#if defined(CROSSPOINT_NATIVE_TEXT) && CROSSPOINT_NATIVE_TEXT
#include <cstddef>
#include <memory>

class Page;
class PageElement;

// Page element ownership uses the same bounded native heap as its line payloads.
class NativePageElements {
  friend class Page;
  using Element = std::unique_ptr<PageElement>;
  Element* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  bool reserve(size_t count);
  bool append(Element element);

 public:
  NativePageElements() = default;
  ~NativePageElements();
  NativePageElements(const NativePageElements&) = delete;
  NativePageElements& operator=(const NativePageElements&) = delete;
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }
  Element& operator[](size_t index) { return data_[index]; }
  const Element& operator[](size_t index) const { return data_[index]; }
  Element& front() { return data_[0]; }
  const Element& front() const { return data_[0]; }
  Element& back() { return data_[size_ - 1]; }
  const Element& back() const { return data_[size_ - 1]; }
  Element* begin() { return data_; }
  const Element* begin() const { return data_; }
  Element* end() { return size_ ? data_ + size_ : data_; }
  const Element* end() const { return size_ ? data_ + size_ : data_; }
};
#endif
