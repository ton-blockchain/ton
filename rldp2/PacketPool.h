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

#include "td/utils/buffer.h"
#include <memory>
#include <vector>

namespace ton {
namespace rldp2 {

/**
 * Thread-local memory pool for frequently allocated packet structures.
 * Reduces allocation overhead in high-throughput network scenarios.
 */
template<typename T>
class ObjectPool {
public:
  static constexpr size_t kMaxFreeList = 512;  // Max objects in free list

  /**
   * Get an object from the pool or allocate a new one.
   */
  static std::unique_ptr<T> acquire() {
    auto& pool = get_thread_pool();

    if (!pool.free_list.empty()) {
      auto obj = std::move(pool.free_list.back());
      pool.free_list.pop_back();
      return obj;
    }

    return std::make_unique<T>();
  }

  /**
   * Return an object to the pool for reuse.
   */
  static void release(std::unique_ptr<T> obj) {
    if (!obj) {
      return;
    }

    auto& pool = get_thread_pool();

    if (pool.free_list.size() < kMaxFreeList) {
      pool.free_list.push_back(std::move(obj));
    }
  }

  /**
   * Get pool size (for monitoring).
   */
  static size_t pool_size() {
    auto& pool = get_thread_pool();
    return pool.free_list.size();
  }

private:
  struct ThreadLocalPool {
    std::vector<std::unique_ptr<T>> free_list;

    ThreadLocalPool() {
      free_list.reserve(kMaxFreeList / 2);
    }
  };

  static ThreadLocalPool& get_thread_pool() {
    static thread_local ThreadLocalPool pool;
    return pool;
  }
};

// Specialized pool for buffer slices (frequently used in packet handling)
class BufferSlicePool {
public:
  /**
   * Get a BufferSlice of the specified size from the pool.
   * Reuses cached buffers of similar size when available.
   */
  static td::BufferSlice acquire(size_t size);

  /**
   * Return a BufferSlice to the pool for potential reuse.
   */
  static void release(td::BufferSlice buffer);

  /**
   * Get pool statistics.
   */
  struct Stats {
    size_t total_allocations{0};
    size_t pool_hits{0};
    size_t cached_buffers{0};
  };

  static Stats get_stats();
  static void reset_stats();

private:
  static constexpr size_t kMaxCachedBuffers = 128;
  static constexpr size_t kMinBufferSize = 64;
  static constexpr size_t kMaxBufferSize = 128 * 1024;  // 128KB

  struct BufferEntry {
    size_t size;
    td::BufferSlice buffer;
  };

  struct ThreadLocalPool {
    std::vector<BufferEntry> cached_buffers;
    Stats stats;
  };

  static ThreadLocalPool& get_thread_pool();
};

}  // namespace rldp2
}  // namespace ton
