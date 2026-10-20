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
#include <array>
#include <barrier>
#include <thread>
#include <vector>

#include "td/utils/SeqlockCache.h"
#include "td/utils/tests.h"

namespace {

template <size_t Words, class Word = td::uint64>
void check_parameters(size_t slots) {
  using Cache = td::SeqlockCache<Words, Word>;
  using Key = typename Cache::Key;
  Cache cache(slots);
  const auto& reader = cache;
  ASSERT_EQ(reader.size(), slots);
  std::vector<Key> expected(slots);
  std::vector<bool> occupied(slots);
  Key key{};
  ASSERT_TRUE(!reader.contains(key));  // An empty slot must not match the zero key.
  for (Word value = 0; value < 128; ++value) {
    if (value != 0) {
      key[(value - 1) % Words] = value;
    }
    auto index = cache.key_to_slot(key);
    ASSERT_TRUE(index < slots);
    ASSERT_TRUE(!reader.contains(key));
    ASSERT_TRUE(cache.insert(key));
    if (occupied[index]) {
      ASSERT_TRUE(!reader.contains(expected[index]));  // A collision evicts the previous key.
    }
    expected[index] = key;
    occupied[index] = true;
    for (size_t slot = 0; slot < slots; ++slot) {
      if (occupied[slot]) {
        ASSERT_TRUE(reader.contains(expected[slot]));  // Other slots retain their keys.
      }
    }
  }
}

}  // namespace

TEST(SeqlockCache, Parameters) {
  check_parameters<1>(1);
  check_parameters<9>(3);
  check_parameters<29>(4);
  check_parameters<41>(2);
  check_parameters<9, td::uint8>(3);
  check_parameters<9, td::uint16>(3);
  check_parameters<9, td::uint32>(3);
  check_parameters<9, td::int32>(3);
  check_parameters<9>(16);
  check_parameters<9>(17);
}

TEST(SeqlockCache, ConcurrentReplacement) {
  td::SeqlockCache<2> cache(1);
  decltype(cache)::Key a{}, b{}, mixed{};
  a.fill(0x1111111111111111ULL);
  b.fill(0x2222222222222222ULL);
  mixed = a;
  for (size_t i = mixed.size() / 2; i < mixed.size(); ++i) {
    mixed[i] = b[i];
  }
  std::barrier start(6);
  std::vector<std::thread> workers;
  for (int writer = 0; writer < 4; ++writer) {
    workers.emplace_back([&, writer] {
      start.arrive_and_wait();
      for (int i = 0; i < 100000; ++i) {
        cache.insert((i + writer) % 2 ? a : b);
      }
    });
  }
  for (int reader = 0; reader < 2; ++reader) {
    workers.emplace_back([&] {
      start.arrive_and_wait();
      for (int i = 0; i < 200000; ++i) {
        // No writer inserts mixed. Contention may cause misses, never false hits.
        ASSERT_TRUE(!cache.contains(mixed));
        (void)cache.contains(a);
        (void)cache.contains(b);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_TRUE(cache.insert(a, 0));
  ASSERT_TRUE(cache.contains(a));
  ASSERT_TRUE(!cache.contains(b));
}
