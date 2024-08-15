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

bool verify(const P1 &pub, td::Slice msg, const P2 &sig) {
  try {
    blst::P1_Affine p1(pub.data(), P1_SIZE);
    if (p1.is_inf()) {
      return false;
    }
    blst::P2_Affine p2(sig.data(), P2_SIZE);
    // core_verify checks for p1.in_group() and p2.in_group()
    return p2.core_verify(p1, true, (const byte *)msg.data(), msg.size(), DST) == BLST_SUCCESS;
  } catch (BLST_ERROR) {
    return false;
  }
}

P2 aggregate(const std::vector<P2> &sig) {
  try {
    if (sig.empty()) {
      throw VmError{Excno::unknown, "no signatures"};
    }
    blst::P2 aggregated;
    for (size_t i = 0; i < sig.size(); ++i) {
      blst::P2_Affine p2(sig[i].data(), P2_SIZE);
      if (i == 0) {
        aggregated = p2.to_jacobian();
      } else {
        aggregated.aggregate(p2);
      }
    }
    P2 result;
    aggregated.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

bool fast_aggregate_verify(const std::vector<P1> &pubs, td::Slice msg, const P2 &sig) {
  try {
    if (pubs.empty()) {
      return false;
    }
    blst::P1 p1_aggregated;
    for (size_t i = 0; i < pubs.size(); ++i) {
      blst::P1_Affine p1(pubs[i].data(), P1_SIZE);
      if (p1.is_inf()) {
        return false;
      }
      if (i == 0) {
        p1_aggregated = p1.to_jacobian();
      } else {
        p1_aggregated.aggregate(p1);
      }
    }
    blst::P2_Affine p2(sig.data(), P2_SIZE);
    blst::P1_Affine p1 = p1_aggregated.to_affine();
    // core_verify checks for p1.in_group() and p2.in_group()
    return p2.core_verify(p1, true, (const byte *)msg.data(), msg.size(), DST) == BLST_SUCCESS;
  } catch (BLST_ERROR) {
    return false;
  }
}

bool aggregate_verify(const std::vector<std::pair<P1, td::BufferSlice>> &pubs_msgs, const P2 &sig) {
  try {
    if (pubs_msgs.empty()) {
      return false;
    }
    std::unique_ptr<blst::Pairing> pairing = std::make_unique<blst::Pairing>(true, DST);
    blst::P2_Affine p2_zero;
    for (const auto &p : pubs_msgs) {
      blst::P1_Affine p1(p.first.data(), P1_SIZE);
      if (!p1.in_group() || p1.is_inf()) {
        return false;
      }
      pairing->aggregate(&p1, &p2_zero, (const td::uint8 *)p.second.data(), p.second.size());
    }
    pairing->commit();
    blst::P2_Affine p2(sig.data(), P2_SIZE);
    if (!p2.in_group()) {
      return false;
    }
    blst::PT pt(p2);
    return pairing->finalverify(&pt);
  } catch (BLST_ERROR) {
    return false;
  }
}

template <typename P, typename blst_P, typename blst_P_Affine>
static P generic_add(const P &a, const P &b) {
  try {
    blst_P point(a.data(), a.size() / 8);
    point.aggregate(blst_P_Affine(b.data(), b.size() / 8));
    P result;
    point.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

template <typename P, typename blst_P, typename blst_P_Affine>
static P generic_sub(const P &a, const P &b) {
  try {
    blst_P point(b.data(), b.size() / 8);
    point.neg();
    point.aggregate(blst_P_Affine(a.data(), a.size() / 8));
    P result;
    point.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

template <typename P, typename blst_P>
static P generic_neg(const P &a) {
  try {
    blst_P point(a.data(), a.size() / 8);
    point.neg();
    P result;
    point.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

template <typename P, typename blst_P>
static P generic_zero() {
  static P zero = []() -> P {
    blst_P point = blst_P();
    P result;
    point.compress(result.data());
    return result;
  }();
  return zero;
}

template <typename P, typename blst_P>
static P generic_mul(const P &p, const td::RefInt256 &x) {
  CHECK(x.not_null() && x->is_valid());
  if (x->sgn() == 0) {
    return generic_zero<P, blst_P>();
  }
  td::uint8 x_bytes[32];
  CHECK((x % get_r())->export_bytes(x_bytes, 32, false));
  try {
    blst_P point(p.data(), p.size() / 8);
    blst::Scalar scalar;
    scalar.from_bendian(x_bytes, 32);
    point.mult(scalar);
    P result;
    point.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

template <typename P, typename blst_P, typename blst_P_Affine, typename blst_P_Affines>
static P generic_multiexp(const std::vector<std::pair<P, td::RefInt256>> &ps) {
  if (ps.size() == 1) {
    return generic_mul<P, blst_P>(ps[0].first, ps[0].second);
  }
  try {
    std::vector<blst_P_Affine> points(ps.size());
    std::vector<td::Bits256> scalars(ps.size());
    std::vector<const byte *> scalar_ptrs(ps.size());
    for (size_t i = 0; i < ps.size(); ++i) {
      points[i] = blst_P_Affine(ps[i].first.data(), ps[i].first.size() / 8);
      CHECK(ps[i].second.not_null() && ps[i].second->is_valid());
      CHECK((ps[i].second % get_r())->export_bytes_lsb(scalars[i].data(), 32));
      scalar_ptrs[i] = (const byte *)&scalars[i];
    }
    blst_P point =
        ps.empty() ? blst_P() : blst_P_Affines::mult_pippenger(points.data(), points.size(), scalar_ptrs.data(), 256);
    P result;
    point.compress(result.data());
    return result;
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

template <typename P, typename blst_P>
static bool generic_in_group(const P &a) {
  try {
    blst_P point = blst_P(a.data(), a.size() / 8);
    return point.in_group();
  } catch (BLST_ERROR e) {
    return false;
  }
}

template <typename P, typename blst_P>
static bool generic_is_zero(const P &a) {
  return a == generic_zero<P, blst_P>();
}

P1 g1_add(const P1 &a, const P1 &b) {
  return generic_add<P1, blst::P1, blst::P1_Affine>(a, b);
}

P1 g1_sub(const P1 &a, const P1 &b) {
  return generic_sub<P1, blst::P1, blst::P1_Affine>(a, b);
}

P1 g1_neg(const P1 &a) {
  return generic_neg<P1, blst::P1>(a);
}

P1 g1_mul(const P1 &p, const td::RefInt256 &x) {
  return generic_mul<P1, blst::P1>(p, x);
}

P1 g1_multiexp(const std::vector<std::pair<P1, td::RefInt256>> &ps) {
  return generic_multiexp<P1, blst::P1, blst::P1_Affine, blst::P1_Affines>(ps);
}

P1 g1_zero() {
  return generic_zero<P1, blst::P1>();
}

P1 map_to_g1(const FP &a) {
  blst_fp fp;
  blst_fp_from_bendian(&fp, a.data());
  blst_p1 point;
  blst_map_to_g1(&point, &fp, nullptr);
  P1 result;
  blst_p1_compress(result.data(), &point);
  return result;
}

bool g1_in_group(const P1 &a) {
  return generic_in_group<P1, blst::P1>(a);
}

bool g1_is_zero(const P1 &a) {
  return generic_is_zero<P1, blst::P1>(a);
}

P2 g2_add(const P2 &a, const P2 &b) {
  return generic_add<P2, blst::P2, blst::P2_Affine>(a, b);
}

P2 g2_sub(const P2 &a, const P2 &b) {
  return generic_sub<P2, blst::P2, blst::P2_Affine>(a, b);
}

P2 g2_neg(const P2 &a) {
  return generic_neg<P2, blst::P2>(a);
}

P2 g2_mul(const P2 &p, const td::RefInt256 &x) {
  return generic_mul<P2, blst::P2>(p, x);
}

P2 g2_multiexp(const std::vector<std::pair<P2, td::RefInt256>> &ps) {
  return generic_multiexp<P2, blst::P2, blst::P2_Affine, blst::P2_Affines>(ps);
}

P2 g2_zero() {
  return generic_zero<P2, blst::P2>();
}

P2 map_to_g2(const FP2 &a) {
  blst_fp2 fp2;
  blst_fp_from_bendian(&fp2.fp[0], a.data());
  blst_fp_from_bendian(&fp2.fp[1], a.data() + FP_SIZE);
  blst_p2 point;
  blst_map_to_g2(&point, &fp2, nullptr);
  P2 result;
  blst_p2_compress(result.data(), &point);
  return result;
}

bool g2_in_group(const P2 &a) {
  return generic_in_group<P2, blst::P2>(a);
}

bool g2_is_zero(const P2 &a) {
  return generic_is_zero<P2, blst::P2>(a);
}

bool pairing(const std::vector<std::pair<P1, P2>> &ps) {
  try {
    std::unique_ptr<blst::Pairing> pairing = std::make_unique<blst::Pairing>(true, DST);
    for (const auto &p : ps) {
      blst::P1_Affine point1(p.first.data(), P1_SIZE);
      blst::P2_Affine point2(p.second.data(), P2_SIZE);
      pairing->raw_aggregate(&point2, &point1);
    }
    pairing->commit();
    return pairing->finalverify();
  } catch (BLST_ERROR e) {
    throw VmError{Excno::unknown, PSTRING() << "blst error " << e};
  }
}

td::RefInt256 get_r() {
  static td::RefInt256 r = td::dec_string_to_int256(
      td::Slice{"52435875175126190479447740508185965837690552500527637822603658699938581184513"});
  return r;
}

}  // namespace bls
}  // namespace vm
