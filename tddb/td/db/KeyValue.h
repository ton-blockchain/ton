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
#include "td/utils/Status.h"
#include "td/utils/Time.h"
#include "td/utils/logging.h"
#include "td/utils/Span.h"
#include "td/utils/ThreadSafeCounter.h"
#include <functional>
namespace td {
struct UsageStats {
  size_t get_count{};
  size_t get_found_count{};
  size_t get_not_found_count{};
  size_t set_count{};
  UsageStats operator+(const UsageStats& other) const {
    return UsageStats{.get_count = get_count + other.get_count,
                      .get_found_count = get_found_count + other.get_found_count,
                      .get_not_found_count = get_not_found_count + other.get_not_found_count,
                      .set_count = set_count + other.set_count};
  }
  UsageStats operator-(const UsageStats& other) const {
    return UsageStats{.get_count = get_count - other.get_count,
                      .get_found_count = get_found_count - other.get_found_count,
                      .get_not_found_count = get_not_found_count - other.get_not_found_count,
                      .set_count = set_count - other.set_count};
  }
  NamedStats to_named_stats() const {
    NamedStats ns;
    ns.stats_int["usage_get_count"] += get_count;
    ns.stats_int["usage_get_found_count"] += get_found_count;
    ns.stats_int["usage_get_not_found_count"] += get_not_found_count;
    ns.stats_int["usage_set_count"] += set_count;
    return ns;
  }
};
inline td::StringBuilder& operator<<(td::StringBuilder& sb, const UsageStats& stats) {
  sb << "get: " << stats.get_count << ", +" << stats.get_found_count << ", -" << stats.get_not_found_count;
  return sb;
}

class KeyValueReader {
 public:
  virtual ~KeyValueReader() = default;
  enum class GetStatus : int32 { Ok, NotFound };

  virtual Result<GetStatus> get(Slice key, std::string &value) = 0;
  virtual Result<std::vector<GetStatus>> get_multi(td::Span<Slice> keys, std::vector<std::string> *values) = 0;
  virtual Result<size_t> count(Slice prefix) = 0;
  virtual Status for_each(std::function<Status(Slice, Slice)> f) {
    return Status::Error("for_each is not supported");
  }
  virtual Status for_each_in_range(Slice begin, Slice end, std::function<Status(Slice, Slice)> f) {
    return td::Status::Error("foreach_range is not supported");
  }
};

class PrefixedKeyValueReader : public KeyValueReader {
 public:
  PrefixedKeyValueReader(std::shared_ptr<KeyValueReader> reader, Slice prefix)
      : reader_(std::move(reader)), prefix_(prefix.str()) {
  }
  Result<GetStatus> get(Slice key, std::string& value) override {
    return reader_->get(PSLICE() << prefix_ << key, value);
  }
  Result<std::vector<GetStatus>> get_multi(td::Span<Slice> keys, std::vector<std::string> *values) override {
    std::vector<Slice> prefixed_keys;
    prefixed_keys.reserve(keys.size());
    for (auto &key : keys) {
      prefixed_keys.push_back(PSLICE() << prefix_ << key);
    }
    return reader_->get_multi(prefixed_keys, values);
  }
  Result<size_t> count(Slice prefix) override {
    return reader_->count(PSLICE() << prefix_ << prefix);
  }

 private:
  std::shared_ptr<KeyValueReader> reader_;
  std::string prefix_;
};

class KeyValue : public KeyValueReader {
 public:
  virtual Status set(Slice key, Slice value) = 0;
  virtual Status erase(Slice key) = 0;
  virtual Status merge(Slice key, Slice value) {
    return Status::Error("merge is not supported");
  }
  virtual Status run_gc() {
    return Status::OK();
  }

  virtual Status begin_write_batch() = 0;
  virtual Status commit_write_batch() = 0;
  virtual Status abort_write_batch() = 0;

  virtual Status begin_transaction() = 0;
  virtual Status commit_transaction() = 0;
  virtual Status abort_transaction() = 0;
  // Desctructor will abort transaction

  virtual std::unique_ptr<KeyValueReader> snapshot() = 0;

  virtual std::string stats() const {
    return "";
  }
  virtual Status flush() {
    return Status::OK();
  }
  virtual UsageStats get_usage_stats() {
    return {};
  }
};
class PrefixedKeyValue : public KeyValue {
 public:
  PrefixedKeyValue(std::shared_ptr<KeyValue> kv, Slice prefix) : kv_(std::move(kv)), prefix_(prefix.str()) {
  }
  Result<GetStatus> get(Slice key, std::string& value) override {
    return kv_->get(PSLICE() << prefix_ << key, value);
  }
  Result<std::vector<GetStatus>> get_multi(td::Span<Slice> keys, std::vector<std::string> *values) override {
    std::vector<Slice> prefixed_keys;
    prefixed_keys.reserve(keys.size());
    for (auto &key : keys) {
      prefixed_keys.push_back(PSLICE() << prefix_ << key);
    }
    return kv_->get_multi(prefixed_keys, values);
  }
  Result<size_t> count(Slice prefix) override {
    return kv_->count(PSLICE() << prefix_ << prefix);
  }
  Status set(Slice key, Slice value) override {
    return kv_->set(PSLICE() << prefix_ << key, value);
  }
  Status erase(Slice key) override {
    return kv_->erase(PSLICE() << prefix_ << key);
  }

  Status begin_write_batch() override {
    return kv_->begin_write_batch();
  }
  Status commit_write_batch() override {
    return kv_->commit_write_batch();
  }
  Status abort_write_batch() override {
    return kv_->abort_write_batch();
  }

  Status begin_transaction() override {
    return kv_->begin_transaction();
  }
  Status commit_transaction() override {
    return kv_->commit_transaction();
  }
  Status abort_transaction() override {
    return kv_->abort_transaction();
  }
  // Desctructor will abort transaction

  std::unique_ptr<KeyValueReader> snapshot() override {
    return kv_->snapshot();
  }

  std::string stats() const override {
    return kv_->stats();
  }
  Status flush() override {
    return kv_->flush();
  }

 private:
  std::shared_ptr<KeyValue> kv_;
  std::string prefix_;
};

}  // namespace td
