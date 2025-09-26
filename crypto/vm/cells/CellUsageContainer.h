#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <mutex>

/**
 * Thread-safe container for storing cell usage tree nodes.
 * @tparam T node type
 */
template <typename T>
class CellUsageContainer {
  static constexpr size_t BLOCK_SIZE = std::max(1ul, 4096 / sizeof(T));
  static constexpr size_t DEFAULT_CAP = 512;

 public:
  explicit CellUsageContainer(size_t initial_size) : size_(0), cap_(0) {
    static_assert(std::atomic<T**>::is_always_lock_free);
    ensure_capacity(0, (std::max(DEFAULT_CAP, initial_size) + BLOCK_SIZE - 1) / BLOCK_SIZE);
    size_ = initial_size;
  }

  const T& operator[](size_t i) const {
    return pointer_[i / BLOCK_SIZE][i % BLOCK_SIZE];
  }

  T& operator[](size_t i) {
    return pointer_[i / BLOCK_SIZE][i % BLOCK_SIZE];
  }

  size_t emplace_back() {
    size_t pos = size_.fetch_add(1);
    size_t current_cap;
    while (pos / BLOCK_SIZE >= (current_cap = cap_.load())) {
      ensure_capacity(current_cap, 2 * current_cap);
    }
    return pos;
  }

  ~CellUsageContainer() {
    for (size_t i = 0; i < cap_; ++i) {
      delete[] pointer_[i];
    }

    for (auto& elem : storage_) {
      delete[] elem;
    }
  }

 private:
  std::atomic_size_t size_;
  std::atomic_size_t cap_;
  std::mutex resize_lock_;

  std::atomic<T**> pointer_;

  std::vector<T**> storage_;

  void ensure_capacity(size_t current_cap, size_t target_cap) {
    std::lock_guard lock(resize_lock_);
    if (current_cap != cap_) {
      return;
    }

    T** new_data = new T*[target_cap];
    for (size_t i = 0; i < cap_; ++i) {
      new_data[i] = pointer_[i];
    }
    for (size_t i = cap_; i < target_cap; ++i) {
      new_data[i] = new T[BLOCK_SIZE];
    }
    storage_.emplace_back(new_data);
    pointer_.store(new_data);
    cap_ = target_cap;
  }
};