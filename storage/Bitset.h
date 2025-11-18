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

    Copyright 2017-2020 Telegram Systems LLP
*/

#pragma once

#include "td/utils/Slice.h"
#include "td/utils/logging.h"

namespace td {
struct Bitset {
 public:
  td::Slice as_slice() const {
    return td::Slice(bits_).substr(0, (bits_size_ + 7) / 8);
  }
  bool get(size_t offset) const {
    auto i = offset / 8;
    if (i >= bits_.size()) {
      return false;
    }
    auto bit_i = offset % 8;
    return (bits_[i] & (1 << bit_i)) != 0;
  }

  void reserve(size_t offset) {
    auto i = offset / 8;
    if (i >= bits_.size()) {
      bits_.resize(i + 1);
    }
  }

  bool set_one(size_t offset) {
    auto i = offset / 8;
    auto bit_i = offset % 8;
    bits_size_ = std::max(bits_size_, offset + 1);
    if (i >= bits_.size()) {
      bits_.resize(std::max(i + 1, bits_.size() * 2));
    }
    auto mask = 1 << bit_i;
    if ((bits_[i] & mask) == 0) {
      bits_[i] |= (char)mask;
      count_++;
      return true;
    }
    return false;
  }

  bool set_zero(size_t offset) {
    auto i = offset / 8;
    if (i >= bits_.size()) {
      return false;
    }
    auto bit_i = offset % 8;
    auto mask = 1 << bit_i;
    if (bits_[i] & mask) {
      bits_[i] &= (char)~mask;
      count_--;
      return true;
    }
    return false;
  }

  size_t ones_count() const {
    return count_;
  }

  void set_raw(std::string bits) {
    bits_ = std::move(bits);
    bits_size_ = 0;
    count_ = 0;

    // Fast path: Use __builtin_popcountll for efficient bit counting
    // Process 8 bytes (64 bits) at a time using hardware popcount instruction
    const size_t num_full_words = bits_.size() / 8;
    const uint64_t* words = reinterpret_cast<const uint64_t*>(bits_.data());

    for (size_t i = 0; i < num_full_words; i++) {
      uint64_t word = words[i];
      if (word != 0) {
        count_ += __builtin_popcountll(word);
        // Update bits_size_ to the last set bit in this word
        bits_size_ = i * 64 + 64 - __builtin_clzll(word);
      }
    }

    // Handle remaining bytes (< 8 bytes)
    for (size_t i = num_full_words * 8; i < bits_.size(); i++) {
      unsigned char byte = static_cast<unsigned char>(bits_[i]);
      if (byte != 0) {
        count_ += __builtin_popcount(byte);
        // Find the highest set bit in this byte
        bits_size_ = i * 8 + 8 - __builtin_clz(byte << 24);
      }
    }
  }

  size_t size() const {
    return bits_.size() * 8;
  }

 private:
  std::string bits_;
  size_t bits_size_{0};
  size_t count_{0};
};
}  // namespace td
