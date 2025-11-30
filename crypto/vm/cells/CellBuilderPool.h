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

#include "CellBuilder.h"
#include <memory>
#include <vector>
#include <atomic>

namespace vm {

/**
 * Thread-local memory pool for CellBuilder objects to reduce allocation overhead.
 * CellBuilder is frequently allocated during cell construction, making it a hot spot.
 *
 * This pool uses a simple free-list design with thread-local storage to avoid
 * synchronization overhead.
 */
class CellBuilderPool {
public:
  static constexpr size_t kChunkSize = 128;  // Objects per chunk
  static constexpr size_t kMaxFreeList = 256;  // Max objects in free list

  /**
   * Get a CellBuilder from the pool or allocate a new one.
   */
  static std::unique_ptr<CellBuilder> acquire();

  /**
   * Return a CellBuilder to the pool for reuse.
   */
  static void release(std::unique_ptr<CellBuilder> builder);

  /**
   * Get pool statistics (for debugging/monitoring).
   */
  struct Stats {
    size_t allocations{0};
    size_t deallocations{0};
    size_t pool_hits{0};
    size_t pool_size{0};
  };

  static Stats get_stats();
  static void reset_stats();

private:
  struct ThreadLocalPool {
    std::vector<std::unique_ptr<CellBuilder>> free_list;
    Stats stats;
  };

  static ThreadLocalPool& get_thread_pool();
};

}  // namespace vm
