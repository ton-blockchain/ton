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

#include "storage/Bitset.h"
#include "td/utils/tests.h"
#include "td/utils/Time.h"
#include <random>

// Test Phase 5.1: Bitset optimization with __builtin_popcount

TEST(BitsetOptimization, SetRawPerformance) {
  // Test the optimized set_raw() method with __builtin_popcountll
  std::string bits;
  bits.resize(1024);  // 1KB = 8192 bits

  // Fill with random data
  std::mt19937 rng(42);
  for (size_t i = 0; i < bits.size(); i++) {
    bits[i] = static_cast<char>(rng() % 256);
  }

  td::Bitset bitset;
  auto start = td::Timestamp::now();
  bitset.set_raw(std::string(bits));
  auto elapsed = td::Timestamp::now().at() - start.at();

  // Verify correctness
  size_t expected_count = 0;
  for (size_t i = 0; i < 8192; i++) {
    if (bitset.get(i)) {
      expected_count++;
    }
  }

  ASSERT_EQ(bitset.ones_count(), expected_count);

  // Performance check: should complete in < 10ms for 1KB
  LOG(INFO) << "Bitset set_raw() for 1KB: " << (elapsed * 1000.0) << "ms, ones_count=" << bitset.ones_count();
  ASSERT_TRUE(elapsed < 0.01);  // < 10ms
}

TEST(BitsetOptimization, SetRawCorrectness) {
  // Test correctness of the optimized implementation
  std::string bits;

  // Test case 1: All zeros
  bits.resize(8, '\0');
  td::Bitset bitset1;
  bitset1.set_raw(std::string(bits));
  ASSERT_EQ(bitset1.ones_count(), 0u);

  // Test case 2: All ones
  bits.assign(8, '\xFF');
  td::Bitset bitset2;
  bitset2.set_raw(std::string(bits));
  ASSERT_EQ(bitset2.ones_count(), 64u);

  // Test case 3: Mixed pattern
  bits.clear();
  bits.push_back('\x01');  // 00000001
  bits.push_back('\x03');  // 00000011
  bits.push_back('\x07');  // 00000111
  bits.push_back('\x0F');  // 00001111
  bits.push_back('\xFF');  // 11111111
  bits.push_back('\x00');  // 00000000
  bits.push_back('\xAA');  // 10101010
  bits.push_back('\x55');  // 01010101

  td::Bitset bitset3;
  bitset3.set_raw(std::string(bits));
  // Expected: 1 + 2 + 3 + 4 + 8 + 0 + 4 + 4 = 26
  ASSERT_EQ(bitset3.ones_count(), 26u);
}

TEST(BitsetOptimization, SetRawEdgeCases) {
  td::Bitset bitset;

  // Empty bitset
  bitset.set_raw(std::string());
  ASSERT_EQ(bitset.ones_count(), 0u);

  // Single byte
  bitset.set_raw(std::string(1, '\x0F'));
  ASSERT_EQ(bitset.ones_count(), 4u);

  // Non-aligned size (not multiple of 8)
  std::string bits;
  bits.resize(15, '\xFF');  // 15 bytes = 120 bits
  bitset.set_raw(std::string(bits));
  ASSERT_EQ(bitset.ones_count(), 120u);

  // Large bitset (16KB)
  bits.resize(16384, '\xAA');  // 10101010 pattern
  bitset.set_raw(std::string(bits));
  ASSERT_EQ(bitset.ones_count(), 16384u * 4);  // 4 ones per byte
}

TEST(BitsetOptimization, SetRawBenchmark) {
  // Benchmark for different sizes
  std::vector<size_t> sizes = {128, 1024, 4096, 16384, 65536};  // bytes

  std::mt19937 rng(42);
  for (size_t size : sizes) {
    std::string bits;
    bits.resize(size);
    for (size_t i = 0; i < size; i++) {
      bits[i] = static_cast<char>(rng() % 256);
    }

    td::Bitset bitset;
    auto start = td::Timestamp::now();

    // Run multiple iterations for small sizes
    int iterations = std::max(1, static_cast<int>(1024 / size));
    for (int i = 0; i < iterations; i++) {
      bitset.set_raw(std::string(bits));
    }

    auto elapsed = (td::Timestamp::now().at() - start.at()) / iterations;
    double throughput_mbps = (size * 8.0) / (elapsed * 1000000.0);

    LOG(INFO) << "Bitset set_raw() for " << size << " bytes: "
              << (elapsed * 1000.0) << "ms, throughput=" << throughput_mbps << " Mbit/s";

    // Performance target: should handle at least 100 Mbit/s
    ASSERT_TRUE(throughput_mbps > 100.0);
  }
}

TEST(BitsetOptimization, SetRawConsistency) {
  // Verify that optimized implementation gives same results as naive approach
  std::mt19937 rng(12345);

  for (int test = 0; test < 100; test++) {
    size_t size = 1 + (rng() % 1000);
    std::string bits;
    bits.resize(size);
    for (size_t i = 0; i < size; i++) {
      bits[i] = static_cast<char>(rng() % 256);
    }

    td::Bitset bitset;
    bitset.set_raw(std::string(bits));

    // Verify by manually counting
    size_t expected = 0;
    for (size_t i = 0; i < size * 8; i++) {
      if (bitset.get(i)) {
        expected++;
      }
    }

    ASSERT_EQ(bitset.ones_count(), expected) << "Mismatch at test " << test << ", size " << size;
  }
}
