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
#include "refcnt.hpp"

#include "td/utils/ScopeGuard.h"

namespace td {

Ref<CntObject> CntObject::clone() const {
  return Ref<CntObject>{make_copy(), Ref<CntObject>::acquire_t()};
}

namespace detail {
struct SafeDeleter {
 public:
  thread_local static td::int64 delete_count;
  void retire(const CntObject *ptr) {
    if (is_active_) {
      to_delete_.push_back(ptr);
      return;
    }
    is_active_ = true;
    SCOPE_EXIT {
      is_active_ = false;
    };
    delete ptr;
    delete_count++;
    while (!to_delete_.empty()) {
      auto *ptr = to_delete_.back();
      to_delete_.pop_back();
      delete_count++;
      delete ptr;
    }
  }

 private:
  std::vector<const CntObject *> to_delete_;
  bool is_active_{false};
};
thread_local td::int64 SafeDeleter::delete_count{0};

TD_THREAD_LOCAL SafeDeleter *deleter;
void safe_delete(const CntObject *ptr) {
  init_thread_local<SafeDeleter>(deleter);
  deleter->retire(ptr);
}
}  // namespace detail
int64 ref_get_delete_count() {
  return detail::SafeDeleter::delete_count;
}
}  // namespace td
