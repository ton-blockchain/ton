/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/

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
