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
#include "PacketPool.h"
#include <algorithm>

namespace ton {
namespace rldp2 {

BufferSlicePool::ThreadLocalPool& BufferSlicePool::get_thread_pool() {
  static thread_local ThreadLocalPool pool;
  static thread_local bool initialized = false;
  if (!initialized) {
    pool.cached_buffers.reserve(kMaxCachedBuffers);
    initialized = true;
  }
  return pool;
}

td::BufferSlice BufferSlicePool::acquire(size_t size) {
  auto& pool = get_thread_pool();
  pool.stats.total_allocations++;

  // Don't pool very small or very large buffers
  if (size < kMinBufferSize || size > kMaxBufferSize) {
    return td::BufferSlice(size);
  }

  // Find a cached buffer of similar size (within 20% tolerance)
  auto it = std::find_if(pool.cached_buffers.begin(), pool.cached_buffers.end(),
                         [size](const BufferEntry& entry) {
                           size_t diff = entry.size > size ? entry.size - size : size - entry.size;
                           return diff * 5 <= size;  // Within 20%
                         });

  if (it != pool.cached_buffers.end()) {
    auto buffer = std::move(it->buffer);
    pool.cached_buffers.erase(it);
    pool.stats.pool_hits++;
    pool.stats.cached_buffers = pool.cached_buffers.size();

    // Resize if needed
    if (buffer.size() != size) {
      buffer.truncate(size);
      if (buffer.size() < size) {
        // Need to reallocate if too small
        return td::BufferSlice(size);
      }
    }

    return buffer;
  }

  pool.stats.cached_buffers = pool.cached_buffers.size();
  return td::BufferSlice(size);
}

void BufferSlicePool::release(td::BufferSlice buffer) {
  if (buffer.empty()) {
    return;
  }

  auto& pool = get_thread_pool();

  size_t size = buffer.size();
  if (size < kMinBufferSize || size > kMaxBufferSize) {
    return;  // Don't pool
  }

  if (pool.cached_buffers.size() < kMaxCachedBuffers) {
    pool.cached_buffers.push_back(BufferEntry{size, std::move(buffer)});
    pool.stats.cached_buffers = pool.cached_buffers.size();
  }
}

BufferSlicePool::Stats BufferSlicePool::get_stats() {
  auto& pool = get_thread_pool();
  return pool.stats;
}

void BufferSlicePool::reset_stats() {
  auto& pool = get_thread_pool();
  pool.stats = Stats{};
  pool.stats.cached_buffers = pool.cached_buffers.size();
}

}  // namespace rldp2
}  // namespace ton
