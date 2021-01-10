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

#include <atomic>

#include "td/utils/common.h"
#include "td/utils/port/thread.h"
namespace td {
template <class T>

class AtomicRead {
 public:
  void read(T &dest) const {
    while (true) {
      static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
      auto version_before = version.load();
      memcpy(&dest, &value, sizeof(dest));
      auto version_after = version.load();
      if (version_before == version_after && version_before % 2 == 0) {
        break;
      }
      td::this_thread::yield();
    }
  }

  friend struct Write;
  struct Write {
    explicit Write(AtomicRead *read) {
      read->do_lock();
      ptr.reset(read);
    }
    struct Destructor {
      void operator()(AtomicRead *read) const {
        read->do_unlock();
      }
    };
    T &operator*() {
      return value();
    }
    T *operator->() {
      return &value();
    }
    T &value() {
      CHECK(ptr);
      return ptr->value;
    }

   private:
    std::unique_ptr<AtomicRead, Destructor> ptr;
  };
  Write lock() {
    return Write(this);
  }

 private:
  std::atomic<td::uint64> version{0};
  T value;

  void do_lock() {
    CHECK(++version % 2 == 1);
  }
  void do_unlock() {
    CHECK(++version % 2 == 0);
  }
};
};  // namespace td
