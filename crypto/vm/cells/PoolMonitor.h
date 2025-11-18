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

#include "CellBuilderPool.h"
#include <string>
#include <sstream>

namespace vm {

/**
 * Utility class for monitoring and reporting memory pool statistics.
 * Useful for performance analysis and pool tuning.
 */
class PoolMonitor {
public:
  /**
   * Get a formatted string with current pool statistics.
   */
  static std::string get_statistics_report() {
    std::ostringstream oss;

    auto cell_stats = CellBuilderPool::get_stats();

    oss << "=== Memory Pool Statistics ===\n";
    oss << "CellBuilder Pool:\n";
    oss << "  Allocations:   " << cell_stats.allocations << "\n";
    oss << "  Deallocations: " << cell_stats.deallocations << "\n";
    oss << "  Pool hits:     " << cell_stats.pool_hits << "\n";
    oss << "  Pool size:     " << cell_stats.pool_size << "\n";

    if (cell_stats.allocations > 0) {
      double hit_rate = 100.0 * cell_stats.pool_hits / cell_stats.allocations;
      oss << "  Hit rate:      " << hit_rate << "%\n";

      double reuse_rate = (cell_stats.allocations > 0) ?
          100.0 * (cell_stats.allocations - cell_stats.allocations + cell_stats.pool_hits) / cell_stats.allocations : 0;
      oss << "  Reuse rate:    " << reuse_rate << "%\n";
    }

    oss << "==============================\n";

    return oss.str();
  }

  /**
   * Get a compact one-line statistics summary.
   */
  static std::string get_compact_stats() {
    auto cell_stats = CellBuilderPool::get_stats();
    std::ostringstream oss;

    oss << "CellBuilder[";
    if (cell_stats.allocations > 0) {
      double hit_rate = 100.0 * cell_stats.pool_hits / cell_stats.allocations;
      oss << "hits:" << cell_stats.pool_hits << "/" << cell_stats.allocations
          << "(" << static_cast<int>(hit_rate) << "%) ";
    }
    oss << "pool:" << cell_stats.pool_size << "]";

    return oss.str();
  }

  /**
   * Reset all pool statistics (useful for benchmarking specific operations).
   */
  static void reset_all_statistics() {
    CellBuilderPool::reset_stats();
  }
};

}  // namespace vm
