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
#include "td/db/KeyValue.h"

#include <map>

namespace td {

struct Merger {
  virtual ~Merger() = default;
  virtual void merge_value_and_update(std::string &value, Slice update) = 0;
  virtual void merge_update_and_update(std::string  &left_update, Slice right_update) = 0;
};
class MemoryKeyValue : public KeyValue {
 public:
  MemoryKeyValue() = default;
  MemoryKeyValue(std::shared_ptr<Merger> merger) : merger_(std::move(merger)) {
  }
  Result<GetStatus> get(Slice key, std::string& value) override;
  Result<std::vector<GetStatus>> get_multi(td::Span<Slice> keys, std::vector<std::string> *values) override;
  Status for_each(std::function<Status(Slice, Slice)> f) override;
  Status for_each_in_range(Slice begin, Slice end, std::function<Status(Slice, Slice)> f) override;
  Status set(Slice key, Slice value) override;
  Status merge(Slice key, Slice value) override;
  Status erase(Slice key) override;
  Result<size_t> count(Slice prefix) override;

  Status begin_write_batch() override;
  Status commit_write_batch() override;
  Status abort_write_batch() override;

  Status begin_transaction() override;
  Status commit_transaction() override;
  Status abort_transaction() override;

  std::unique_ptr<KeyValueReader> snapshot() override;

  std::string stats() const override;

  UsageStats get_usage_stats() override {
    return usage_stats_;
  }

 private:
  static constexpr size_t buckets_n = 64;
  struct Bucket {
    std::mutex mutex;
    std::map<std::string, std::string, std::less<>> map;
  };
  struct Unlock {
    void operator()(Bucket* bucket) const {
      bucket->mutex.unlock();
    }
  };
  std::array<Bucket, buckets_n> buckets_{};
  int64 get_count_{0};
  UsageStats usage_stats_{};
  std::shared_ptr<Merger> merger_;

  std::unique_ptr<Bucket, Unlock> lock(Bucket& bucket) {
    bucket.mutex.lock();
    return std::unique_ptr<Bucket, Unlock>(&bucket);
  }
  std::unique_ptr<Bucket, Unlock> lock(td::Slice key);
};
}  // namespace td
