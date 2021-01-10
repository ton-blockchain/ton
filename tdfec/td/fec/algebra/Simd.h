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
#include "td/utils/misc.h"

#include "td/fec/algebra/Octet.h"

#if __SSSE3__
#define TD_SSE3 1
#endif

#if __AVX2__
#define TD_AVX2 1
#define TD_SSE3 1
#endif

#if TD_AVX2
#include <immintrin.h> /* avx2 */
#elif TD_SSE3
#include <tmmintrin.h> /* ssse3 */
#endif

namespace td {
class Simd_null {
 public:
  static constexpr size_t alignment() {
    return 32;  // gf256_from_gf2 relies on 32 alignment
  }

  static std::string get_name() {
    return "Without simd";
  }
  static bool is_aligned_pointer(const void *ptr) {
    return ::td::is_aligned_pointer<alignment()>(ptr);
  }

  static void gf256_add(void *a, const void *b, size_t size) {
    DCHECK(is_aligned_pointer(a));
    DCHECK(is_aligned_pointer(b));
    uint8 *ap = reinterpret_cast<uint8 *>(a);
    const uint8 *bp = reinterpret_cast<const uint8 *>(b);
    for (size_t i = 0; i < size; i++) {
      ap[i] ^= bp[i];
    }
  }
  static void gf256_mul(void *a, uint8 u, size_t size) {
    DCHECK(is_aligned_pointer(a));
    uint8 *ap = reinterpret_cast<uint8 *>(a);

    for (size_t i = 0; i < size; i++) {
      ap[i] = (Octet(ap[i]) * Octet(u)).value();
    }
  }
  static void gf256_add_mul(void *a, const void *b, uint8 u, size_t size) {
    DCHECK(is_aligned_pointer(a));
    DCHECK(is_aligned_pointer(b));
    uint8 *ap = reinterpret_cast<uint8 *>(a);
    const uint8 *bp = reinterpret_cast<const uint8 *>(b);

    for (size_t i = 0; i < size; i++) {
      ap[i] = (Octet(ap[i]) + Octet(bp[i]) * Octet(u)).value();
    }
  }

  static void gf256_from_gf2(void *a, const void *b, size_t size) {
    uint8 *ap = reinterpret_cast<uint8 *>(a);
    const uint8 *bp = reinterpret_cast<const uint8 *>(b);
    size *= 8;
    for (size_t i = 0; i < size; i++, ap++) {
      *ap = (bp[i / 8] >> (i % 8)) & 1;
    }
  }
};

#if TD_SSE3
class Simd_sse : public Simd_null {
 public:
  static constexpr size_t alignment() {
    return 32;
  }

  static std::string get_name() {
    return "With SSE";
  }

  static bool is_aligned_pointer(const void *ptr) {
    return ::td::is_aligned_pointer<alignment()>(ptr);
  }

  static void gf256_add(void *a, const void *b, size_t size) {
    DCHECK(is_aligned_pointer(a));
    DCHECK(is_aligned_pointer(b));
    uint8 *ap = reinterpret_cast<uint8 *>(a);
    const uint8 *bp = reinterpret_cast<const uint8 *>(b);
    __m128i *ap128 = reinterpret_cast<__m128i *>(ap);
    const __m128i *bp128 = reinterpret_cast<const __m128i *>(bp);
    for (size_t idx = 0; idx < size; idx += 16) {
      _mm_storeu_si128(ap128, _mm_xor_si128(_mm_loadu_si128(ap128), _mm_loadu_si128(bp128)));
      ap128++;
      bp128++;
    }
  }
  static void gf256_mul(void *a, uint8 u, size_t size) {
    DCHECK(is_aligned_pointer(a));
    uint8 *ap = reinterpret_cast<uint8 *>(a);

    const __m128i mask = _mm_set1_epi8(0x0f);
    const __m128i urow_hi = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Octet::OctMulHi[u]));
    const __m128i urow_lo = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Octet::OctMulLo[u]));

    __m128i *ap128 = reinterpret_cast<__m128i *>(ap);
    for (size_t idx = 0; idx < size; idx += 16) {
      __m128i ax = _mm_loadu_si128(ap128);
      __m128i lo = _mm_and_si128(ax, mask);
      ax = _mm_srli_epi64(ax, 4);
      __m128i hi = _mm_and_si128(ax, mask);
      lo = _mm_shuffle_epi8(urow_lo, lo);
      hi = _mm_shuffle_epi8(urow_hi, hi);

      _mm_storeu_si128(ap128, _mm_xor_si128(lo, hi));
      ap128++;
    }
  }
  static void gf256_add_mul(void *a, const void *b, uint8 u, size_t size) {
    DCHECK(is_aligned_pointer(a));
    DCHECK(is_aligned_pointer(b));
    uint8 *ap = reinterpret_cast<uint8 *>(a);
    const uint8 *bp = reinterpret_cast<const uint8 *>(b);

    const __m128i mask = _mm_set1_epi8(0x0f);
    const __m128i urow_hi = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Octet::OctMulHi[u]));
    const __m128i urow_lo = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Octet::OctMulLo[u]));

    __m128i *ap128 = reinterpret_cast<__m128i *>(ap);
    const __m128i *bp128 = reinterpret_cast<const __m128i *>(bp);
    for (size_t idx = 0; idx < size; idx += 16) {
      __m128i bx = _mm_loadu_si128(bp128++);
      __m128i lo = _mm_and_si128(bx, mask);
      bx = _mm_srli_epi64(bx, 4);
      __m128i hi = _mm_and_si128(bx, mask);
      lo = _mm_shuffle_epi8(urow_lo, lo);
      hi = _mm_shuffle_epi8(urow_hi, hi);

      _mm_storeu_si128(ap128, _mm_xor_si128(_mm_loadu_si128(ap128), _mm_xor_si128(lo, hi)));
      ap128++;
    }
  }
};
#endif  // SSSE3

#ifdef TD_AVX2
class Simd_avx : public Simd_sse {
 public:
  static std::string get_name() {
    return "With AVX";
  }

  static void gf256_add(void *a, const void *b, size_t size) {
    DCHECK(is_aligned_pointer(a));
    DCHECK(is_aligned_pointer(b));
    uint8 *ap = reinterpret_cast<uint8 *>(a);
    const uint8 *bp = reinterpret_cast<const uint8 *>(b);
    __m256i *ap256 = reinterpret_cast<__m256i *>(ap);
    const __m256i *bp256 = reinterpret_cast<const __m256i *>(bp);
    for (size_t idx = 0; idx < size; idx += 32) {
      _mm256_storeu_si256(ap256, _mm256_xor_si256(_mm256_loadu_si256(ap256), _mm256_loadu_si256(bp256)));
      ap256++;
      bp256++;
    }
  }

  static __m256i get_mask(const uint32 mask) {
    // abcd -> abcd * 8
    __m256i vmask(_mm256_set1_epi32(mask));

    // abcd * 8 -> aaaaaaaabbbbbbbbccccccccdddddddd
    const __m256i shuffle(
        _mm256_setr_epi64x(0x0000000000000000, 0x0101010101010101, 0x0202020202020202, 0x0303030303030303));
    vmask = _mm256_shuffle_epi8(vmask, shuffle);

    const __m256i bit_mask(_mm256_set1_epi64x(0x7fbfdfeff7fbfdfe));
    vmask = _mm256_or_si256(vmask, bit_mask);
    return _mm256_and_si256(_mm256_cmpeq_epi8(vmask, _mm256_set1_epi64x(-1)), _mm256_set1_epi8(1));
  }

  static void gf256_from_gf2(void *a, const void *b, size_t size) {
    DCHECK(is_aligned_pointer(a));
    DCHECK(size % 4 == 0);
    __m256i *ap256 = reinterpret_cast<__m256i *>(a);
    const uint32 *bp = reinterpret_cast<const uint32 *>(b);
    size /= 4;
    for (size_t i = 0; i < size; i++, bp++, ap256++) {
      *ap256 = get_mask(*bp);
    }
  }

  static __attribute__((noinline)) void gf256_mul(void *a, uint8 u, size_t size) {
    const __m128i urow_hi_small = _mm_load_si128(reinterpret_cast<const __m128i *>(Octet::OctMulHi[u]));
    const __m256i urow_hi = _mm256_broadcastsi128_si256(urow_hi_small);
    const __m128i urow_lo_small = _mm_load_si128(reinterpret_cast<const __m128i *>(Octet::OctMulLo[u]));
    const __m256i urow_lo = _mm256_broadcastsi128_si256(urow_lo_small);

    const __m256i mask = _mm256_set1_epi8(0x0f);
    __m256i *ap256 = (__m256i *)a;
    for (size_t idx = 0; idx < size; idx += 32) {
      __m256i ax = _mm256_load_si256(ap256);
      __m256i lo = _mm256_and_si256(ax, mask);
      ax = _mm256_srli_epi64(ax, 4);
      __m256i hi = _mm256_and_si256(ax, mask);
      lo = _mm256_shuffle_epi8(urow_lo, lo);
      hi = _mm256_shuffle_epi8(urow_hi, hi);

      _mm256_store_si256(ap256, _mm256_xor_si256(lo, hi));
      ap256++;
    }
  }

  static __attribute__((noinline)) void gf256_add_mul(void *a, const void *b, uint8 u, size_t size) {
    const __m128i urow_hi_small = _mm_load_si128(reinterpret_cast<const __m128i *>(Octet::OctMulHi[u]));
    const __m256i urow_hi = _mm256_broadcastsi128_si256(urow_hi_small);
    const __m128i urow_lo_small = _mm_load_si128(reinterpret_cast<const __m128i *>(Octet::OctMulLo[u]));
    const __m256i urow_lo = _mm256_broadcastsi128_si256(urow_lo_small);

    const __m256i mask = _mm256_set1_epi8(0x0f);
    __m256i *ap256 = (__m256i *)a;
    const __m256i *bp256 = (const __m256i *)b;
    for (size_t idx = 0; idx < size; idx += 32) {
      __m256i bx = _mm256_load_si256(bp256++);
      __m256i lo = _mm256_and_si256(bx, mask);
      bx = _mm256_srli_epi64(bx, 4);
      __m256i hi = _mm256_and_si256(bx, mask);
      lo = _mm256_shuffle_epi8(urow_lo, lo);
      hi = _mm256_shuffle_epi8(urow_hi, hi);

      _mm256_store_si256(ap256, _mm256_xor_si256(_mm256_load_si256(ap256), _mm256_xor_si256(lo, hi)));
      ap256++;
    }
  }
};
#endif  // AVX2

#if TD_AVX2
using Simd = Simd_avx;
#elif TD_SSE3
using Simd = Simd_sse;
#else
using Simd = Simd_null;
#endif

}  // namespace td
