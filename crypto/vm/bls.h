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

namespace vm {
namespace bls {

const size_t PUBKEY_SIZE = 48;
const size_t SIGNATURE_SIZE = 96;

using PubKey = td::BitArray<PUBKEY_SIZE * 8>;
using Signature = td::BitArray<SIGNATURE_SIZE * 8>;

bool verify(const PubKey &pub, td::Slice msg, const Signature &signature);
Signature aggregate(const std::vector<Signature> &signatures);
bool fast_aggregate_verify(const std::vector<PubKey> &pubs, td::Slice msg, const Signature &signature);
bool aggregate_verify(const std::vector<std::pair<PubKey, td::BufferSlice>> &pubs_msgs, const Signature &signature);

}  // namespace bls
}  // namespace vm
