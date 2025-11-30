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

#include "PacketPool.h"
#include <string>
#include <sstream>

namespace ton {
namespace rldp2 {

/**
 * Utility class for monitoring and reporting RLDP2 memory pool statistics.
 */
class PoolMonitor {
public:
  /**
   * Get a formatted string with current pool statistics.
   */
  static std::string get_statistics_report() {
    std::ostringstream oss;

    auto buffer_stats = BufferSlicePool::get_stats();

    oss << "=== RLDP2 Pool Statistics ===\n";
    oss << "BufferSlice Pool:\n";
    oss << "  Total allocations: " << buffer_stats.total_allocations << "\n";
    oss << "  Pool hits:         " << buffer_stats.pool_hits << "\n";
    oss << "  Cached buffers:    " << buffer_stats.cached_buffers << "\n";

    if (buffer_stats.total_allocations > 0) {
      double hit_rate = 100.0 * buffer_stats.pool_hits / buffer_stats.total_allocations;
      oss << "  Hit rate:          " << hit_rate << "%\n";

      // Estimate memory saved (assuming average buffer size ~4KB)
      size_t avg_buffer_size = 4096;
      size_t allocations_saved = buffer_stats.pool_hits;
      size_t bytes_saved = allocations_saved * avg_buffer_size;
      oss << "  Est. allocs saved: " << allocations_saved << " (~"
          << (bytes_saved / 1024) << " KB reused)\n";
    }

    oss << "============================\n";

    return oss.str();
  }

  /**
   * Get a compact one-line statistics summary.
   */
  static std::string get_compact_stats() {
    auto buffer_stats = BufferSlicePool::get_stats();
    std::ostringstream oss;

    oss << "BufferPool[";
    if (buffer_stats.total_allocations > 0) {
      double hit_rate = 100.0 * buffer_stats.pool_hits / buffer_stats.total_allocations;
      oss << "hits:" << buffer_stats.pool_hits << "/" << buffer_stats.total_allocations
          << "(" << static_cast<int>(hit_rate) << "%) ";
    }
    oss << "cached:" << buffer_stats.cached_buffers << "]";

    return oss.str();
  }

  /**
   * Reset all pool statistics.
   */
  static void reset_all_statistics() {
    BufferSlicePool::reset_stats();
  }
};

}  // namespace rldp2
}  // namespace ton
