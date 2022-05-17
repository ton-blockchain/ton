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
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <array>
#include <string>
#include <iostream>
#include <sstream>
#include <random>
#include "common/refcnt.hpp"
#include "common/bigint.hpp"
#include "common/refint.h"
#include "modbigint.cpp"

#include "td/utils/tests.h"

int mkint_chk_mode = -1, res_chk_mode = 0;
long long iterations = 100000, cur_iteration = -1, debug_iteration = -2;
#define IFDEBUG if (cur_iteration == debug_iteration || debug_iteration == -3)

using BInt = modint::ModArray<18>;     // integers up to 2^537
using MRInt = modint::MixedRadix<18>;  // auxiliary integer representation for printing, comparing etc

MRInt p2_256, np2_256, p2_63, np2_63;
constexpr long long ll_min = -2 * (1LL << 62), ll_max = ~ll_min;
constexpr double dbl_pow256 = 1.1579208923731619542e77 /* 0x1p256 */;  // 2^256

std::mt19937_64 Random(666);

template <typename T>
bool equal(td::RefInt256 x, T y) {
  return !td::cmp(x, y);
}

bool equal_or_nan(td::RefInt256 x, td::RefInt256 y) {
  return equal(x, y) || (!x->is_valid() && !y->fits_bits(257)) || (!y->is_valid() && !x->fits_bits(257));
}

#define CHECK_EQ(__x, __y) CHECK(equal(__x, __y))
#define CHECK_EQ_NAN(__x, __y) CHECK(equal_or_nan(__x, __y))

bool mr_in_range(const MRInt& x) {
  return x < p2_256 && x >= np2_256;
}

bool mr_is_small(const MRInt& x) {
  return x < p2_63 && x >= np2_63;
}

bool mr_fits_bits(const MRInt& x, int bits) {
  if (bits > 0) {
    return x < MRInt::pow2(bits - 1) && x >= MRInt::negpow2(bits - 1);
  } else {
    return !bits && !x.sgn();
  }
}

bool mr_ufits_bits(const MRInt& x, int bits) {
  return bits >= 0 && x.sgn() >= 0 && x < MRInt::pow2(bits);
}

struct ShowBin {
  unsigned char* data;
  ShowBin(unsigned char _data[64]) : data(_data) {
  }
};

std::ostream& operator<<(std::ostream& os, ShowBin bin) {
  int i = 0, s = bin.data[0];
  if (s == 0 || s == 0xff) {
    while (i < 64 && bin.data[i] == s) {
      i++;
    }
  }
  if (i >= 3) {
    os << (s ? "ff..ff" : "00..00");
  } else {
    i = 0;
  }
  constexpr static char hex_digits[] = "0123456789abcdef";
  while (i < 64) {
    int t = bin.data[i++];
    os << hex_digits[t >> 4] << hex_digits[t & 15];
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const td::AnyIntView<td::BigIntInfo>& x) {
  os << '[';
  for (int i = 0; i < x.size(); i++) {
    os << ' ' << x.digits[i];
  }
  os << " ]";
  return os;
}

template <typename T>
bool extract_value_any_bool(BInt& val, const td::AnyIntView<T>& x, bool chk_norm = true) {
  int n = x.size();
  if (n <= 0 || n > x.max_size() || (!x.digits[n - 1] && n > 1)) {
    return false;
  }
  assert(n == 1 || x.digits[n - 1] != 0);
  val.set_zero();
  for (int i = n - 1; i >= 0; --i) {
    val.lshift_add(T::word_shift, x.digits[i]);
    if (chk_norm && (x.digits[i] < -T::Half || x.digits[i] >= T::Half)) {
      return false;  // unnormalized
    }
  }
  return true;
}

template <typename T>
bool extract_value_bool(BInt& val, const T& x, bool chk_norm = true) {
  return extract_value_any_bool(val, x.as_any_int(), chk_norm);
}

BInt extract_value_any(const td::AnyIntView<td::BigIntInfo>& x, bool chk_norm = true) {
  BInt res;
  CHECK(extract_value_any_bool(res, x, chk_norm));
  return res;
}

template <typename T>
BInt extract_value(const T& x, bool chk_norm = true) {
  return extract_value_any(x.as_any_int(), chk_norm);
}

template <typename T>
BInt extract_value_alt(const T& x) {
  BInt res;
  const int* md = res.mod_array();
  for (int i = 0; i < res.n / 2; i++) {
    T copy{x};
    int m1 = md[2 * i], m2 = md[2 * i + 1];
    long long rem = copy.divmod_short((long long)m1 * m2);
    res.a[2 * i] = (int)(rem % m1);
    res.a[2 * i + 1] = (int)(rem % m2);
  }
  if (res.n & 1) {
    T copy{x};
    res.a[res.n - 1] = (int)copy.divmod_short(md[res.n - 1]);
  }
  return res;
}

constexpr int min_spec_int = -0xfd08, max_spec_int = 0xfd07;
// x = sgn*(ord*256+a*16+b) => sgn*((32+a)*2^(ord-2) + b - 8)
// x = -0xfd08 => -2^256 ... x = 0xfd07 => 2^256 - 1
td::RefInt256 make_special_int(int x, BInt* ptr = nullptr, unsigned char bin[64] = nullptr) {
  bool sgn = (x < 0);
  if (sgn) {
    x = -x;
  }
  int ord = (x >> 8) - 2, a = 32 + ((x >> 4) & 15), b = (x & 15) - 8;
  if (ord < 0) {
    a >>= -ord;
    ord = 0;
  }
  if (sgn) {
    a = -a;
    b = -b;
  }
  if (ptr) {
    ptr->set_int(a);
    *ptr <<= ord;
    *ptr += b;
  }
  if (bin) {
    int acc = b, r = ord;
    for (int i = 63; i >= 0; --i) {
      if (r < 8) {
        acc += (a << r);
        r = 1024;
      }
      r -= 8;
      bin[i] = (unsigned char)(acc & 0xff);
      acc >>= 8;
    }
  }
  return (td::make_refint(a) << ord) + b;
}

int rand_int(int min, int max) {
  return min + (int)(Random() % (max - min + 1));
}

unsigned randu() {
  return (unsigned)(Random() << 16);
}

bool coin() {
  return Random() & (1 << 28);
}

// returns 0 with probability 1/2, 1 with prob. 1/4, ..., k with prob. 1/2^(k+1)
int randexp(int max = 63, int min = 0) {
  return min + __builtin_clzll(Random() | (1ULL << (63 - max + min)));
}

void bin_add_small(unsigned char bin[64], long long val, int shift = 0) {
  val <<= shift & 7;
  for (int i = 63 - (shift >> 3); i >= 0 && val; --i) {
    val += bin[i];
    bin[i] = (unsigned char)val;
    val >>= 8;
  }
}

// adds sgn * (random number less than 2^(ord - ord2)) * 2^ord2
td::RefInt256 add_random_bits(td::RefInt256 x, BInt& val, unsigned char bin[64], int ord2, int ord, int sgn = 1) {
  int t;
  do {
    t = std::max((ord - 1) & -16, ord2);
    int a = sgn * rand_int(0, (1 << (ord - t)) - 1);
    // add a << t
    val.add_lshift(t, a);
    x += td::make_refint(a) << t;
    bin_add_small(bin, a, t);
    ord = t;
  } while (t > ord2);
  return x;
}

// generates a random integer in range -2^256 .. 2^256-1 (and sometimes outside)
// distribution is skewed towards +/- 2^n +/- 2^n +/- smallint, but completely random integers are also generated
td::RefInt256 make_random_int0(BInt& val, unsigned char bin[64]) {
  memset(bin, 0, 64);
  int ord = rand_int(-257, 257);
  if (ord <= 2 && ord >= -2) {
    // -2..2 represent themselves
    val.set_int(ord);
    bin_add_small(bin, ord);
    return td::make_refint(ord);
  }
  int sgn = (ord < 0 ? -1 : 1);
  ord = sgn * ord - 1;
  int f = std::min(ord, randexp(15)), a = sgn * rand_int(1 << f, (2 << f) - 1);
  ord -= f;
  // first summand is a << ord
  auto res = td::make_refint(a) << ord;
  val.set_int(a);
  val <<= ord;
  bin_add_small(bin, a, ord);
  if (!ord) {
    // all bits ready
    return res;
  }
  for (int s = 0; s < 2 && ord; s++) {
    // decide whether we want an intermediate order (50%), and whether we want randomness above/below that order
    int ord2 = (s ? 0 : std::max(0, rand_int(~ord, ord - 1)));
    if (!rand_int(0, 4)) {  // 20%
      // random bits between ord2 and ord
      res = add_random_bits(std::move(res), val, bin, ord2, ord, sgn);
    }
    if (rand_int(0, 4)) {  // 80%
      // non-zero adjustment
      f = randexp(15);
      a = rand_int(-(2 << f) + 1, (2 << f) - 1);
      ord = std::max(ord2 - f, 0);
      // add a << ord
      val.add_lshift(ord, a);
      res += (td::make_refint(a) << ord);
      bin_add_small(bin, a, ord);
    }
  }
  return res;
}

td::RefInt256 make_random_int(BInt& val, unsigned char bin[64]) {
  while (true) {
    auto res = make_random_int0(val, bin);
    if (res->fits_bits(257)) {
      return res;
    }
  }
}

void check_one_int_repr(td::RefInt256 x, int mode, int in_range, const BInt* valptr = nullptr,
                        const unsigned char bin[64] = nullptr) {
  CHECK(x.not_null() && (in_range <= -2 || x->is_valid()));
  if (!x->is_valid()) {
    // not much to check when x is a NaN
    unsigned char bytes[64];
    if (valptr) {
      // check that the true answer at `valptr` is out of range
      CHECK(!mr_in_range(valptr->to_mixed_radix()));
      if (mode & 0x200) {
        // check BInt binary export
        valptr->to_binary(bytes, 64);
        if (bin) {
          // check that the two true answers match
          CHECK(!memcmp(bin, bytes, 64));
        } else {
          bin = bytes;
        }
      }
    }
    if (bin) {
      // check that the true answer in `bin` is out of range
      int i = 0, sgn = (bin[0] >= 0x80 ? -1 : 0);
      while (i < 32 && bin[i] == (unsigned char)sgn)
        ;
      CHECK(i < 32);
      if (valptr && (mode & 0x100)) {
        // check BInt binary export
        BInt val2;
        val2.from_binary(bin, 64);
        CHECK(*valptr == val2);
      }
    }
    return;
  }
  unsigned char bytes[64];
  CHECK(x->export_bytes(bytes, 64));
  if (bin) {
    CHECK(!memcmp(bytes, bin, 64));
  }
  BInt val = extract_value(*x);
  if (valptr) {
    if (val != *valptr) {
      std::cerr << "extracted " << val << " from " << x << ' ' << x->as_any_int() << ", expected " << *valptr
                << std::endl;
    }
    CHECK(val == *valptr);
  }
  if (mode & 1) {
    BInt val2 = extract_value_alt(*x);
    CHECK(val == val2);
  }
  if (mode & 2) {
    // check binary import
    td::BigInt256 y;
    y.import_bytes(bytes, 64);
    CHECK(y == *x);
  }
  if (mode & 0x100) {
    // check binary import for BInt
    BInt val2;
    val2.from_binary(bytes, 64);
    CHECK(val == val2);
  }
  // check if small (fits into 64 bits)
  long long xval = (long long)val;
  bool is_small = (xval != ll_min || val == xval);
  CHECK(is_small == x->fits_bits(64));
  if (is_small) {
    // special check for small (64-bit) values
    CHECK(x->to_long() == xval);
    CHECK((long long)__builtin_bswap64(*(long long*)(bytes + 64 - 8)) == xval);
    CHECK(in_range);
    // check sign
    CHECK(x->sgn() == (xval > 0 ? 1 : (xval < 0 ? -1 : 0)));
    // check comparison with long long
    CHECK(x == xval);
    CHECK(!cmp(x, xval));
    if (mode & 4) {
      // check constructor from long long
      CHECK(!cmp(x, td::make_refint(xval)));
      if (xval != ll_min) {
        CHECK(x > xval - 1);
        CHECK(x > td::make_refint(xval - 1));
      }
      if (xval != ll_max) {
        CHECK(x < xval + 1);
        CHECK(x < td::make_refint(xval + 1));
      }
    }
    if (!(mode & ~0x107)) {
      return;  // fast check for small ints in this case
    }
  }

  MRInt mval(val);  // somewhat slow
  bool val_in_range = mr_in_range(mval);
  CHECK(x->fits_bits(257) == val_in_range);
  if (in_range >= 0) {
    CHECK((int)val_in_range == in_range);
  }
  if (mode & 0x200) {
    // check binary export for BInt
    unsigned char bytes2[64];
    mval.to_binary(bytes2, 64);
    CHECK(!memcmp(bytes, bytes2, 64));
  }
  // check sign
  int sgn = mval.sgn();
  CHECK(x->sgn() == sgn);
  CHECK(is_small == mr_is_small(mval));
  if (is_small) {
    CHECK((long long)mval == xval);
  }
  if (mode & 0x10) {
    // check decimal export
    std::string dec = mval.to_dec_string();
    CHECK(x->to_dec_string() == dec);
    // check decimal import
    td::BigInt256 y;
    int l = y.parse_dec(dec);
    CHECK((std::size_t)l == dec.size() && y == *x);
    if (mode & 0x1000) {
      // check decimal import for BInt
      BInt val2;
      CHECK(val2.from_dec_string(dec) && val2 == val);
    }
  }
  if (mode & 0x20) {
    // check binary bit size
    int sz = x->bit_size();
    CHECK(sz >= 0 && sz <= 300);
    CHECK(x->fits_bits(sz) && (!sz || !x->fits_bits(sz - 1)));
    CHECK(mr_fits_bits(mval, sz) && !mr_fits_bits(mval, sz - 1));
    int usz = x->bit_size(false);
    CHECK(sgn >= 0 || usz == 0x7fffffff);
    if (sgn >= 0) {
      CHECK(x->unsigned_fits_bits(usz) && (!usz || !x->unsigned_fits_bits(usz - 1)));
      CHECK(mr_ufits_bits(mval, usz) && !mr_ufits_bits(mval, usz - 1));
    } else {
      CHECK(!x->unsigned_fits_bits(256) && !x->unsigned_fits_bits(300));
    }
  }
}

void init_aux() {
  np2_256 = p2_256 = MRInt::pow2(256);
  np2_256.negate();
  CHECK(np2_256 == MRInt::negpow2(256));
  p2_63 = np2_63 = MRInt::pow2(63);
  np2_63.negate();
  CHECK(np2_63 == MRInt::negpow2(63));
}

std::vector<td::RefInt256> SpecInt;
BInt SpecIntB[max_spec_int - min_spec_int + 1];

void init_check_special_ints() {
  std::cerr << "check special ints" << std::endl;
  BInt b;
  unsigned char binary[64];
  for (int idx = min_spec_int - 512; idx <= max_spec_int + 512; idx++) {
    td::RefInt256 x = make_special_int(idx, &b, binary);
    check_one_int_repr(x, mkint_chk_mode, idx >= min_spec_int && idx <= max_spec_int, &b, binary);
    if (idx >= min_spec_int && idx <= max_spec_int) {
      SpecIntB[idx - min_spec_int] = b;
      SpecInt.push_back(std::move(x));
    }
  }
}

void check_res(td::RefInt256 y, const BInt& yv) {
  check_one_int_repr(std::move(y), res_chk_mode, -2, &yv);
}

void check_unary_ops_on(td::RefInt256 x, const BInt& xv) {
  // NEGATE
  BInt yv = -xv;
  check_res(-x, yv);
  // NOT
  check_res(~x, yv -= 1);
}

void check_unary_ops() {
  std::cerr << "check unary ops" << std::endl;
  for (int idx = min_spec_int; idx <= max_spec_int; idx++) {
    check_unary_ops_on(SpecInt[idx - min_spec_int], SpecIntB[idx - min_spec_int]);
  }
}

void check_pow2_ops(int shift) {
  // POW2
  td::RefInt256 r{true};
  r.unique_write().set_pow2(shift);
  check_res(r, BInt::pow2(shift));
  // POW2DEC
  r.unique_write().set_pow2(shift).add_tiny(-1).normalize();
  check_res(r, BInt::pow2(shift) - 1);
  // NEGPOW2
  r.unique_write().set_pow2(shift).negate().normalize();
  check_res(r, -BInt::pow2(shift));
}

void check_pow2_ops() {
  std::cerr << "check power-2 ops" << std::endl;
  for (int i = 0; i <= 256; i++) {
    check_pow2_ops(i);
  }
}

void check_shift_ops_on(int shift, td::RefInt256 x, const BInt& xv, const MRInt& mval) {
  // LSHIFT
  check_res(x << shift, xv << shift);
  // FITS
  CHECK(x->fits_bits(shift) == mr_fits_bits(mval, shift));
  // UFITS
  CHECK(x->unsigned_fits_bits(shift) == mr_ufits_bits(mval, shift));
  // ADDPOW2 / SUBPOW2
  auto y = x;
  y.write().add_pow2(shift).normalize();
  check_res(std::move(y), xv + BInt::pow2(shift));
  y = x;
  y.write().sub_pow2(shift).normalize();
  check_res(std::move(y), xv - BInt::pow2(shift));
  // RSHIFT, MODPOW2
  for (int round_mode = -1; round_mode <= 1; round_mode++) {
    auto r = x, q = td::rshift(x, shift, round_mode);  // RSHIFT
    CHECK(q.not_null() && q->is_valid());
    r.write().mod_pow2(shift, round_mode).normalize();  // MODPOW2
    CHECK(r.not_null() && r->is_valid());
    if (round_mode < 0) {
      CHECK(!cmp(x >> shift, q));  // operator>> should be equivalent to td::rshift
    }
    BInt qv = extract_value(*q), rv = extract_value(*r);
    // check main division equality (q << shift) + r == x
    CHECK((qv << shift) + rv == xv);
    MRInt rval(rv);
    // check remainder range
    switch (round_mode) {
      case 1:
        rval.negate();  // fallthrough
      case -1:
        CHECK(mr_ufits_bits(rval, shift));
        break;
      case 0:
        CHECK(mr_fits_bits(rval, shift));
    }
  }
}

void check_shift_ops() {
  std::cerr << "check left/right shift ops" << std::endl;
  for (int idx = min_spec_int; idx <= max_spec_int; idx++) {
    //for (int idx : {-52240, -52239, -52238, -3, -2, -1, 0, 1, 2, 3, 52238, 52239, 52240}) {
    const auto& xv = SpecIntB[idx - min_spec_int];
    MRInt mval(xv);
    if (!(idx % 1000)) {
      std::cerr << "# " << idx << " : " << mval << std::endl;
    }
    for (int i = 0; i <= 256; i++) {
      check_shift_ops_on(i, SpecInt[idx - min_spec_int], xv, mval);
    }
  }
}

void check_remainder_range(BInt& rv, const BInt& dv, int rmode = -1) {
  if (rmode > 0) {
    rv.negate();
  } else if (!rmode) {
    rv *= 2;
  }
  MRInt d(dv), r(rv);
  int ds = d.sgn(), rs = r.sgn();
  //std::cerr << "rmode=" << rmode << " ds=" << ds << " rs=" << rs << " d=" << d << " r=" << r << std::endl;
  if (!rs) {
    return;
  }
  if (rmode) {
    // must have 0 < r < d or 0 > r > d
    //if (rs != ds) std::cerr << "iter=" << cur_iteration << " : rmode=" << rmode << " ds=" << ds << " rs=" << rs << " d=" << d << " r=" << r << std::endl;
    CHECK(rs == ds);
    CHECK(ds * r.cmp(d) < 0);
  } else {
    // must have -d <= r < d or -d >= r > d
    if (rs == -ds) {
      r.negate();
      CHECK(ds * r.cmp(d) <= 0);
    } else {
      CHECK(ds * r.cmp(d) < 0);
    }
  }
}

void check_divmod(td::RefInt256 x, const BInt& xv, long long xl, td::RefInt256 y, const BInt& yv, long long yl,
                  int rmode = -2) {
  if (rmode < -1) {
    //IFDEBUG std::cerr << "  divide " << x << " / " << y << std::endl;
    for (rmode = -1; rmode <= 1; rmode++) {
      check_divmod(x, xv, xl, y, yv, yl, rmode);
    }
    return;
  }
  auto dm = td::divmod(x, y, rmode);
  auto q = std::move(dm.first), r = std::move(dm.second);
  if (!yl) {
    // division by zero
    CHECK(q.not_null() && !q->is_valid() && r.not_null() && !r->is_valid());
    return;
  }
  CHECK(q.not_null() && q->is_valid() && r.not_null() && r->is_valid());
  CHECK_EQ(x, y * q + r);
  BInt qv = extract_value(*q), rv = extract_value(*r);
  CHECK(xv == yv * qv + rv);
  //IFDEBUG std::cerr << "    quot=" << q << " rem=" << r << std::endl;
  check_remainder_range(rv, yv, rmode);
  if (yl != ll_min && rmode == -1) {
    // check divmod_short()
    auto qq = x;
    auto rem = qq.write().divmod_short(yl);
    qq.write().normalize();
    CHECK(qq->is_valid());
    CHECK_EQ(qq, q);
    CHECK(r == rem);
    if (xl != ll_min) {
      auto dm = std::lldiv(xl, yl);
      if (dm.rem && (dm.rem ^ yl) < 0) {
        dm.rem += yl;
        dm.quot--;
      }
      CHECK(q == dm.quot);
      CHECK(r == dm.rem);
    }
  }
}

void check_binary_ops_on(td::RefInt256 x, const BInt& xv, td::RefInt256 y, const BInt& yv) {
  bool x_small = x->fits_bits(62), y_small = y->fits_bits(62);  // not 63
  long long xl = x_small ? x->to_long() : ll_min, yl = y_small ? y->to_long() : ll_min;
  if (x_small) {
    CHECK(x == xl);
  }
  if (y_small) {
    CHECK(y == yl);
  }
  // ADD, ADDR
  auto z = x + y, w = y + x;
  CHECK_EQ(z, w);
  check_res(z, xv + yv);
  // ADDCONST
  if (y_small) {
    CHECK_EQ(z, x + yl);
  }
  if (x_small) {
    CHECK_EQ(z, y + xl);
  }
  if (x_small && y_small) {
    CHECK_EQ(z, xl + yl);
  }
  // SUB
  z = x - y;
  check_res(z, xv - yv);
  // SUBCONST
  if (y_small) {
    CHECK_EQ(z, x - yl);
    if (x_small) {
      CHECK_EQ(z, xl - yl);
    }
  }
  // SUBR
  z = y - x;
  check_res(z, yv - xv);
  if (x_small) {
    CHECK_EQ(z, y - xl);
    if (y_small) {
      CHECK_EQ(z, yl - xl);
    }
  }
  // CMP
  MRInt xmr(xv), ymr(yv);
  int cmpv = xmr.cmp(ymr);
  CHECK(td::cmp(x, y) == cmpv);
  CHECK(td::cmp(y, x) == -cmpv);
  if (y_small) {
    CHECK(td::cmp(x, yl) == cmpv);
  }
  if (x_small) {
    CHECK(td::cmp(y, xl) == -cmpv);
  }
  if (x_small && y_small) {
    CHECK(cmpv == (xl < yl ? -1 : (xl > yl ? 1 : 0)));
  }
  // MUL
  z = x * y;
  BInt zv = xv * yv;
  check_res(z, zv);
  CHECK_EQ(z, y * x);
  // MULCONST
  if (y_small) {
    CHECK_EQ_NAN(z, x * yl);
  }
  if (x_small) {
    CHECK_EQ_NAN(z, y * xl);
  }
  if (x_small && y_small && (!yl || std::abs(xl) <= ll_max / std::abs(yl))) {
    CHECK_EQ(z, xl * yl);
  }
  // DIVMOD
  if (z->fits_bits(257)) {
    int adj = 2 * rand_int(-2, 2) - (int)z->is_odd();
    z += adj;
    z >>= 1;
    zv += adj;
    zv >>= 1;
    // z is approximately x * y / 2; divide by y
    check_divmod(z, zv, z->fits_bits(62) ? z->to_long() : ll_min, y, yv, yl);
  }
  check_divmod(x, xv, xl, y, yv, yl);
}

void finish_check_muldivmod(td::RefInt256 x, const BInt& xv, td::RefInt256 y, const BInt& yv, td::RefInt256 z,
                            const BInt& zv, td::RefInt256 q, td::RefInt256 r, int rmode) {
  static constexpr double eps = 1e-14;
  CHECK(q.not_null() && r.not_null());
  //std::cerr << " muldivmod: " << xv << " * " << yv << " / " << zv << " (round " << rmode << ") = " << q << " " << r << std::endl;
  if (!zv) {
    // division by zero
    CHECK(!q->is_valid() && !r->is_valid());
    return;
  }
  CHECK(r->is_valid());  // remainder always exists if y != 0
  BInt xyv = xv * yv, rv = extract_value(*r);
  MRInt xy_mr(xyv), z_mr(zv);
  double q0 = (double)xy_mr / (double)z_mr;
  if (std::abs(q0) < 1.01 * dbl_pow256) {
    // result more or less in range
    CHECK(q->is_valid());
  } else if (!q->is_valid()) {
    // result out of range, NaN is an acceptable answer
    // check that x * y - r is divisible by z
    xyv -= rv;
    xyv /= zv;
    xy_mr = xyv;
    double q1 = (double)xy_mr;
    CHECK(std::abs(q1 - q0) < eps * std::abs(q0));
  } else {
    BInt qv = extract_value(*q);
    // must have x * y = z * q + r
    CHECK(xv * yv == zv * qv + rv);
  }
  // check that r is in correct range [0, z) or [0, -z) or [-z/2, z/2)
  check_remainder_range(rv, zv, rmode);
}

void check_muldivmod_on(td::RefInt256 x, const BInt& xv, td::RefInt256 y, const BInt& yv, td::RefInt256 z,
                        const BInt& zv, int rmode = 2) {
  if (rmode < -1) {
    for (rmode = -1; rmode <= 1; rmode++) {
      check_muldivmod_on(x, xv, y, yv, z, zv, rmode);
    }
    return;
  } else if (rmode > 1) {
    rmode = rand_int(-1, 1);
  }
  // MULDIVMOD
  auto qr = td::muldivmod(x, y, z, rmode);
  finish_check_muldivmod(std::move(x), xv, std::move(y), yv, std::move(z), zv, std::move(qr.first),
                         std::move(qr.second), rmode);
}

void check_mul_rshift_on(td::RefInt256 x, const BInt& xv, td::RefInt256 y, const BInt& yv, int shift, int rmode = 2) {
  if (rmode < -1) {
    for (rmode = -1; rmode <= 1; rmode++) {
      check_mul_rshift_on(x, xv, y, yv, shift, rmode);
    }
    return;
  } else if (rmode > 1) {
    rmode = rand_int(-1, 1);
  }
  // MULRSHIFTMOD
  typename td::BigInt256::DoubleInt tmp{0};
  tmp.add_mul(*x, *y);
  typename td::BigInt256::DoubleInt tmp2{tmp};
  tmp2.rshift(shift, rmode).normalize();
  tmp.normalize().mod_pow2(shift, rmode).normalize();
  finish_check_muldivmod(std::move(x), xv, std::move(y), yv, {}, BInt::pow2(shift), td::make_refint(tmp2),
                         td::make_refint(tmp), rmode);
}

void check_lshift_div_on(td::RefInt256 x, const BInt& xv, td::RefInt256 y, const BInt& yv, int shift, int rmode = 2) {
  if (rmode < -1) {
    for (rmode = -1; rmode <= 1; rmode++) {
      check_lshift_div_on(x, xv, y, yv, shift, rmode);
    }
    return;
  } else if (rmode > 1) {
    rmode = rand_int(-1, 1);
  }
  // LSHIFTDIV
  typename td::BigInt256::DoubleInt tmp{*x}, quot;
  tmp <<= shift;
  tmp.mod_div(*y, quot, rmode);
  quot.normalize();
  finish_check_muldivmod(std::move(x), xv, {}, BInt::pow2(shift), std::move(y), yv, td::make_refint(quot),
                         td::make_refint(tmp), rmode);
}

void check_random_ops() {
  constexpr long long chk_it = 100000;
  std::cerr << "check random ops (" << iterations << " iterations)" << std::endl;
  BInt xv, yv, zv;
  unsigned char xbin[64], ybin[64], zbin[64];
  for (cur_iteration = 0; cur_iteration < iterations; cur_iteration++) {
    auto x = make_random_int0(xv, xbin);
    if (!(cur_iteration % 10000)) {
      std::cerr << "#" << cur_iteration << ": check on " << xv << " = " << ShowBin(xbin) << " = " << x->as_any_int()
                << std::endl;
    }
    check_one_int_repr(x, cur_iteration < chk_it ? -1 : 0, -1, &xv, xbin);
    MRInt xmr(xv);
    if (!x->fits_bits(257)) {
      continue;
    }
    check_unary_ops_on(x, xv);
    for (int j = 0; j < 10; j++) {
      int shift = rand_int(0, 256);
      //std::cerr << "check shift by " << shift << std::endl;
      check_shift_ops_on(shift, x, xv, xmr);
      auto y = make_random_int(yv, ybin);
      //std::cerr << "  y = " << y << " = " << yv << " = " << ShowBin(ybin) << " = " << y->as_any_int() << std::endl;
      check_one_int_repr(y, 0, 1, &yv, ybin);
      check_binary_ops_on(x, xv, y, yv);
      //std::cerr << "  *>> " << shift << std::endl;
      check_mul_rshift_on(x, xv, y, yv, shift);
      //std::cerr << "  <</ " << shift << std::endl;
      check_lshift_div_on(x, xv, y, yv, shift);
      auto z = make_random_int(zv, zbin);
      //std::cerr << "  */ z = " << z << " = " << zv << " = " << ShowBin(zbin) << " = " << z->as_any_int() << std::endl;
      check_muldivmod_on(x, xv, y, yv, z, zv);
    }
  }
}

void check_special() {
  std::cerr << "run special tests" << std::endl;
  check_divmod((td::make_refint(-1) << 207) - 1, BInt::negpow2(207) - 1, ll_min, (td::make_refint(1) << 207) - 1,
               BInt::pow2(207) - 1, ll_min);
}

int main(int argc, char* const argv[]) {
  bool do_check_shift_ops = false;
  int i;
  while ((i = getopt(argc, argv, "hSs:i:")) != -1) {
    switch (i) {
      case 'S':
        do_check_shift_ops = true;
        break;
      case 's':
        Random.seed(atoll(optarg));
        break;
      case 'i':
        iterations = atoll(optarg);
        break;
      default:
        std::cerr << "unknown option: " << (char)i << std::endl;
        // fall through
      case 'h':
        std::cerr << "usage:\t" << argv[0] << " [-S] [-i<random-op-iterations>] [-s<random-seed>]" << std::endl;
        return 2;
    }
  }
  modint::init();
  init_aux();
  init_check_special_ints();
  check_pow2_ops();
  check_unary_ops();
  if (do_check_shift_ops) {
    check_shift_ops();
  }
  check_special();
  check_random_ops();
  return 0;
}
