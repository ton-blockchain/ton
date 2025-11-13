#pragma once
#include <atomic>
#include <memory>
#include <mutex>

#include "td/utils/int_types.h"

namespace td {

template <class T, bool IsTArithmetic = std::is_arithmetic<T>::value>
class SharedValue;

template <class T>
class SharedValue<T, false> {
 public:
  explicit SharedValue(T value) : inner_(std::make_shared<Inner>(std::move(value))) {
    load();
  }
  SharedValue() : SharedValue(T()) {
  }
  void set_value(T new_value) {
    std::lock_guard<std::mutex> guard(inner_->mutex_);
    inner_->value_ = std::move(new_value);
    inner_->generation_++;
  }
  bool changed() const {
    return generation_ != inner_->generation_.load();
  }
  const T &load_cached() const {
    return value_;
  }
  const T &load() const {
    if (!changed()) {
      return load_cached();
    }
    std::lock_guard<std::mutex> guard(inner_->mutex_);
    generation_ = inner_->generation_.load();
    value_ = inner_->value_;
    return value_;
  }

 private:
  struct Inner {
    std::mutex mutex_;
    std::atomic<td::uint64> generation_{1};
    T value_;
    Inner(T value) : value_(std::move(value)) {
    }
  };
  mutable td::uint64 generation_{0};
  mutable T value_{};
  std::shared_ptr<Inner> inner_;
};

template <class T>
class SharedValue<T, true> {
 public:
  explicit SharedValue(T value) : inner_(std::make_shared<Inner>(std::move(value))) {
    load();
  }
  SharedValue() : SharedValue(T()) {
  }
  void set_value(T new_value) {
    inner_->value_ = std::move(new_value);
  }
  bool changed() const {
    return value_ != inner_->value_.load();
  }
  T load_cached() const {
    return value_;
  }
  T load() const {
    value_ = inner_->value_;
    return value_;
  }

 private:
  struct Inner {
    std::atomic<T> value_;
    Inner(T value) : value_(std::move(value)) {
    }
  };
  mutable T value_{};
  std::shared_ptr<Inner> inner_;
};
}  // namespace td
