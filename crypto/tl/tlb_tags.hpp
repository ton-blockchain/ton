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

#include <cstdint>
#include <array>

namespace tlb {

// Constexpr lookup table for N-bit tag patterns
// Enables O(1) tag resolution with compile-time table generation
template <unsigned N>
struct TagLookup {
  static_assert(N > 0 && N <= 8, "Tag bits must be 1-8");
  static constexpr unsigned TABLE_SIZE = 1u << N;

  std::array<int8_t, TABLE_SIZE> table{};

  constexpr TagLookup() = default;

  // Set tag value for a specific bit pattern
  constexpr void set(unsigned pattern, int8_t tag) {
    table[pattern & (TABLE_SIZE - 1)] = tag;
  }

  // Lookup tag from prefetched bits
  constexpr int lookup(unsigned long long bits) const {
    return table[bits & (TABLE_SIZE - 1)];
  }

  // Lookup with validation (-1 for invalid)
  constexpr int lookup_validated(unsigned long long bits) const {
    int8_t tag = table[bits & (TABLE_SIZE - 1)];
    return tag;
  }
};

// Factory for creating common tag lookup tables

// 1-bit tag lookup (Bool, Maybe, Either patterns)
inline constexpr auto make_binary_tag_lookup() {
  TagLookup<1> t;
  t.set(0, 0);  // bit 0 -> tag 0
  t.set(1, 1);  // bit 1 -> tag 1
  return t;
}

// Pre-built common tag tables
inline constexpr auto BINARY_TAGS = make_binary_tag_lookup();

// 2-bit tag lookup for 4-variant types
inline constexpr auto make_quad_tag_lookup() {
  TagLookup<2> t;
  t.set(0b00, 0);
  t.set(0b01, 1);
  t.set(0b10, 2);
  t.set(0b11, 3);
  return t;
}

inline constexpr auto QUAD_TAGS = make_quad_tag_lookup();

// 3-bit tag lookup for 8-variant types
inline constexpr auto make_octal_tag_lookup() {
  TagLookup<3> t;
  for (unsigned i = 0; i < 8; ++i) {
    t.set(i, static_cast<int8_t>(i));
  }
  return t;
}

inline constexpr auto OCTAL_TAGS = make_octal_tag_lookup();

// 4-bit tag lookup for 16-variant types
inline constexpr auto make_hex_tag_lookup() {
  TagLookup<4> t;
  for (unsigned i = 0; i < 16; ++i) {
    t.set(i, static_cast<int8_t>(i));
  }
  return t;
}

inline constexpr auto HEX_TAGS = make_hex_tag_lookup();

// Helper for creating custom tag patterns with prefix matching
// Returns -1 for patterns that don't match any defined tag
template <unsigned N>
constexpr TagLookup<N> make_prefix_tag_lookup(
    std::initializer_list<std::pair<unsigned, int8_t>> patterns,
    int8_t default_tag = -1) {
  TagLookup<N> t;
  // Initialize all entries to default
  for (unsigned i = 0; i < TagLookup<N>::TABLE_SIZE; ++i) {
    t.set(i, default_tag);
  }
  // Set specific patterns
  for (const auto& p : patterns) {
    t.set(p.first, p.second);
  }
  return t;
}

// Utility to create variable-length prefix lookup
// For patterns like: 0 -> tag0, 10 -> tag1, 11 -> tag2
template <unsigned MaxBits>
struct PrefixTagLookup {
  static_assert(MaxBits > 0 && MaxBits <= 8, "Max bits must be 1-8");

  TagLookup<MaxBits> table;
  std::array<uint8_t, 1u << MaxBits> bit_lengths{};

  constexpr PrefixTagLookup() = default;

  // Set a prefix pattern (pattern, bits used, tag value)
  constexpr void set_prefix(unsigned pattern, unsigned bits, int8_t tag) {
    unsigned mask = (1u << bits) - 1;
    unsigned base = pattern & mask;
    // Fill all table entries that match this prefix
    unsigned fill_count = 1u << (MaxBits - bits);
    for (unsigned i = 0; i < fill_count; ++i) {
      unsigned idx = base | (i << bits);
      table.set(idx, tag);
      bit_lengths[idx] = static_cast<uint8_t>(bits);
    }
  }

  // Lookup returns both tag and number of bits consumed
  constexpr std::pair<int, unsigned> lookup(unsigned long long bits) const {
    unsigned idx = bits & ((1u << MaxBits) - 1);
    return {table.lookup(bits), bit_lengths[idx]};
  }
};

}  // namespace tlb
