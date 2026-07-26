/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <algorithm>
#include <cmath>

#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "test/plumtree/util.h"

namespace ton::overlay::plumtree_sim {

PrivateKey make_key(td::Slice label) {
  td::Bits256 hash;
  td::sha256(label, hash.as_slice());
  return PrivateKey{privkeys::Ed25519{hash}};
}

void fill_payload(td::MutableSlice data, td::uint64 seed) {
  td::Random::Xorshift128plus rnd(seed, seed ^ 0x9e3779b97f4a7c15ULL);
  rnd.bytes(data);
}

double percentile(std::vector<double> values, double p) {
  values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return value < 0.0; }), values.end());
  if (values.empty()) {
    return -1.0;
  }
  std::sort(values.begin(), values.end());
  auto last_index = values.size() - 1;
  auto position = std::max(0.0, std::min(1.0, p)) * static_cast<double>(last_index);
  auto lo = static_cast<std::size_t>(std::floor(position));
  auto hi = static_cast<std::size_t>(std::ceil(position));
  if (lo == hi) {
    return values[lo];
  }
  auto weight = position - static_cast<double>(lo);
  return values[lo] * (1.0 - weight) + values[hi] * weight;
}

}  // namespace ton::overlay::plumtree_sim
