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
#include "td/db/MemoryKeyValue.h"

#include "td/utils/format.h"
#include "td/utils/Span.h"

namespace td {
Result<MemoryKeyValue::GetStatus> MemoryKeyValue::get(Slice key, std::string &value) {
  auto bucket = lock(key);
  auto &map = bucket->map;

  usage_stats_.get_count++;
  auto it = map.find(key);
  if (it == map.end()) {
    usage_stats_.get_not_found_count++;
    return GetStatus::NotFound;
  }
  value = it->second;
  usage_stats_.get_found_count++;
  return GetStatus::Ok;
}

std::unique_ptr<MemoryKeyValue::Bucket, MemoryKeyValue::Unlock> MemoryKeyValue::lock(td::Slice key) {
  auto bucket_id = std::hash<std::string_view>()(std::string_view(key.data(), key.size())) % buckets_.size();
  return lock(buckets_[bucket_id]);
}

Result<std::vector<MemoryKeyValue::GetStatus>> MemoryKeyValue::get_multi(td::Span<Slice> keys,
                                                                        std::vector<std::string> *values) {
  values->resize(keys.size());
  std::vector<GetStatus> res;
  res.reserve(keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    res.push_back(get(keys[i], (*values)[i]).move_as_ok());
  }
  return res;
}

Status MemoryKeyValue::for_each(std::function<Status(Slice, Slice)> f) {
  for (auto &unlocked_bucket : buckets_) {
    auto bucket = lock(unlocked_bucket);
    for (auto &it : bucket->map) {
      TRY_STATUS(f(it.first, it.second));
    }
  }
  return Status::OK();
}

Status MemoryKeyValue::for_each_in_range(Slice begin, Slice end, std::function<Status(Slice, Slice)> f) {
  for (auto &unlocked_bucket : buckets_) {
    auto bucket = lock(unlocked_bucket);
    auto &map = bucket->map;
    for (auto it = map.lower_bound(begin); it != map.end(); it++) {
      if (it->first < end) {
        TRY_STATUS(f(it->first, it->second));
      } else {
        break;
      }
    }
  }
  return Status::OK();
}
Status MemoryKeyValue::set(Slice key, Slice value) {
  auto bucket = lock(key);
  auto &map = bucket->map;

  usage_stats_.set_count++;
  map[key.str()] = value.str();
  return Status::OK();
}
Status MemoryKeyValue::merge(Slice key, Slice update) {
  CHECK(merger_);
  auto bucket = lock(key);
  auto &map = bucket->map;
  auto &value = map[key.str()];
  merger_->merge_value_and_update(value, update);
  if (value.empty()) {
    map.erase(key.str());
  }
  return td::Status::OK();
}
Status MemoryKeyValue::erase(Slice key) {
  auto bucket = lock(key);
  auto &map = bucket->map;
  auto it = map.find(key);
  if (it != map.end()) {
    map.erase(it);
  }
  return Status::OK();
}

Result<size_t> MemoryKeyValue::count(Slice prefix) {
  size_t res = 0;
  for (auto &unlocked_bucket : buckets_) {
    auto bucket = lock(unlocked_bucket);
    auto &map = bucket->map;
    for (auto it = map.lower_bound(prefix); it != map.end(); it++) {
      if (Slice(it->first).truncate(prefix.size()) != prefix) {
        break;
      }
      res++;
    }
  }
  return res;
}

std::unique_ptr<KeyValueReader> MemoryKeyValue::snapshot() {
  auto res = std::make_unique<MemoryKeyValue>();
  for (size_t i = 0; i < buckets_.size(); i++) {
    auto bucket = lock(buckets_[i]);
    res->buckets_[i].map = bucket->map;
  }
  return std::move(res);
}

std::string MemoryKeyValue::stats() const {
  return PSTRING() << "MemoryKeyValueStats{" << tag("get_count", get_count_) << "}";
}
Status MemoryKeyValue::begin_write_batch() {
  return Status::OK();
}
Status MemoryKeyValue::commit_write_batch() {
  return Status::OK();
}
Status MemoryKeyValue::abort_write_batch() {
  UNREACHABLE();
}

Status MemoryKeyValue::begin_transaction() {
  UNREACHABLE();
}
Status MemoryKeyValue::commit_transaction() {
  UNREACHABLE();
}
Status MemoryKeyValue::abort_transaction() {
  UNREACHABLE();
}
}  // namespace td
