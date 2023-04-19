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

#include "bls.h"
#include "blst.h"
#include "blst.hpp"
#include "excno.hpp"

namespace vm {
namespace bls {

static const std::string DST = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

bool verify(const PubKey &pub, td::Slice msg, const Signature &signature) {
  try {
    blst::P1_Affine p1(pub.data(), PUBKEY_SIZE);
    if (!p1.in_group() || p1.is_inf()) {
      return false;
    }
    blst::P2_Affine p2(signature.data(), SIGNATURE_SIZE);
    return p2.core_verify(p1, true, (const byte *)msg.data(), msg.size(), DST) == BLST_SUCCESS;
  } catch (BLST_ERROR) {
    return false;
  }
}

Signature aggregate(const std::vector<Signature> &signatures) {
  try {
    if (signatures.empty()) {
      throw VmError{Excno::unknown, "no signatures"};
    }
    blst::P2 aggregated;
    for (size_t i = 0; i < signatures.size(); ++i) {
      blst::P2_Affine p2(signatures[i].data(), SIGNATURE_SIZE);
      if (i == 0) {
        aggregated = p2.to_jacobian();
      } else {
        aggregated.aggregate(p2);
      }
    }
    Signature result;
    aggregated.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

bool fast_aggregate_verify(const std::vector<PubKey> &pubs, td::Slice msg, const Signature &signature) {
  try {
    if (pubs.empty()) {
      return false;
    }
    blst::P1 p1_aggregated;
    for (size_t i = 0; i < pubs.size(); ++i) {
      blst::P1_Affine p1(pubs[i].data(), PUBKEY_SIZE);
      if (!p1.in_group() || p1.is_inf()) {
        return false;
      }
      if (i == 0) {
        p1_aggregated = p1.to_jacobian();
      } else {
        p1_aggregated.aggregate(p1);
      }
    }
    blst::P2_Affine p2(signature.data(), SIGNATURE_SIZE);
    blst::P1_Affine p1 = p1_aggregated.to_affine();
    return p2.core_verify(p1, true, (const byte *)msg.data(), msg.size(), DST) == BLST_SUCCESS;
  } catch (BLST_ERROR) {
    return false;
  }
}

bool aggregate_verify(const std::vector<std::pair<PubKey, td::BufferSlice>> &pubs_msgs, const Signature &signature) {
  try {
    if (pubs_msgs.empty()) {
      return false;
    }
    std::unique_ptr<blst::Pairing> pairing = std::make_unique<blst::Pairing>(true, DST);
    for (const auto &p : pubs_msgs) {
      blst::P1_Affine p1(p.first.data(), PUBKEY_SIZE);
      if (!p1.in_group() || p1.is_inf()) {
        return false;
      }
      pairing->aggregate(&p1, nullptr, (const td::uint8 *)p.second.data(), p.second.size());
    }
    pairing->commit();
    blst::PT sig(blst::P2_Affine(signature.data(), SIGNATURE_SIZE));
    return pairing->finalverify(&sig);
  } catch (BLST_ERROR) {
    return false;
  }
}

}  // namespace bls
}  // namespace vm
