/*
    This file is part of TON Blockchain Library.

    Memory pool performance test and validation.
*/

#include "vm/cells/CellBuilderPool.h"
#include "vm/cells/PoolMonitor.h"
#include "rldp2/PacketPool.h"
#include "rldp2/PoolMonitor.h"

#include <iostream>
#include <chrono>
#include <vector>

using namespace std::chrono;

void test_cellbuilder_pool() {
  std::cout << "\n=== Testing CellBuilder Pool ===\n";

  vm::PoolMonitor::reset_all_statistics();

  // Warm-up: Fill the pool
  {
    std::vector<std::unique_ptr<vm::CellBuilder>> builders;
    for (int i = 0; i < 50; i++) {
      builders.push_back(vm::CellBuilderPool::acquire());
    }
    // All released when vector goes out of scope
  }

  // Benchmark with pool
  auto start = high_resolution_clock::now();
  for (int i = 0; i < 10000; i++) {
    auto builder = vm::CellBuilderPool::acquire();
    // Simulate some work
    builder->store_long(i, 32);
  }
  auto end = high_resolution_clock::now();
  auto duration_pool = duration_cast<microseconds>(end - start).count();

  std::cout << "Pool-based allocation: " << duration_pool << " μs\n";
  std::cout << vm::PoolMonitor::get_statistics_report();

  // Benchmark without pool (for comparison)
  auto start2 = high_resolution_clock::now();
  for (int i = 0; i < 10000; i++) {
    auto builder = std::make_unique<vm::CellBuilder>();
    builder->store_long(i, 32);
  }
  auto end2 = high_resolution_clock::now();
  auto duration_direct = duration_cast<microseconds>(end2 - start2).count();

  std::cout << "\nDirect allocation: " << duration_direct << " μs\n";

  double speedup = (double)duration_direct / duration_pool;
  std::cout << "Speedup: " << speedup << "x\n";
}

void test_buffer_pool() {
  std::cout << "\n=== Testing BufferSlice Pool ===\n";

  ton::rldp2::PoolMonitor::reset_all_statistics();

  // Warm-up: Fill the pool with various sizes
  {
    std::vector<td::BufferSlice> buffers;
    for (int i = 0; i < 50; i++) {
      buffers.push_back(ton::rldp2::BufferSlicePool::acquire(4096));
      buffers.push_back(ton::rldp2::BufferSlicePool::acquire(8192));
    }
    for (auto& buf : buffers) {
      ton::rldp2::BufferSlicePool::release(std::move(buf));
    }
  }

  // Benchmark with pool
  auto start = high_resolution_clock::now();
  for (int i = 0; i < 5000; i++) {
    auto buffer = ton::rldp2::BufferSlicePool::acquire(4096);
    // Simulate some work
    std::memset(buffer.data(), i & 0xFF, 100);
    ton::rldp2::BufferSlicePool::release(std::move(buffer));
  }
  auto end = high_resolution_clock::now();
  auto duration_pool = duration_cast<microseconds>(end - start).count();

  std::cout << "Pool-based allocation: " << duration_pool << " μs\n";
  std::cout << ton::rldp2::PoolMonitor::get_statistics_report();

  // Benchmark without pool
  auto start2 = high_resolution_clock::now();
  for (int i = 0; i < 5000; i++) {
    auto buffer = td::BufferSlice(4096);
    std::memset(buffer.data(), i & 0xFF, 100);
  }
  auto end2 = high_resolution_clock::now();
  auto duration_direct = duration_cast<microseconds>(end2 - start2).count();

  std::cout << "\nDirect allocation: " << duration_direct << " μs\n";

  double speedup = (double)duration_direct / duration_pool;
  std::cout << "Speedup: " << speedup << "x\n";
}

void test_concurrent_usage() {
  std::cout << "\n=== Testing Concurrent Pool Usage ===\n";
  std::cout << "(Pools are thread-local, no locking overhead)\n";

  // Simulate mixed allocation pattern
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 100; i++) {
      auto builder = vm::CellBuilderPool::acquire();
      auto buffer = ton::rldp2::BufferSlicePool::acquire(1024 + (i % 10) * 512);

      // Simulate work
      builder->store_long(i, 32);
      std::memset(buffer.data(), 0, buffer.size());

      // Early release of some buffers
      if (i % 3 == 0) {
        ton::rldp2::BufferSlicePool::release(std::move(buffer));
      }
    }

    std::cout << "\nRound " << (round + 1) << ":\n";
    std::cout << "  " << vm::PoolMonitor::get_compact_stats() << "\n";
    std::cout << "  " << ton::rldp2::PoolMonitor::get_compact_stats() << "\n";
  }
}

int main() {
  std::cout << "TON Memory Pool Performance Test\n";
  std::cout << "=================================\n";

  test_cellbuilder_pool();
  test_buffer_pool();
  test_concurrent_usage();

  std::cout << "\n=== Final Statistics ===\n";
  std::cout << vm::PoolMonitor::get_compact_stats() << "\n";
  std::cout << ton::rldp2::PoolMonitor::get_compact_stats() << "\n";

  std::cout << "\nTest completed successfully!\n";
  return 0;
}
