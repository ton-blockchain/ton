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

#include <map>
#include <memory>
#include "List.h"
#include "check.h"
#include "common.h"

namespace td {

template <typename K, typename V>
class LRUCache {
 public:
  explicit LRUCache(uint64 max_size) : max_size_(max_size) {
  }
  LRUCache(const LRUCache&) = delete;
  LRUCache& operator=(const LRUCache&) = delete;

  V* get_if_exists(const K& key, bool update = true) {
    auto it = cache_.find(key);
    if (unlikely(it == cache_.end())) {
      return nullptr;
    }
    Entry* entry = it->second.get();
    if (likely(update)) {
      entry->remove();
      lru_.put(entry);
    }
    return &entry->value;
  }

  bool contains(const K& key) const {
    return cache_.find(key) != cache_.end();
  }

  bool put(const K& key, V value, bool update = true, uint64 weight = 1) {
    bool added = false;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      update = true;
      auto entry = std::make_unique<Entry>(key, std::move(value), weight);
      Entry* entry_ptr = entry.get();
      cache_.emplace(key, std::move(entry));
      added = true;
      total_weight_ += weight;
      if (update) {
        lru_.put(entry_ptr);
        cleanup();
      }
    } else {
      it->second->value = std::move(value);
      if (update) {
        it->second->remove();
        lru_.put(it->second.get());
        cleanup();
      }
    }
    return added;
  }

  V& get(const K& key, bool update = true, uint64 weight = 1) {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      update = true;
      auto entry = std::make_unique<Entry>(key, weight);
      Entry* entry_ptr = entry.get();
      auto [new_it, _] = cache_.emplace(key, std::move(entry));
      total_weight_ += weight;
      if (update) {
        lru_.put(entry_ptr);
        cleanup();
      }
      return new_it->second->value;
    } else {
      if (update) {
        it->second->remove();
        lru_.put(it->second.get());
        cleanup();
      }
      return it->second->value;
    }
  }

 private:
  struct Entry : ListNode {
    Entry(K key, uint64 weight) : key(std::move(key)), weight(weight) {
    }
    Entry(K key, V value, uint64 weight) : key(std::move(key)), value(std::move(value)), weight(weight) {
    }
    K key;
    V value;
    uint64 weight;
  };

  std::map<K, std::unique_ptr<Entry>> cache_;
  ListNode lru_;
  uint64 max_size_;
  uint64 total_weight_ = 0;

  void cleanup() {
    while (total_weight_ > max_size_ && cache_.size() > 1) {
      auto to_remove = (Entry*)lru_.get();
      CHECK(to_remove);
      to_remove->remove();
      total_weight_ -= to_remove->weight;
      cache_.erase(to_remove->key);
    }
  }
};

}  // namespace td
