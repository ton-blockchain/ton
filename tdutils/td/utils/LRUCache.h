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

#include <memory>
#include <set>

#include "List.h"
#include "check.h"

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
    if (it == cache_.end()) {
      return nullptr;
    }
    Entry* entry = it->get();
    if (update) {
      entry->remove();
      lru_.put(entry);
    }
    return &entry->value;
  }

  bool contains(const K& key) const {
    return cache_.contains(key);
  }

  bool put(const K& key, V value, bool update = true, uint64 weight = 1) {
    bool added = false;
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      update = true;
      it = cache_.insert(std::make_unique<Entry>(key, std::move(value), weight)).first;
      added = true;
      total_weight_ += weight;
    } else {
      (*it)->value = std::move(value);
      if (update) {
        (*it)->remove();
      }
    }
    if (update) {
      lru_.put(it->get());
      cleanup();
    }
    return added;
  }

  V& get(const K& key, bool update = true, uint64 weight = 1) {
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      update = true;
      it = cache_.insert(std::make_unique<Entry>(key, weight)).first;
      total_weight_ += weight;
    } else if (update) {
      (*it)->remove();
    }
    V& result = (*it)->value;
    if (update) {
      lru_.put(it->get());
      cleanup();
    }
    return result;
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
  struct Cmp {
    using is_transparent = void;
    bool operator()(const std::unique_ptr<Entry>& a, const std::unique_ptr<Entry>& b) const {
      return a->key < b->key;
    }
    bool operator()(const std::unique_ptr<Entry>& a, const K& b) const {
      return a->key < b;
    }
    bool operator()(const K& a, const std::unique_ptr<Entry>& b) const {
      return a < b->key;
    }
  };
  std::set<std::unique_ptr<Entry>, Cmp> cache_;
  ListNode lru_;
  uint64 max_size_;
  uint64 total_weight_ = 0;

  void cleanup() {
    while (total_weight_ > max_size_ && cache_.size() > 1) {
      auto to_remove = (Entry*)lru_.get();
      CHECK(to_remove);
      to_remove->remove();
      total_weight_ -= to_remove->weight;
      cache_.erase(cache_.find(to_remove->key));
    }
  }
};

}  // namespace td
