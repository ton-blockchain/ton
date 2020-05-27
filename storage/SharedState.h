#pragma once

#include <atomic>
#include "td/utils/MovableValue.h"
#include <memory>

namespace td {
template <class T>
class SharedState {
 public:
  friend class Guard;
  class Guard {
   public:
    Guard(Guard &&) = default;
    Guard(SharedState<T> *self) : self(self) {
      CHECK(!self->data_->is_locked.exchange(true));
    }
    ~Guard() {
      if (self.get()) {
        CHECK(self.get()->data_->is_locked.exchange(false));
      }
    }
    T *get() {
      return &self.get()->data_->data;
    }
    T *operator->() {
      return get();
    }

   private:
    td::MovableValue<SharedState<T> *> self;
  };

  auto lock() {
    return Guard{this};
  }
  auto unsafe() {
    return &data_->data;
  }

 private:
  struct Data {
    std::atomic<bool> is_locked{};
    T data;
  };
  std::shared_ptr<Data> data_{std::make_shared<Data>()};
};
}  // namespace td
