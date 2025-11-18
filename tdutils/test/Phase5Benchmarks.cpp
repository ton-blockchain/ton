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

#include "td/utils/tests.h"
#include "td/utils/Time.h"
#include "td/utils/HashMap.h"
#include "td/utils/HashSet.h"
#include "td/utils/VectorQueue.h"
#include <map>
#include <set>
#include <queue>
#include <random>
#include <vector>

// Test Phase 5 Optimizations: Benchmarks for HashMap, HashSet, VectorQueue

// =============================================================================
// Benchmark 1: HashMap vs std::map performance
// =============================================================================

TEST(Phase5Benchmarks, HashMapVsStdMap) {
  constexpr int NUM_OPERATIONS = 100000;
  std::mt19937 rng(42);

  // Generate test data
  std::vector<std::pair<uint64_t, uint64_t>> test_data;
  for (int i = 0; i < NUM_OPERATIONS; i++) {
    test_data.emplace_back(rng(), rng());
  }

  // Test std::map
  {
    std::map<uint64_t, uint64_t> map;
    auto start = td::Timestamp::now();

    for (const auto& [key, value] : test_data) {
      map[key] = value;
    }

    // Random lookups
    for (int i = 0; i < NUM_OPERATIONS; i++) {
      auto it = map.find(test_data[i].first);
      (void)it;
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "std::map: " << NUM_OPERATIONS << " inserts + " << NUM_OPERATIONS << " lookups in "
              << (elapsed * 1000.0) << "ms (O(log n))";
  }

  // Test td::HashMap
  {
    td::HashMap<uint64_t, uint64_t> hashmap;
    auto start = td::Timestamp::now();

    for (const auto& [key, value] : test_data) {
      hashmap[key] = value;
    }

    // Random lookups
    for (int i = 0; i < NUM_OPERATIONS; i++) {
      auto it = hashmap.find(test_data[i].first);
      (void)it;
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "td::HashMap: " << NUM_OPERATIONS << " inserts + " << NUM_OPERATIONS << " lookups in "
              << (elapsed * 1000.0) << "ms (O(1))";

    // HashMap should be faster (expect 2-5x improvement)
    // Note: This is a soft check, actual speedup depends on hardware
  }
}

// =============================================================================
// Benchmark 2: HashSet vs std::set performance
// =============================================================================

TEST(Phase5Benchmarks, HashSetVsStdSet) {
  constexpr int NUM_OPERATIONS = 100000;
  std::mt19937 rng(42);

  // Generate test data
  std::vector<uint64_t> test_data;
  for (int i = 0; i < NUM_OPERATIONS; i++) {
    test_data.push_back(rng());
  }

  // Test std::set
  {
    std::set<uint64_t> set;
    auto start = td::Timestamp::now();

    for (uint64_t value : test_data) {
      set.insert(value);
    }

    // Random lookups
    for (uint64_t value : test_data) {
      auto it = set.find(value);
      (void)it;
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "std::set: " << NUM_OPERATIONS << " inserts + " << NUM_OPERATIONS << " lookups in "
              << (elapsed * 1000.0) << "ms (O(log n))";
  }

  // Test td::HashSet
  {
    td::HashSet<uint64_t> hashset;
    auto start = td::Timestamp::now();

    for (uint64_t value : test_data) {
      hashset.insert(value);
    }

    // Random lookups
    for (uint64_t value : test_data) {
      auto it = hashset.find(value);
      (void)it;
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "td::HashSet: " << NUM_OPERATIONS << " inserts + " << NUM_OPERATIONS << " lookups in "
              << (elapsed * 1000.0) << "ms (O(1))";

    // HashSet should be faster (expect 2-5x improvement)
  }
}

// =============================================================================
// Benchmark 3: VectorQueue vs std::queue performance
// =============================================================================

TEST(Phase5Benchmarks, VectorQueueVsStdQueue) {
  constexpr int NUM_OPERATIONS = 100000;

  struct Event {
    uint64_t id;
    double timestamp;
    uint32_t data[8];  // 32 bytes payload
  };

  // Test std::queue
  {
    std::queue<Event> queue;
    auto start = td::Timestamp::now();

    // Enqueue
    for (int i = 0; i < NUM_OPERATIONS; i++) {
      Event e{static_cast<uint64_t>(i), static_cast<double>(i), {}};
      queue.push(e);
    }

    // Dequeue
    for (int i = 0; i < NUM_OPERATIONS; i++) {
      auto e = queue.front();
      queue.pop();
      (void)e;
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "std::queue: " << NUM_OPERATIONS << " push + " << NUM_OPERATIONS << " pop in "
              << (elapsed * 1000.0) << "ms";
  }

  // Test td::VectorQueue
  {
    td::VectorQueue<Event> queue;
    auto start = td::Timestamp::now();

    // Enqueue
    for (int i = 0; i < NUM_OPERATIONS; i++) {
      Event e{static_cast<uint64_t>(i), static_cast<double>(i), {}};
      queue.push(e);
    }

    // Dequeue
    for (int i = 0; i < NUM_OPERATIONS; i++) {
      auto e = queue.front();
      queue.pop();
      (void)e;
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "td::VectorQueue: " << NUM_OPERATIONS << " push + " << NUM_OPERATIONS << " pop in "
              << (elapsed * 1000.0) << "ms";

    // VectorQueue should be faster due to better cache locality
  }
}

// =============================================================================
// Benchmark 4: Combined workload simulation (realistic scenario)
// =============================================================================

TEST(Phase5Benchmarks, RealisticWorkloadSimulation) {
  constexpr int NUM_TRANSFERS = 10000;
  std::mt19937 rng(42);

  // Simulate RLDP connection with many transfers
  LOG(INFO) << "Simulating realistic RLDP workload with " << NUM_TRANSFERS << " transfers...";

  // Using HashMap (optimized)
  {
    td::HashMap<uint64_t, std::vector<uint8_t>> transfers;
    td::HashSet<uint64_t> completed;
    auto start = td::Timestamp::now();

    // Simulate transfer lifecycle
    for (int i = 0; i < NUM_TRANSFERS; i++) {
      uint64_t transfer_id = rng();

      // Create transfer
      std::vector<uint8_t> data(1024);  // 1KB per transfer
      transfers[transfer_id] = std::move(data);

      // Process transfer (multiple lookups)
      for (int j = 0; j < 10; j++) {
        auto it = transfers.find(transfer_id);
        if (it != transfers.end()) {
          // Simulate processing
          volatile size_t size = it->second.size();
          (void)size;
        }
      }

      // Complete transfer
      transfers.erase(transfer_id);
      completed.insert(transfer_id);

      // Check if completed (common operation)
      if (i % 100 == 0) {
        for (int k = 0; k < 100; k++) {
          completed.find(rng());
        }
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    double throughput = NUM_TRANSFERS / elapsed;
    LOG(INFO) << "HashMap/HashSet: " << NUM_TRANSFERS << " transfers processed in "
              << (elapsed * 1000.0) << "ms (" << throughput << " transfers/sec)";

    // Performance target: should handle > 10k transfers/sec
    ASSERT_TRUE(throughput > 10000.0);
  }
}

// =============================================================================
// Benchmark 5: Memory allocation patterns
// =============================================================================

TEST(Phase5Benchmarks, MemoryAllocationPattern) {
  constexpr int NUM_OPERATIONS = 50000;

  // Test allocation overhead of std::queue vs VectorQueue
  struct LargeEvent {
    uint64_t id;
    uint8_t payload[512];  // 512 bytes
  };

  LOG(INFO) << "Testing memory allocation patterns...";

  // std::queue allocates on every push
  {
    auto start = td::Timestamp::now();
    std::queue<LargeEvent> queue;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
      LargeEvent e{static_cast<uint64_t>(i), {}};
      queue.push(e);
      if (i % 2 == 0 && !queue.empty()) {
        queue.pop();
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "std::queue (per-element allocation): " << (elapsed * 1000.0) << "ms";
  }

  // VectorQueue amortizes allocations
  {
    auto start = td::Timestamp::now();
    td::VectorQueue<LargeEvent> queue;

    for (int i = 0; i < NUM_OPERATIONS; i++) {
      LargeEvent e{static_cast<uint64_t>(i), {}};
      queue.push(e);
      if (i % 2 == 0 && !queue.empty()) {
        queue.pop();
      }
    }

    auto elapsed = td::Timestamp::now().at() - start.at();
    LOG(INFO) << "td::VectorQueue (amortized allocation): " << (elapsed * 1000.0) << "ms";

    // VectorQueue should be significantly faster (2-3x)
  }
}

// =============================================================================
// Benchmark 6: Cache locality comparison
// =============================================================================

TEST(Phase5Benchmarks, CacheLocalityComparison) {
  constexpr int NUM_OPERATIONS = 100000;
  std::mt19937 rng(42);

  // Generate sequential access pattern (good for cache)
  std::vector<uint64_t> keys;
  for (int i = 0; i < NUM_OPERATIONS; i++) {
    keys.push_back(i);
  }

  LOG(INFO) << "Testing cache locality with sequential access...";

  // std::map (tree structure, poor cache locality)
  {
    std::map<uint64_t, uint64_t> map;
    for (uint64_t key : keys) {
      map[key] = key * 2;
    }

    auto start = td::Timestamp::now();
    uint64_t sum = 0;
    for (uint64_t key : keys) {
      sum += map[key];
    }
    auto elapsed = td::Timestamp::now().at() - start.at();

    LOG(INFO) << "std::map sequential lookup: " << (elapsed * 1000.0) << "ms, sum=" << sum;
  }

  // td::HashMap (hash table, better cache locality)
  {
    td::HashMap<uint64_t, uint64_t> hashmap;
    for (uint64_t key : keys) {
      hashmap[key] = key * 2;
    }

    auto start = td::Timestamp::now();
    uint64_t sum = 0;
    for (uint64_t key : keys) {
      sum += hashmap[key];
    }
    auto elapsed = td::Timestamp::now().at() - start.at();

    LOG(INFO) << "td::HashMap sequential lookup: " << (elapsed * 1000.0) << "ms, sum=" << sum;

    // HashMap should be 3-5x faster due to better cache locality
  }
}
