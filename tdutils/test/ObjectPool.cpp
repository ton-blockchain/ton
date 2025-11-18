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
#include "td/utils/tests.h"
#include "td/utils/port/thread.h"

#include <atomic>
#include <vector>

TEST(ObjectPool, basic) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;

  // Test basic allocation and release
  auto ptr1 = pool.create(42);
  CHECK(ptr1->value == 42);

  auto weak1 = ptr1.get_weak();
  CHECK(weak1.is_alive());

  pool.release(std::move(ptr1));
  CHECK(!weak1.is_alive());
}

TEST(ObjectPool, chunked_allocation) {
  class Counter {
   public:
    static std::atomic<int> construction_count;
    static std::atomic<int> destruction_count;

    Counter() {
      construction_count++;
    }
    ~Counter() {
      destruction_count++;
    }
    void clear() {}
  };

  Counter::construction_count = 0;
  Counter::destruction_count = 0;

  {
    td::ObjectPool<Counter> pool;
    std::vector<typename td::ObjectPool<Counter>::OwnerPtr> ptrs;

    // Allocate more than CHUNK_SIZE (64) to test chunked allocation
    for (int i = 0; i < 200; i++) {
      ptrs.push_back(pool.create());
    }

    // Verify all objects were constructed
    CHECK(Counter::construction_count >= 200);

    // Release half of them
    for (int i = 0; i < 100; i++) {
      pool.release(std::move(ptrs[i]));
    }

    // Reuse released objects
    for (int i = 0; i < 100; i++) {
      ptrs[i] = pool.create();
    }

    // Should have reused objects, not allocated many new ones
    // With chunked allocation, we allocate in multiples of 64
    CHECK(Counter::construction_count < 300);
  }

  // All objects should be destroyed when pool is destroyed
  CHECK(Counter::destruction_count == Counter::construction_count);
}

std::atomic<int> ObjectPool_chunked_allocation_Counter_construction_count{0};
std::atomic<int> ObjectPool_chunked_allocation_Counter_destruction_count{0};

class ObjectPool_chunked_allocation_Counter {
 public:
  static std::atomic<int> construction_count;
  static std::atomic<int> destruction_count;

  ObjectPool_chunked_allocation_Counter() {
    construction_count++;
  }
  ~ObjectPool_chunked_allocation_Counter() {
    destruction_count++;
  }
  void clear() {}
};

std::atomic<int> ObjectPool_chunked_allocation_Counter::construction_count{0};
std::atomic<int> ObjectPool_chunked_allocation_Counter::destruction_count{0};

TEST(ObjectPool, reuse) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;

  // Create and release an object
  auto ptr1 = pool.create();
  ptr1->value = 42;
  pool.release(std::move(ptr1));

  // Create another object - should reuse the previous one
  auto ptr2 = pool.create();
  CHECK(ptr2->value == 0);  // Should be cleared
}

TEST(ObjectPool, weak_ptr_safety) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;
  std::vector<typename td::ObjectPool<Node>::WeakPtr> weak_ptrs;

  // Create objects and store weak pointers
  for (int i = 0; i < 10; i++) {
    auto ptr = pool.create();
    ptr->value = i;
    weak_ptrs.push_back(ptr.get_weak());
    pool.release(std::move(ptr));
  }

  // All weak pointers should be dead after release
  for (auto &weak : weak_ptrs) {
    CHECK(!weak.is_alive());
  }

  // Create new objects - they should reuse the storage
  auto ptr = pool.create();
  ptr->value = 999;
  auto weak = ptr.get_weak();
  CHECK(weak.is_alive());

  // Old weak pointers should still be dead
  for (auto &old_weak : weak_ptrs) {
    CHECK(!old_weak.is_alive());
  }
}

TEST(ObjectPool, concurrent_stress) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;
  std::atomic<int> total_operations{0};
  const int num_threads = 4;
  const int operations_per_thread = 1000;

  std::vector<td::thread> threads;
  for (int t = 0; t < num_threads; t++) {
    threads.emplace_back([&pool, &total_operations, operations_per_thread]() {
      for (int i = 0; i < operations_per_thread; i++) {
        auto ptr = pool.create();
        ptr->value = i;
        CHECK(ptr->value == i);
        pool.release(std::move(ptr));
        total_operations++;
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  CHECK(total_operations == num_threads * operations_per_thread);
}

TEST(ObjectPool, generation_increment) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;

  auto ptr1 = pool.create();
  auto gen1 = ptr1.generation();
  auto weak1 = ptr1.get_weak();
  pool.release(std::move(ptr1));

  auto ptr2 = pool.create();
  auto gen2 = ptr2.generation();

  // Generation should have incremented
  CHECK(gen2 > gen1);
  CHECK(!weak1.is_alive());  // Old weak ptr should be dead
}

TEST(ObjectPool, empty_and_reset) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;

  auto ptr = pool.create();
  CHECK(!ptr.empty());

  ptr.reset();
  CHECK(ptr.empty());

  auto ptr2 = pool.create();
  CHECK(!ptr2.empty());
  auto ptr3 = std::move(ptr2);
  CHECK(ptr2.empty());
  CHECK(!ptr3.empty());
}

TEST(ObjectPool, create_empty) {
  class Node {
   public:
    int value = 0;
    void clear() {
      value = 0;
    }
  };

  td::ObjectPool<Node> pool;

  // Test create_empty (no initialization)
  auto ptr = pool.create_empty();
  CHECK(!ptr.empty());

  // Value should be default-constructed
  ptr->value = 123;
  CHECK(ptr->value == 123);
}
