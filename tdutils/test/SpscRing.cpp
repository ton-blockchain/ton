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

#include <string>
#include <thread>
#include <vector>

#include "td/utils/Slice.h"
#include "td/utils/SpscRing.h"
#include "td/utils/port/thread.h"
#include "td/utils/tests.h"

namespace {
std::vector<std::string> pop_records(td::SpscRing &ring) {
  std::vector<std::string> result;
  ring.pop_each([&](td::Slice value) { result.push_back(value.str()); });
  return result;
}
constexpr td::uint32 kHeader = sizeof(td::uint32);  // per-record overhead, see SpscRing
}  // namespace

TEST(SpscRing, Records) {
  td::SpscRing ring(32);
  ASSERT_TRUE(ring.push(td::Slice("aaa")));
  ASSERT_TRUE(ring.push(td::Slice("bbbb")));

  auto values = pop_records(ring);
  ASSERT_EQ(static_cast<size_t>(2), values.size());
  ASSERT_EQ(std::string("aaa"), values[0]);
  ASSERT_EQ(std::string("bbbb"), values[1]);
  ASSERT_EQ(static_cast<td::uint64>(0), ring.dropped());

  // A record whose offset wraps the buffer end is still read in one piece (the 2x-buffer guarantee).
  ASSERT_TRUE(ring.push(td::Slice("abcdefghijklmnop")));
  values = pop_records(ring);
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  ASSERT_EQ(std::string("abcdefghijklmnop"), values[0]);
}

TEST(SpscRing, Clear) {
  td::SpscRing ring(64);
  ASSERT_TRUE(ring.push(td::Slice("aaa")));
  ASSERT_TRUE(ring.push(td::Slice("bbbb")));
  ASSERT_EQ(static_cast<td::uint64>(2 * kHeader + 3 + 4), ring.clear());
  ASSERT_EQ(static_cast<td::uint64>(0), ring.clear());
  ASSERT_TRUE(pop_records(ring).empty());
}

TEST(SpscRing, Drops) {
  td::SpscRing ring(32);
  std::string full(32 - kHeader, 'a');  // a record that exactly fills the ring
  ASSERT_TRUE(ring.push(full));
  ASSERT_TRUE(!ring.push(td::Slice("x")));  // no room left
  ASSERT_EQ(static_cast<td::uint64>(1), ring.dropped());

  auto values = pop_records(ring);
  ASSERT_EQ(static_cast<size_t>(1), values.size());
  ASSERT_EQ(full, values[0]);

  ASSERT_TRUE(ring.push(td::Slice("x")));  // space reclaimed
  ASSERT_EQ(std::vector<std::string>{"x"}, pop_records(ring));

  td::SpscRing small(16);
  ASSERT_TRUE(!small.push(std::string(16 - kHeader + 1, 'z')));  // record larger than the ring
  ASSERT_EQ(static_cast<td::uint64>(1), small.dropped());
  ASSERT_TRUE(small.push(std::string(16 - kHeader, 'z')));  // exactly fits

  td::SpscRing tiny(8);
  ASSERT_TRUE(tiny.push(td::Slice("")));  // empty payload is a valid record
  ASSERT_EQ(static_cast<size_t>(1), pop_records(tiny).size());
}

TEST(SpscRing, WrapStress) {
  td::SpscRing ring(64);
  td::uint64 pushed = 0;
  td::uint64 popped = 0;
  for (int round = 0; round < 200000; round++) {
    if (ring.push(PSTRING() << "r" << pushed)) {  // varying length -> records straddle the wrap over time
      pushed++;
    }
    ring.pop_each([&](td::Slice value) {  // every record comes back intact and in FIFO order
      ASSERT_EQ(PSTRING() << "r" << popped, value.str());
      popped++;
    });
  }
  ASSERT_EQ(pushed, popped);
  ASSERT_EQ(static_cast<td::uint64>(0), ring.dropped());
}

TEST(SpscRing, Concurrent) {
  td::SpscRing ring(1024);  // small on purpose: the producer keeps filling it, exercising the full/wrap paths
  constexpr td::uint64 kRecords = 1000000;

  td::thread producer([&] {
    for (td::uint64 i = 0; i < kRecords; i++) {
      auto rec = PSTRING() << "r" << i;  // varying length -> records straddle the wrap over time
      while (!ring.push(rec)) {          // retry on a full ring so no record is lost (drops counts the retries)
        std::this_thread::yield();
      }
    }
  });

  td::uint64 popped = 0;  // consumer runs on the test thread, so assertions stay here
  while (popped < kRecords) {
    ring.pop_each([&](td::Slice value) {
      ASSERT_EQ(PSTRING() << "r" << popped, value.str());  // every record arrives intact and in FIFO order
      popped++;
    });
  }
  producer.join();

  ASSERT_EQ(kRecords, popped);  // exact FIFO, no record lost across the wraps
}
