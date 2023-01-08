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
*/

#pragma once
#include "td/db/KeyValueAsync.h"
#include "tl-utils/common-utils.hpp"

namespace db {

using DbType = td::KeyValueAsync<td::Bits256, td::BufferSlice>;

template <typename T>
inline void db_get(DbType& db, td::Bits256 key, bool allow_not_found,
                   td::Promise<ton::tl_object_ptr<T>> promise) {
  db.get(key, [allow_not_found, promise = std::move(promise)](td::Result<DbType::GetResult> R) mutable {
    TRY_RESULT_PROMISE(promise, r, std::move(R));
    if (r.status == td::KeyValueReader::GetStatus::NotFound) {
      if (allow_not_found) {
        promise.set_value(nullptr);
      } else {
        promise.set_error(td::Status::Error("Key not found"));
      }
      return;
    }
    promise.set_result(ton::fetch_tl_object<T>(r.value, true));
  });
}

template <typename T>
inline td::Result<ton::tl_object_ptr<T>> db_get(td::KeyValue& db, td::Bits256 key, bool allow_not_found) {
  std::string value;
  TRY_RESULT(r, db.get(key.as_slice(), value));
  if (r == td::KeyValue::GetStatus::NotFound) {
    if (allow_not_found) {
      return nullptr;
    } else {
      return td::Status::Error("Key not found");
    }
  }
  return ton::fetch_tl_object<T>(td::Slice(value), true);
}

}  // namespace db