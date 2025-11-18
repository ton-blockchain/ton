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
#include "CellBuilderPool.h"

namespace vm {

CellBuilderPool::ThreadLocalPool& CellBuilderPool::get_thread_pool() {
  static thread_local ThreadLocalPool pool;
  static thread_local bool initialized = false;
  if (!initialized) {
    pool.free_list.reserve(kMaxFreeList);
    initialized = true;
  }
  return pool;
}

std::unique_ptr<CellBuilder> CellBuilderPool::acquire() {
  auto& pool = get_thread_pool();
  pool.stats.allocations++;

  // Try to get from free list
  if (!pool.free_list.empty()) {
    auto builder = std::move(pool.free_list.back());
    pool.free_list.pop_back();
    pool.stats.pool_hits++;
    pool.stats.pool_size = pool.free_list.size();

    // Reset the builder to clean state
    // CellBuilder doesn't need explicit reset, construction handles it
    return builder;
  }

  // Allocate new if pool is empty
  pool.stats.pool_size = 0;
  return std::make_unique<CellBuilder>();
}

void CellBuilderPool::release(std::unique_ptr<CellBuilder> builder) {
  if (!builder) {
    return;
  }

  auto& pool = get_thread_pool();
  pool.stats.deallocations++;

  // Return to pool if not full
  if (pool.free_list.size() < kMaxFreeList) {
    pool.free_list.push_back(std::move(builder));
    pool.stats.pool_size = pool.free_list.size();
  }
  // Otherwise, let it be destroyed (implicit via unique_ptr)
}

CellBuilderPool::Stats CellBuilderPool::get_stats() {
  auto& pool = get_thread_pool();
  return pool.stats;
}

void CellBuilderPool::reset_stats() {
  auto& pool = get_thread_pool();
  pool.stats = Stats{};
  pool.stats.pool_size = pool.free_list.size();
}

}  // namespace vm
