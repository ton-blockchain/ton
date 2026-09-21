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

#include <array>
#include <atomic>
#include <limits>
#include <type_traits>
#include <vector>

#include "td/utils/Hash.h"
#include "td/utils/check.h"
#include "td/utils/common.h"

#if !TD_HAVE_ABSL
#include "td/utils/Slice.h"
#endif

namespace td {

// Fixed-size concurrent cache of keys.
// Operations never wait for a busy slot: contains may miss and insert may skip.
template <size_t KeyWords, typename Word = uint64>
class SeqlockCache {
 public:
  static_assert(KeyWords > 0);
  static_assert(std::is_integral_v<Word> && std::has_unique_object_representations_v<Word>);
  using Key = std::array<Word, KeyWords>;

  explicit SeqlockCache(size_t slot_count) : slot_count_(slot_count), entries_(slot_count) {
    CHECK(slot_count > 0);
  }
  SeqlockCache(const SeqlockCache&) = delete;
  SeqlockCache& operator=(const SeqlockCache&) = delete;

  size_t size() const {
    return slot_count_;
  }

  bool contains(const Key& key, size_t slot) const {
    DCHECK(slot < slot_count_);
    const auto& entry = entries_[slot];
    auto sequence = entry.sequence.load(std::memory_order_acquire);
    if (sequence == 0 || (sequence & 1)) {
      return false;
    }
    for (size_t i = 0; i < KeyWords; ++i) {
      if (entry.key[i].load(std::memory_order_relaxed) != key[i]) {
        return false;
      }
    }
    // Paired with the writer's release fence through the atomic payload words.
    // If any new word was observed, the final sequence read must see that writer.
    std::atomic_thread_fence(std::memory_order_acquire);
    return entry.sequence.load(std::memory_order_relaxed) == sequence;
  }

  bool contains(const Key& key) const {
    return contains(key, key_to_slot(key));
  }

  bool insert(const Key& key, size_t slot) {
    DCHECK(slot < slot_count_);
    auto& entry = entries_[slot];
    auto sequence = entry.sequence.load(std::memory_order_relaxed);
    // Refuse a busy slot and prevent version wraparound (ABA).
    // Acquire orders this writer's payload stores after the previous writer's.
    if ((sequence & 1) || sequence == std::numeric_limits<uint64>::max() - 1 ||
        !entry.sequence.compare_exchange_strong(sequence, sequence + 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
      return false;
    }
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < KeyWords; ++i) {
      entry.key[i].store(key[i], std::memory_order_relaxed);
    }
    entry.sequence.store(sequence + 2, std::memory_order_release);
    return true;
  }

  bool insert(const Key& key) {
    return insert(key, key_to_slot(key));
  }

  size_t key_to_slot(const Key& key) const {
#if TD_HAVE_ABSL
    return Hash<Key>{}(key) % slot_count_;
#else
    // Remove when non-Abseil Hash implementation add support for std::array.
    return SliceHash{}(Slice{reinterpret_cast<const char*>(key.data()), key.size() * sizeof(Word)}) % slot_count_;
#endif
  }

 private:
  static_assert(std::atomic<uint64>::is_always_lock_free);
  static_assert(std::atomic<Word>::is_always_lock_free);
  struct Entry {
    std::atomic<uint64> sequence{0};
    std::array<std::atomic<Word>, KeyWords> key{};
  };
  const size_t slot_count_;
  std::vector<Entry> entries_;
};

}  // namespace td
