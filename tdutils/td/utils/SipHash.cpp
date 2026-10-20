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
#include "td/utils/Random.h"
#include "td/utils/SipHash.h"
#include "td/utils/as.h"

namespace td {
namespace {

uint64 rotl(uint64 x, int bits) {
  return (x << bits) | (x >> (64 - bits));
}

struct SipState {
  uint64 v0, v1, v2, v3;

  void round() {
    v0 += v1;
    v1 = rotl(v1, 13);
    v1 ^= v0;
    v0 = rotl(v0, 32);
    v2 += v3;
    v3 = rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotl(v1, 17);
    v1 ^= v2;
    v2 = rotl(v2, 32);
  }

  void absorb(uint64 m) {
    v3 ^= m;
    round();
    v0 ^= m;
  }
};

}  // namespace

uint64 sip_hash13(Slice data, uint64 k0, uint64 k1) {
  SipState s{0x736f6d6570736575ULL ^ k0, 0x646f72616e646f6dULL ^ k1, 0x6c7967656e657261ULL ^ k0,
             0x7465646279746573ULL ^ k1};

  const unsigned char *p = data.ubegin();
  size_t tail_size = data.size() & 7;
  const unsigned char *tail = p + data.size() - tail_size;
  // tdutils already assumes a little-endian host (see bits.h), so a block is read as it lies.
  for (; p != tail; p += 8) {
    s.absorb(as<uint64>(p));
  }

  uint64 last = static_cast<uint64>(data.size()) << 56;
  for (size_t i = 0; i < tail_size; i++) {
    last |= static_cast<uint64>(tail[i]) << (8 * i);
  }
  s.absorb(last);

  s.v2 ^= 0xff;
  s.round();
  s.round();
  s.round();
  return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

uint64 sip_hash13(Slice data) {
  static const uint64 k0 = Random::secure_uint64();
  static const uint64 k1 = Random::secure_uint64();
  return sip_hash13(data, k0, k1);
}

}  // namespace td
