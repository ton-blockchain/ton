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

#include "interfaces/liteserver.h"
#include <map>

namespace ton::validator {

class LiteServerCacheImpl : public LiteServerCache {
 public:
  void start_up() override {
    alarm();
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(60.0);
    if (queries_cnt_ > 0 || !send_message_cache_.empty()) {
      LOG(WARNING) << "LS Cache stats: " << queries_cnt_ << " queries, " << queries_hit_cnt_ << " hits; "
                   << cache_.size() << " entries, size=" << total_size_ << "/" << MAX_CACHE_SIZE << ";   "
                   << send_message_cache_.size() << " different sendMessage queries, " << send_message_error_cnt_
                   << " duplicates";
      queries_cnt_ = 0;
      queries_hit_cnt_ = 0;
      send_message_cache_.clear();
      send_message_error_cnt_ = 0;
    }
  }

  void lookup(td::Bits256 key, td::Promise<td::BufferSlice> promise) override {
    ++queries_cnt_;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      promise.set_error(td::Status::Error("not found"));
      return;
    }
    ++queries_hit_cnt_;
    auto entry = it->second.get();
    entry->remove();
    lru_.put(entry);
    promise.set_value(entry->value_.clone());
  }

  void update(td::Bits256 key, td::BufferSlice value) override {
    std::unique_ptr<CacheEntry> &entry = cache_[key];
    if (entry == nullptr) {
      entry = std::make_unique<CacheEntry>(key, std::move(value));
    } else {
      total_size_ -= entry->size();
      entry->value_ = std::move(value);
      entry->remove();
    }
    lru_.put(entry.get());
    total_size_ += entry->size();

    while (total_size_ > MAX_CACHE_SIZE) {
      auto to_remove = (CacheEntry *)lru_.get();
      CHECK(to_remove);
      total_size_ -= to_remove->size();
      to_remove->remove();
      cache_.erase(to_remove->key_);
    }
  }

  void process_send_message(td::Bits256 key, td::Promise<td::Unit> promise) override {
    if (send_message_cache_.insert(key).second) {
      promise.set_result(td::Unit());
    } else {
      ++send_message_error_cnt_;
      promise.set_error(td::Status::Error("duplicate message"));
    }
  }

 private:
  struct CacheEntry : public td::ListNode {
    explicit CacheEntry(td::Bits256 key, td::BufferSlice value) : key_(key), value_(std::move(value)) {
    }
    td::Bits256 key_;
    td::BufferSlice value_;

    size_t size() const {
      return value_.size() + 32 * 2;
    }
  };

  std::map<td::Bits256, std::unique_ptr<CacheEntry>> cache_;
  td::ListNode lru_;
  size_t total_size_ = 0;

  size_t queries_cnt_ = 0, queries_hit_cnt_ = 0;

  std::set<td::Bits256> send_message_cache_;
  size_t send_message_error_cnt_ = 0;

  static const size_t MAX_CACHE_SIZE = 64 << 20;
};

}  // namespace ton::validator
