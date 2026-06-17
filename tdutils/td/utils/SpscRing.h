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

#include <atomic>
#include <cstring>
#include <memory>

#include "td/utils/Slice.h"
#include "td/utils/common.h"

namespace td {

// Wait-free single-producer / single-consumer ring of variable-length records (a [u32 size][bytes] each).
//
// The buffer is allocated at 2x capacity and indexed by `pos & (capacity - 1)`. Since a record never exceeds
// capacity, there are always >= capacity contiguous bytes after any offset, so every record is written and
// read in one piece — no wrap split, no filler, no wrap branch on the hot path (the Quill trick). The
// monotonic 64-bit positions only wrap at the mask; fill = write - read.
//
// push() returns false and counts a drop when a record does not fit; it never blocks or allocates.
class SpscRing {
 public:
  static bool is_valid_capacity(uint32 capacity) {
    return capacity >= 2 * kHeader && capacity <= (1u << 30) && (capacity & (capacity - 1)) == 0;
  }

  explicit SpscRing(uint32 capacity)
      : capacity_(capacity), mask_(capacity - 1), buffer_(std::make_unique<char[]>(size_t{capacity} * 2)) {
    CHECK(is_valid_capacity(capacity));
  }
  SpscRing(const SpscRing &) = delete;
  SpscRing &operator=(const SpscRing &) = delete;

  // Producer: append `data` as one record. Returns false (counting a drop) if it does not fit.
  bool push(Slice data) {
    if (data.size() > capacity_ - kHeader) {
      return drop();
    }
    uint32 size = kHeader + static_cast<uint32>(data.size());
    uint64 write = write_.load(std::memory_order_relaxed);
    if (write - cached_read_ + size > capacity_) {  // recheck against the real read position before dropping
      cached_read_ = read_.load(std::memory_order_acquire);
      if (write - cached_read_ + size > capacity_) {
        return drop();
      }
    }
    char *slot = buffer_.get() + (static_cast<uint32>(write) & mask_);
    auto payload = static_cast<uint32>(data.size());
    std::memcpy(slot, &payload, kHeader);
    std::memcpy(slot + kHeader, data.data(), payload);
    write_.store(write + size, std::memory_order_release);  // publishes the record
    return true;
  }

  // Consumer: invoke fn(Slice) for every committed record, then free the consumed space.
  template <class F>
  void pop_each(F &&fn) {
    uint64 write = write_.load(std::memory_order_acquire);
    uint64 read = read_.load(std::memory_order_relaxed);
    for (uint64 pos = read; pos != write;) {
      char *slot = buffer_.get() + (static_cast<uint32>(pos) & mask_);
      uint32 payload;
      std::memcpy(&payload, slot, kHeader);
      fn(Slice(slot + kHeader, payload));
      pos += kHeader + payload;
    }
    read_.store(write, std::memory_order_release);
  }

  // Consumer: discard all pending records without reading them; returns the number of bytes dropped.
  uint64 clear() {
    uint64 write = write_.load(std::memory_order_acquire);
    uint64 read = read_.load(std::memory_order_relaxed);
    read_.store(write, std::memory_order_release);
    return write - read;
  }

  // Consumer: true once every committed record has been consumed.
  bool empty() const {
    return write_.load(std::memory_order_acquire) == read_.load(std::memory_order_relaxed);
  }

  uint64 dropped() const {
    return dropped_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr uint32 kHeader = sizeof(uint32);  // per-record [u32 payload size] prefix

  bool drop() {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  alignas(64) std::atomic<uint64> write_{0};  // producer publishes, consumer acquires
  alignas(64) std::atomic<uint64> read_{0};   // consumer publishes, producer acquires only when (maybe) full
  alignas(64) std::atomic<uint64> dropped_{0};
  alignas(64) uint64 cached_read_{0};  // producer-private cache of read_, to avoid touching it every push
  const uint32 capacity_;
  const uint32 mask_;
  std::unique_ptr<char[]> buffer_;

  static_assert(std::atomic<uint64>::is_always_lock_free, "SpscRing requires lock-free uint64 atomics");
};

}  // namespace td
