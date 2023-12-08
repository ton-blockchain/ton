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

#include <vector>
#include "td/utils/buffer.h"
#include "common/bitstring.h"
#include "common/refint.h"

namespace vm {
namespace bls {

const size_t P1_SIZE = 48;
const size_t P2_SIZE = 96;
const size_t FP_SIZE = 48;

using P1 = td::BitArray<P1_SIZE * 8>;
using P2 = td::BitArray<P2_SIZE * 8>;
using FP = td::BitArray<FP_SIZE * 8>;
using FP2 = td::BitArray<FP_SIZE * 2 * 8>;

bool verify(const P1 &pub, td::Slice msg, const P2 &sig);
P2 aggregate(const std::vector<P2> &sig);
bool fast_aggregate_verify(const std::vector<P1> &pubs, td::Slice msg, const P2 &sig);
bool aggregate_verify(const std::vector<std::pair<P1, td::BufferSlice>> &pubs_msgs, const P2 &sig);

P1 g1_add(const P1 &a, const P1 &b);
P1 g1_sub(const P1 &a, const P1 &b);
P1 g1_neg(const P1 &a);
P1 g1_mul(const P1 &p, const td::RefInt256 &x);
P1 g1_multiexp(const std::vector<std::pair<P1, td::RefInt256>> &ps);
P1 g1_zero();
P1 map_to_g1(const FP &a);
bool g1_in_group(const P1 &a);
bool g1_is_zero(const P1 &a);

P2 g2_add(const P2 &a, const P2 &b);
P2 g2_sub(const P2 &a, const P2 &b);
P2 g2_neg(const P2 &a);
P2 g2_mul(const P2 &p, const td::RefInt256 &x);
P2 g2_multiexp(const std::vector<std::pair<P2, td::RefInt256>> &ps);
P2 g2_zero();
P2 map_to_g2(const FP2 &a);
bool g2_in_group(const P2 &a);
bool g2_is_zero(const P2 &a);

bool pairing(const std::vector<std::pair<P1, P2>> &ps);

td::RefInt256 get_r();

}  // namespace bls
}  // namespace vm
