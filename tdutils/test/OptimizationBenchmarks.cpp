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
#include "td/utils/ObjectPool.h"
#include "td/utils/LRUCache.h"
#include "td/utils/bits.h"
#include "td/utils/tests.h"
#include "td/utils/Time.h"
#include "td/utils/port/thread.h"
#include "td/utils/Random.h"

#include <vector>
#include <chrono>

// Benchmark ObjectPool chunked allocation performance
TEST(OptimizationBenchmarks, ObjectPool_chunked_allocation) {
  class Node {
   public:
    int data[10] = {0};  // Some data to make object non-trivial
    void clear() {
      for (int i = 0; i < 10; i++) {
        data[i] = 0;
      }
    }
  };

  td::ObjectPool<Node> pool;
  const int num_objects = 10000;

  auto start = std::chrono::high_resolution_clock::now();

  // Allocate many objects - should benefit from chunked allocation
  std::vector<typename td::ObjectPool<Node>::OwnerPtr> objects;
  for (int i = 0; i < num_objects; i++) {
    objects.push_back(pool.create());
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  // With chunked allocation, this should be fast (< 1ms for 10k objects)
  LOG(INFO) << "ObjectPool allocation of " << num_objects << " objects: " << duration.count() << " us";

  // Cleanup
  for (auto &obj : objects) {
    pool.release(std::move(obj));
  }
}

// Benchmark ObjectPool reuse performance
TEST(OptimizationBenchmarks, ObjectPool_reuse) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;
  const int num_cycles = 10000;

  auto start = std::chrono::high_resolution_clock::now();

  // Allocate and release repeatedly - should reuse objects
  for (int i = 0; i < num_cycles; i++) {
    auto obj = pool.create();
    obj->value = i;
    pool.release(std::move(obj));
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  LOG(INFO) << "ObjectPool " << num_cycles << " alloc/free cycles: " << duration.count() << " us";

  // With good reuse, this should be very fast
  CHECK(duration.count() < 5000);  // Should complete in < 5ms
}

// Benchmark LRUCache hash map performance (O(1) vs O(log n))
TEST(OptimizationBenchmarks, LRUCache_hash_map_lookup) {
  const int cache_size = 10000;
  td::LRUCache<int, int> cache(cache_size);

  // Fill cache
  for (int i = 0; i < cache_size; i++) {
    cache.put(i, i * 2);
  }

  const int num_lookups = 100000;
  auto start = std::chrono::high_resolution_clock::now();

  // Random lookups - should be O(1) with hash map
  for (int i = 0; i < num_lookups; i++) {
    int key = td::Random::fast(0, cache_size - 1);
    auto *val = cache.get_if_exists(key);
    (void)val;  // Suppress unused warning
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  LOG(INFO) << "LRUCache " << num_lookups << " random lookups in " << cache_size << " items: "
            << duration.count() << " us";

  // With O(1) hash map, this should be fast
  // With O(log n) set, this would be ~20x slower
  CHECK(duration.count() < 50000);  // Should complete in < 50ms
}

// Benchmark bit manipulation optimizations
TEST(OptimizationBenchmarks, bits_non_zero_optimization) {
  const int num_operations = 1000000;
  std::vector<td::uint32> test_values;

  // Generate non-zero test values
  for (int i = 0; i < 1000; i++) {
    test_values.push_back(td::Random::fast(1, 0xFFFFFFFF));
  }

  auto start = std::chrono::high_resolution_clock::now();

  // Test optimized non-zero functions
  volatile int result = 0;
  for (int i = 0; i < num_operations; i++) {
    td::uint32 val = test_values[i % test_values.size()];
    result += td::count_leading_zeroes_non_zero32(val);
    result += td::count_trailing_zeroes_non_zero32(val);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  LOG(INFO) << "Bit operations " << num_operations << " calls: " << duration.count() << " us";
  LOG(INFO) << "Result: " << result;  // Prevent optimization away

  // Should be very fast with direct builtin calls
  CHECK(duration.count() < 10000);  // Should complete in < 10ms
}

// Benchmark concurrent ObjectPool performance
TEST(OptimizationBenchmarks, ObjectPool_concurrent) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;
  const int num_threads = 4;
  const int operations_per_thread = 10000;

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<td::thread> threads;
  for (int t = 0; t < num_threads; t++) {
    threads.emplace_back([&pool, operations_per_thread]() {
      for (int i = 0; i < operations_per_thread; i++) {
        auto obj = pool.create();
        obj->value = i;
        pool.release(std::move(obj));
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  LOG(INFO) << "ObjectPool concurrent " << (num_threads * operations_per_thread)
            << " operations across " << num_threads << " threads: " << duration.count() << " ms";

  // With optimized memory ordering and chunking, should be reasonably fast
  CHECK(duration.count() < 1000);  // Should complete in < 1 second
}

// Benchmark LRUCache eviction performance
TEST(OptimizationBenchmarks, LRUCache_eviction) {
  const int cache_size = 1000;
  td::LRUCache<int, std::string> cache(cache_size);

  const int num_operations = 10000;
  auto start = std::chrono::high_resolution_clock::now();

  // Add more items than cache size - tests eviction
  for (int i = 0; i < num_operations; i++) {
    cache.put(i, "value_" + std::to_string(i), true, 1);
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  LOG(INFO) << "LRUCache " << num_operations << " insertions with eviction: "
            << duration.count() << " us";

  // With hash map, eviction should be efficient
  CHECK(duration.count() < 100000);  // Should complete in < 100ms
}

// Memory locality benchmark for chunked allocation
TEST(OptimizationBenchmarks, ObjectPool_memory_locality) {
  class Node {
   public:
    int data[16] = {0};  // 64 bytes
    void clear() {
      for (int i = 0; i < 16; i++) {
        data[i] = 0;
      }
    }
  };

  td::ObjectPool<Node> pool;
  const int num_objects = 1000;
  std::vector<typename td::ObjectPool<Node>::OwnerPtr> objects;

  // Allocate objects - should be contiguous in chunks
  for (int i = 0; i < num_objects; i++) {
    objects.push_back(pool.create());
  }

  auto start = std::chrono::high_resolution_clock::now();

  // Sequential access - should benefit from cache locality
  volatile int sum = 0;
  for (auto &obj : objects) {
    for (int i = 0; i < 16; i++) {
      sum += obj->data[i];
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  LOG(INFO) << "Sequential access of " << num_objects << " objects: " << duration.count() << " us";
  LOG(INFO) << "Sum: " << sum;  // Prevent optimization away

  // With good locality, this should be fast
  CHECK(duration.count() < 1000);  // Should complete in < 1ms
}

// Test branch prediction hints effectiveness
TEST(OptimizationBenchmarks, branch_prediction_hints) {
  const int num_iterations = 1000000;
  int hit_count = 0;
  int miss_count = 0;

  auto start = std::chrono::high_resolution_clock::now();

  // Simulate typical cache behavior: 80% hits, 20% misses
  for (int i = 0; i < num_iterations; i++) {
    bool is_hit = (i % 5) != 0;  // 80% true

    if (td::likely(is_hit)) {
      hit_count++;
    } else {
      miss_count++;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  LOG(INFO) << "Branch prediction test " << num_iterations << " iterations: "
            << duration.count() << " us";
  LOG(INFO) << "Hits: " << hit_count << ", Misses: " << miss_count;

  // With good branch prediction, this should be very fast
  CHECK(duration.count() < 5000);  // Should complete in < 5ms
  CHECK(hit_count == 800000);
  CHECK(miss_count == 200000);
}
