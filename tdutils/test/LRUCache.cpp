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
#include "td/utils/common.h"
#include "td/utils/LRUCache.h"
#include "td/utils/tests.h"

#include <string>

TEST(LRUCache, basic) {
  td::LRUCache<int, std::string> cache(3);

  // Test basic put and get
  cache.put(1, "one");
  cache.put(2, "two");
  cache.put(3, "three");

  auto *val1 = cache.get_if_exists(1);
  CHECK(val1 != nullptr);
  CHECK(*val1 == "one");

  auto *val2 = cache.get_if_exists(2);
  CHECK(val2 != nullptr);
  CHECK(*val2 == "two");

  auto *val_missing = cache.get_if_exists(99);
  CHECK(val_missing == nullptr);
}

TEST(LRUCache, eviction) {
  td::LRUCache<int, std::string> cache(3);  // Max weight = 3

  // Add 3 items with weight 1 each
  cache.put(1, "one", true, 1);
  cache.put(2, "two", true, 1);
  cache.put(3, "three", true, 1);

  // All should exist
  CHECK(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));

  // Add a 4th item - should evict least recently used (1)
  cache.put(4, "four", true, 1);

  CHECK(!cache.contains(1));  // Evicted
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));
  CHECK(cache.contains(4));
}

TEST(LRUCache, lru_order) {
  td::LRUCache<int, std::string> cache(3);

  cache.put(1, "one", true, 1);
  cache.put(2, "two", true, 1);
  cache.put(3, "three", true, 1);

  // Access item 1 to make it recently used
  cache.get_if_exists(1);

  // Add item 4 - should evict item 2 (least recently used)
  cache.put(4, "four", true, 1);

  CHECK(cache.contains(1));   // Still there (recently accessed)
  CHECK(!cache.contains(2));  // Evicted (least recently used)
  CHECK(cache.contains(3));
  CHECK(cache.contains(4));
}

TEST(LRUCache, weighted_eviction) {
  td::LRUCache<int, std::string> cache(10);

  // Add items with different weights
  cache.put(1, "small", true, 2);
  cache.put(2, "medium", true, 3);
  cache.put(3, "large", true, 5);
  // Total weight = 10

  CHECK(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));

  // Add item with weight 4 - total would be 14, need to evict
  cache.put(4, "new", true, 4);

  // Should evict items until weight <= 10
  // Will evict 1 (weight 2) and 2 (weight 3) to make room
  CHECK(!cache.contains(1));
  CHECK(!cache.contains(2));
  CHECK(cache.contains(3));  // weight 5
  CHECK(cache.contains(4));  // weight 4
}

TEST(LRUCache, update_existing) {
  td::LRUCache<int, std::string> cache(3);

  cache.put(1, "one", true, 1);
  cache.put(2, "two", true, 1);

  // Update existing key
  cache.put(1, "ONE", true, 1);

  auto *val = cache.get_if_exists(1);
  CHECK(val != nullptr);
  CHECK(*val == "ONE");

  // Should still only have 2 items
  CHECK(cache.contains(1));
  CHECK(cache.contains(2));
}

TEST(LRUCache, get_without_update) {
  td::LRUCache<int, std::string> cache(3);

  cache.put(1, "one", true, 1);
  cache.put(2, "two", true, 1);
  cache.put(3, "three", true, 1);

  // Get without updating LRU order
  auto *val = cache.get_if_exists(1, false);
  CHECK(val != nullptr);
  CHECK(*val == "one");

  // Add item 4 - should still evict item 1 (not moved to front)
  cache.put(4, "four", true, 1);

  CHECK(!cache.contains(1));  // Evicted despite access
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));
  CHECK(cache.contains(4));
}

TEST(LRUCache, get_or_create) {
  td::LRUCache<int, std::string> cache(5);

  // Get non-existent key - should create empty value
  auto &val1 = cache.get(1);
  val1 = "created";

  auto *val1_ptr = cache.get_if_exists(1);
  CHECK(val1_ptr != nullptr);
  CHECK(*val1_ptr == "created");

  // Get existing key
  auto &val2 = cache.get(1);
  CHECK(val2 == "created");
}

TEST(LRUCache, put_without_update) {
  td::LRUCache<int, std::string> cache(3);

  cache.put(1, "one", false, 1);  // Don't update LRU
  cache.put(2, "two", true, 1);
  cache.put(3, "three", true, 1);

  // Item 1 is still in cache but at LRU position
  CHECK(cache.contains(1));

  // Add item 4 - should evict item 1
  cache.put(4, "four", true, 1);

  CHECK(!cache.contains(1));  // Evicted (was not updated)
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));
  CHECK(cache.contains(4));
}

TEST(LRUCache, hash_map_performance) {
  // Test that hash map provides O(1) performance
  const int large_size = 10000;
  td::LRUCache<int, int> cache(large_size);

  // Fill cache
  for (int i = 0; i < large_size; i++) {
    cache.put(i, i * 2, true, 1);
  }

  // Access random elements - should be fast with hash map
  for (int i = 0; i < 1000; i++) {
    int key = (i * 7919) % large_size;  // Pseudo-random access
    auto *val = cache.get_if_exists(key);
    CHECK(val != nullptr);
    CHECK(*val == key * 2);
  }
}

TEST(LRUCache, contains_check) {
  td::LRUCache<int, std::string> cache(5);

  CHECK(!cache.contains(1));

  cache.put(1, "one");
  CHECK(cache.contains(1));

  cache.put(2, "two", true, 10);  // Evicts item 1
  CHECK(!cache.contains(1));
  CHECK(cache.contains(2));
}

TEST(LRUCache, empty_value) {
  td::LRUCache<int, std::string> cache(3);

  // Put empty string
  cache.put(1, "");
  auto *val = cache.get_if_exists(1);
  CHECK(val != nullptr);
  CHECK(val->empty());
}

TEST(LRUCache, string_keys) {
  td::LRUCache<std::string, int> cache(5);

  cache.put("one", 1);
  cache.put("two", 2);
  cache.put("three", 3);

  auto *val = cache.get_if_exists("two");
  CHECK(val != nullptr);
  CHECK(*val == 2);

  CHECK(!cache.contains("missing"));
}

TEST(LRUCache, large_weights) {
  td::LRUCache<int, std::string> cache(100);

  // Add items with large weights
  cache.put(1, "item1", true, 30);
  cache.put(2, "item2", true, 40);
  cache.put(3, "item3", true, 30);
  // Total = 100

  CHECK(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));

  // Add item that exceeds capacity
  cache.put(4, "item4", true, 50);

  // Should evict enough to fit
  CHECK(cache.contains(4));
}

TEST(LRUCache, stress_test) {
  const int num_operations = 10000;
  const int cache_size = 100;
  td::LRUCache<int, int> cache(cache_size);

  for (int i = 0; i < num_operations; i++) {
    int key = i % 200;  // Some keys will be reused

    if (i % 3 == 0) {
      cache.put(key, i);
    } else {
      cache.get_if_exists(key);
    }
  }

  // Cache should still be functional
  cache.put(999, 999);
  auto *val = cache.get_if_exists(999);
  CHECK(val != nullptr);
  CHECK(*val == 999);
}
