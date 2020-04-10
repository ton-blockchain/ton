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
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <cassert>

// ******************************************************

namespace openssl {
#include <openssl/bn.h>
}

namespace arith {
struct dec_string {
  std::string str;
  explicit dec_string(const std::string& s) : str(s) {
  }
};

struct hex_string {
  std::string str;
  explicit hex_string(const std::string& s) : str(s) {
  }
};
}  // namespace arith

namespace arith {

using namespace openssl;

inline void bn_assert(int cond);
BN_CTX* get_ctx();

class BignumBitref {
  BIGNUM* ptr;
  int n;

 public:
  BignumBitref(BIGNUM& x, int _n) : ptr(&x), n(_n){};
  operator bool() const {
    return BN_is_bit_set(ptr, n);
  }
  BignumBitref& operator=(bool val);
};

class Bignum {
  BIGNUM val;

 public:
  class bignum_error {};
  Bignum() {
    BN_init(&val);
  }
  Bignum(long x) {
    BN_init(&val);
    set_long(x);
  }
  ~Bignum() {
    BN_free(&val);
  }
  Bignum(const dec_string& ds) {
    BN_init(&val);
    set_dec_str(ds.str);
  }
  Bignum(const hex_string& hs) {
    BN_init(&val);
    set_hex_str(hs.str);
  }
  Bignum(const Bignum& x) {
    BN_init(&val);
    BN_copy(&val, &x.val);
  }
  //Bignum (Bignum&& x) { val = x.val; }
  void clear() {
    BN_clear(&val);
  }  // use this for sensitive data
  Bignum& operator=(const Bignum& x) {
    BN_copy(&val, &x.val);
    return *this;
  }
  Bignum& operator=(Bignum&& x) {
    swap(x);
    return *this;
  }
  Bignum& operator=(long x) {
    return set_long(x);
  }
  Bignum& operator=(const dec_string& ds) {
    return set_dec_str(ds.str);
  }
  Bignum& operator=(const hex_string& hs) {
    return set_hex_str(hs.str);
  }
  Bignum& swap(Bignum& x) {
    BN_swap(&val, &x.val);
    return *this;
  }
  BIGNUM* bn_ptr() {
    return &val;
  }
  const BIGNUM* bn_ptr() const {
    return &val;
  }
  bool is_zero() const {
    return BN_is_zero(&val);
  }
  int sign() const {
    return BN_is_zero(&val) ? 0 : (BN_is_negative(&val) ? -1 : 1);
  }
  bool odd() const {
    return BN_is_odd(&val);
  }
  int num_bits() const {
    return BN_num_bits(&val);
  }
  int num_bytes() const {
    return BN_num_bytes(&val);
  }
  bool operator[](int n) const {
    return BN_is_bit_set(&val, n);
  }
  BignumBitref operator[](int n) {
    return BignumBitref(val, n);
  }
  void export_msb(unsigned char* buffer, std::size_t size) const;
  Bignum& import_msb(const unsigned char* buffer, std::size_t size);
  Bignum& import_msb(const std::string& s) {
    return import_msb((const unsigned char*)s.c_str(), s.size());
  }
  void export_lsb(unsigned char* buffer, std::size_t size) const;
  Bignum& import_lsb(const unsigned char* buffer, std::size_t size);
  Bignum& import_lsb(const std::string& s) {
    return import_lsb((const unsigned char*)s.c_str(), s.size());
  }

  Bignum& set_dec_str(std::string s) {
    BIGNUM* tmp = &val;
    bn_assert(BN_dec2bn(&tmp, s.c_str()));
    return *this;
  }

  Bignum& set_hex_str(std::string s) {
    BIGNUM* tmp = &val;
    bn_assert(BN_hex2bn(&tmp, s.c_str()));
    return *this;
  }

  Bignum& set_ulong(unsigned long x) {
    bn_assert(BN_set_word(&val, x));
    return *this;
  }

  Bignum& set_long(long x) {
    set_ulong(std::abs(x));
    return x < 0 ? negate() : *this;
  }

  Bignum& negate() {
    BN_set_negative(&val, !BN_is_negative(&val));
    return *this;
  }

  Bignum& operator+=(const Bignum& y) {
    bn_assert(BN_add(&val, &val, &y.val));
    return *this;
  }

  Bignum& operator+=(long y) {
    bn_assert((y >= 0 ? BN_add_word : BN_sub_word)(&val, std::abs(y)));
    return *this;
  }

  Bignum& operator-=(long y) {
    bn_assert((y >= 0 ? BN_sub_word : BN_add_word)(&val, std::abs(y)));
    return *this;
  }

  Bignum& operator*=(const Bignum& y) {
    bn_assert(BN_mul(&val, &val, &y.val, get_ctx()));
    return *this;
  }

  Bignum& operator*=(long y) {
    if (y < 0) {
      negate();
    }
    bn_assert(BN_mul_word(&val, std::abs(y)));
    return *this;
  }

  Bignum& operator<<=(int r) {
    bn_assert(BN_lshift(&val, &val, r));
    return *this;
  }

  Bignum& operator>>=(int r) {
    bn_assert(BN_rshift(&val, &val, r));
    return *this;
  }

  Bignum& operator/=(const Bignum& y) {
    Bignum w;
    bn_assert(BN_div(&val, &w.val, &val, &y.val, get_ctx()));
    return *this;
  }

  Bignum& operator/=(long y) {
    bn_assert(BN_div_word(&val, std::abs(y)) != (BN_ULONG)(-1));
    return y < 0 ? negate() : *this;
  }

  Bignum& operator%=(const Bignum& y) {
    bn_assert(BN_mod(&val, &val, &y.val, get_ctx()));
    return *this;
  }

  Bignum& operator%=(long y) {
    BN_ULONG rem = BN_mod_word(&val, std::abs(y));
    bn_assert(rem != (BN_ULONG)(-1));
    return set_long(y < 0 ? -rem : rem);
  }

  unsigned long divmod(unsigned long y) {
    BN_ULONG rem = BN_div_word(&val, y);
    bn_assert(rem != (BN_ULONG)(-1));
    return rem;
  }

  const Bignum divmod(const Bignum& y);

  std::string to_str() const;
  std::string to_hex() const;
};

inline void bn_assert(int cond) {
  if (!cond) {
    throw Bignum::bignum_error();
  }
}

BN_CTX* get_ctx(void) {
  static BN_CTX* ctx = BN_CTX_new();
  return ctx;
}

BignumBitref& BignumBitref::operator=(bool val) {
  if (val) {
    BN_set_bit(ptr, n);
  } else {
    BN_clear_bit(ptr, n);
  }
  return *this;
}

const Bignum operator+(const Bignum& x, const Bignum& y) {
  Bignum z;
  bn_assert(BN_add(z.bn_ptr(), x.bn_ptr(), y.bn_ptr()));
  return z;
}

const Bignum operator+(const Bignum& x, long y) {
  if (y > 0) {
    Bignum z(x);
    bn_assert(BN_add_word(z.bn_ptr(), y));
    return z;
  } else if (y < 0) {
    Bignum z(x);
    bn_assert(BN_sub_word(z.bn_ptr(), -y));
    return z;
  } else {
    return x;
  }
}

/*
  const Bignum operator+ (Bignum&& x, long y) {
    if (y > 0) {
      bn_assert (BN_add_word (x.bn_ptr(), y));
    } else if (y < 0) {
      bn_assert (BN_sub_word (x.bn_ptr(), -y));
    }
    return std::move (x);
  }
  */

const Bignum operator+(long y, const Bignum& x) {
  return x + y;
}

/*
  const Bignum operator+ (long y, Bignum&& x) {
    return x + y;
  }
  */

const Bignum operator-(const Bignum& x, const Bignum& y) {
  Bignum z;
  bn_assert(BN_sub(z.bn_ptr(), x.bn_ptr(), y.bn_ptr()));
  return z;
}

const Bignum operator-(const Bignum& x, long y) {
  return x + (-y);
}

/*
  const Bignum operator- (Bignum&& x, long y) {
    return x + (-y);
  }
  */

const Bignum operator*(const Bignum& x, const Bignum& y) {
  Bignum z;
  bn_assert(BN_mul(z.bn_ptr(), x.bn_ptr(), y.bn_ptr(), get_ctx()));
  return z;
}

const Bignum operator*(const Bignum& x, long y) {
  if (y > 0) {
    Bignum z(x);
    bn_assert(BN_mul_word(z.bn_ptr(), y));
    return z;
  } else if (y < 0) {
    Bignum z(x);
    z.negate();
    bn_assert(BN_mul_word(z.bn_ptr(), -y));
    return z;
  } else {
    Bignum z(0);
    return z;
  }
}

/*
  const Bignum operator* (Bignum&& x, long y) {
    if (y > 0) {
      bn_assert (BN_mul_word (x.bn_ptr(), y));
    } else if (y < 0) {
      x.negate();
      bn_assert (BN_mul_word (x.bn_ptr(), -y));
    } else {
      x = 0;
    }
    return std::move (x);
  }
  */

const Bignum operator*(long y, const Bignum& x) {
  return x * y;
}

const Bignum operator/(const Bignum& x, const Bignum& y) {
  Bignum z, w;
  bn_assert(BN_div(z.bn_ptr(), w.bn_ptr(), x.bn_ptr(), y.bn_ptr(), get_ctx()));
  return z;
}

const Bignum Bignum::divmod(const Bignum& y) {
  Bignum w;
  bn_assert(BN_div(&val, w.bn_ptr(), &val, y.bn_ptr(), get_ctx()));
  return w;
}

const Bignum operator%(const Bignum& x, const Bignum& y) {
  Bignum z;
  bn_assert(BN_mod(z.bn_ptr(), x.bn_ptr(), y.bn_ptr(), get_ctx()));
  return z;
}

unsigned long operator%(const Bignum& x, unsigned long y) {
  BN_ULONG rem = BN_mod_word(x.bn_ptr(), y);
  bn_assert(rem != (BN_ULONG)(-1));
  return rem;
}

const Bignum operator<<(const Bignum& x, int r) {
  Bignum z;
  bn_assert(BN_lshift(z.bn_ptr(), x.bn_ptr(), r));
  return z;
}

const Bignum operator>>(const Bignum& x, int r) {
  Bignum z;
  bn_assert(BN_rshift(z.bn_ptr(), x.bn_ptr(), r));
  return z;
}

const Bignum abs(const Bignum& x) {
  Bignum T(x);
  if (T.sign() < 0) {
    T.negate();
  }
  return T;
}

const Bignum sqr(const Bignum& x) {
  Bignum z;
  bn_assert(BN_sqr(z.bn_ptr(), x.bn_ptr(), get_ctx()));
  return z;
}

void Bignum::export_msb(unsigned char* buffer, std::size_t size) const {
  bn_assert(size >= 0 && size <= (1 << 20));
  bn_assert(sign() >= 0);
  int n = BN_num_bytes(&val);
  bn_assert(n >= 0 && (unsigned)n <= size);
  bn_assert(BN_bn2bin(&val, buffer + size - n) == n);
  std::memset(buffer, 0, size - n);
}

Bignum& Bignum::import_msb(const unsigned char* buffer, std::size_t size) {
  bn_assert(size >= 0 && size <= (1 << 20));
  std::size_t i = 0;
  while (i < size && !buffer[i]) {
    i++;
  }
  bn_assert(BN_bin2bn(buffer + i, size - i, &val) == &val);
  return *this;
}

void Bignum::export_lsb(unsigned char* buffer, std::size_t size) const {
  bn_assert(size >= 0 && size <= (1 << 20));
  bn_assert(sign() >= 0);
  std::size_t n = BN_num_bytes(&val);
  bn_assert(n >= 0 && (unsigned)n <= size);
  bn_assert(BN_bn2bin(&val, buffer) == (int)n);
  std::memset(buffer + n, 0, size - n);
  for (std::size_t i = 0; 2 * i + 1 < n; i++) {
    std::swap(buffer[i], buffer[n - 1 - i]);
  }
}

Bignum& Bignum::import_lsb(const unsigned char* buffer, std::size_t size) {
  bn_assert(size >= 0 && size <= (1 << 20));
  while (size > 0 && !buffer[size - 1]) {
    size--;
  }
  if (!size) {
    bn_assert(BN_zero(&val));
    return *this;
  }
  unsigned char tmp[size], *ptr = tmp + size;
  for (std::size_t i = 0; i < size; i++) {
    *--ptr = buffer[i];
  }
  bn_assert(BN_bin2bn(tmp, size, &val) == &val);
  return *this;
}

int cmp(const Bignum& x, const Bignum& y) {
  return BN_cmp(x.bn_ptr(), y.bn_ptr());
}

bool operator==(const Bignum& x, const Bignum& y) {
  return cmp(x, y) == 0;
}

bool operator!=(const Bignum& x, const Bignum& y) {
  return cmp(x, y) != 0;
}

bool operator<(const Bignum& x, const Bignum& y) {
  return cmp(x, y) < 0;
}

bool operator<=(const Bignum& x, const Bignum& y) {
  return cmp(x, y) <= 0;
}

bool operator>(const Bignum& x, const Bignum& y) {
  return cmp(x, y) > 0;
}

bool operator>=(const Bignum& x, const Bignum& y) {
  return cmp(x, y) >= 0;
}

bool operator==(const Bignum& x, long y) {
  if (y >= 0) {
    return BN_is_word(x.bn_ptr(), y);
  } else {
    return x == Bignum(y);
  }
}

bool operator!=(const Bignum& x, long y) {
  if (y >= 0) {
    return !BN_is_word(x.bn_ptr(), y);
  } else {
    return x != Bignum(y);
  }
}

std::string Bignum::to_str() const {
  char* ptr = BN_bn2dec(&val);
  std::string z(ptr);
  OPENSSL_free(ptr);
  return z;
}

std::string Bignum::to_hex() const {
  char* ptr = BN_bn2hex(&val);
  std::string z(ptr);
  OPENSSL_free(ptr);
  return z;
}

std::ostream& operator<<(std::ostream& os, const Bignum& x) {
  return os << x.to_str();
}

std::istream& operator>>(std::istream& is, Bignum& x) {
  std::string word;
  is >> word;
  x = dec_string(word);
  return is;
}

bool is_prime(const Bignum& p, int nchecks = 64, bool trial_div = true) {
  return BN_is_prime_fasttest_ex(p.bn_ptr(), BN_prime_checks, get_ctx(), trial_div, 0);
}
}  // namespace arith

namespace arith {
using namespace openssl;

class Residue;
class ResidueRing;

class ResidueRing {
 public:
  struct bad_modulus {};
  struct elem_cnt_mismatch {
    int cnt;
    elem_cnt_mismatch(int x) : cnt(x) {
    }
  };

 private:
  const Bignum modulus;
  mutable int cnt;
  bool prime;
  void cnt_assert(bool b) {
    if (!b) {
      throw elem_cnt_mismatch(cnt);
    }
  }
  Residue* Zero;
  Residue* One;
  Residue* Img_i;
  void init();

 public:
  typedef Residue element;
  explicit ResidueRing(Bignum mod) : modulus(mod), cnt(0), prime(arith::is_prime(mod)), Zero(0), One(0) {
    init();
  }
  ~ResidueRing();
  int incr_count() {
    return ++cnt;
  }
  int decr_count() {
    --cnt;
    cnt_assert(cnt >= 0);
    return cnt;
  }
  const Bignum& get_modulus() const {
    return modulus;
  }
  bool is_prime() const {
    return prime;
  }
  const Residue& zero() const {
    return *Zero;
  }
  const Residue& one() const {
    return *One;
  }
  const Residue& img_i();
  Residue frac(long num, long denom = 1);
  Residue convert(long num);
  Residue convert(const Bignum& x);

  Bignum reduce(const Bignum& x) {
    Bignum r = x % modulus;
    if (r.sign() < 0) {
      r += modulus;
    }
    return r;
  }

  Bignum& do_reduce(Bignum& x) {
    x %= modulus;
    if (x.sign() < 0) {
      x += modulus;
    }
    return x;
  }
};

class Residue {
 public:
  struct not_same_ring {};

 private:
  ResidueRing* ring;
  mutable Bignum val;
  Residue& reduce() {
    ring->do_reduce(val);
    return *this;
  }

 public:
  explicit Residue(ResidueRing& R) : ring(&R) {
    R.incr_count();
  }
  Residue(const Bignum& x, ResidueRing& R) : ring(&R), val(R.reduce(x)) {
    R.incr_count();
  }
  ~Residue() {
    ring->decr_count();
    ring = 0;
  }
  Residue(const Residue& x) : ring(x.ring), val(x.val) {
    ring->incr_count();
  }
  Bignum extract() const {
    return val;
  }
  const Bignum& extract_raw() const {
    return val;
  }
  const Bignum& modulus() const {
    return ring->get_modulus();
  }
  void same_ring(const Residue& y) const {
    if (ring != y.ring) {
      throw not_same_ring();
    }
  }
  ResidueRing& ring_of() const {
    return *ring;
  }
  bool is_zero() const {
    return (val == 0);
  }
  Residue& operator=(const Residue& x) {
    same_ring(x);
    val = x.val;
    return *this;
  }
  Residue& operator=(const Bignum& x) {
    val = ring->reduce(x);
    return *this;
  }
  Residue& operator+=(const Residue& y);
  Residue& operator-=(const Residue& y);
  Residue& operator*=(const Residue& y);
  Residue& operator+=(long y) {
    val += y;
    return reduce();
  }
  Residue& operator-=(long y) {
    val -= y;
    return reduce();
  }
  Residue& operator*=(long y) {
    val *= y;
    return reduce();
  }
  Residue& negate() {
    val.negate();
    return reduce();
  }
  friend const Residue operator+(const Residue& x, const Residue& y);
  friend const Residue operator-(const Residue& x, const Residue& y);
  friend const Residue operator*(const Residue& x, const Residue& y);
  friend const Residue operator-(const Residue& x);
  friend Residue sqr(const Residue& x);
  friend Residue power(const Residue& x, const Bignum& y);
  friend Residue inverse(const Residue& x);
  std::string to_str() const;
};

void ResidueRing::init() {
  Zero = new Residue(0, *this);
  One = new Residue(1, *this);
}

ResidueRing::~ResidueRing() {
  delete Zero;
  delete One;
  Zero = One = 0;
  cnt_assert(!cnt);
}

const Residue operator+(const Residue& x, const Residue& y) {
  x.same_ring(y);
  Residue z(x.ring_of());
  bn_assert(BN_mod_add(z.val.bn_ptr(), x.val.bn_ptr(), y.val.bn_ptr(), x.modulus().bn_ptr(), get_ctx()));
  return z;
}

const Residue operator-(const Residue& x, const Residue& y) {
  x.same_ring(y);
  Residue z(x.ring_of());
  bn_assert(BN_mod_sub(z.val.bn_ptr(), x.val.bn_ptr(), y.val.bn_ptr(), x.modulus().bn_ptr(), get_ctx()));
  return z;
}

const Residue operator*(const Residue& x, const Residue& y) {
  x.same_ring(y);
  Residue z(x.ring_of());
  bn_assert(BN_mod_mul(z.val.bn_ptr(), x.val.bn_ptr(), y.val.bn_ptr(), x.modulus().bn_ptr(), get_ctx()));
  return z;
}

const Residue operator-(const Residue& x) {
  Residue z(x);
  z.val.negate();
  return z.reduce();
}

Residue& Residue::operator+=(const Residue& y) {
  same_ring(y);
  bn_assert(BN_mod_add(val.bn_ptr(), val.bn_ptr(), y.val.bn_ptr(), modulus().bn_ptr(), get_ctx()));
  return *this;
}

Residue& Residue::operator-=(const Residue& y) {
  same_ring(y);
  bn_assert(BN_mod_sub(val.bn_ptr(), val.bn_ptr(), y.val.bn_ptr(), modulus().bn_ptr(), get_ctx()));
  return *this;
}

Residue& Residue::operator*=(const Residue& y) {
  same_ring(y);
  bn_assert(BN_mod_mul(val.bn_ptr(), val.bn_ptr(), y.val.bn_ptr(), modulus().bn_ptr(), get_ctx()));
  return *this;
}

bool operator==(const Residue& x, const Residue& y) {
  x.same_ring(y);
  return x.extract() == y.extract();
}

bool operator!=(const Residue& x, const Residue& y) {
  x.same_ring(y);
  return x.extract() != y.extract();
}

Residue sqr(const Residue& x) {
  Residue z(x.ring_of());
  bn_assert(BN_mod_sqr(z.val.bn_ptr(), x.val.bn_ptr(), x.modulus().bn_ptr(), get_ctx()));
  return z;
}

Residue power(const Residue& x, const Bignum& y) {
  Residue z(x.ring_of());
  bn_assert(BN_mod_exp(z.val.bn_ptr(), x.val.bn_ptr(), y.bn_ptr(), x.modulus().bn_ptr(), get_ctx()));
  return z;
}

Residue inverse(const Residue& x) {
  assert(x.ring_of().is_prime());
  return power(x, x.ring_of().get_modulus() - 2);
}

const Residue& ResidueRing::img_i() {
  if (!Img_i) {
    assert(is_prime());
    assert(modulus % 4 == 1);
    int g = 2;
    Bignum n = (modulus - 1) / 4;
    while (true) {
      Residue t = power(frac(g), n);
      if (t != one() && t != frac(-1)) {
        Img_i = new Residue(t);
        break;
      }
    }
  }
  return *Img_i;
}

Residue sqrt(const Residue& x) {
  assert(x.ring_of().is_prime());
  ResidueRing& R = x.ring_of();
  const Bignum& p = R.get_modulus();
  if (x.is_zero() || !p.odd()) {
    return x;
  }
  if (p[1]) {  // p=3 (mod 4)
    return power(x, (p + 1) >> 2);
  } else if (p[2]) {
    // p=5 (mod 8)
    Residue t = power(x, (p + 3) >> 3);
    return (sqr(t) == x) ? t : R.img_i() * t;
  } else {
    assert(p[2]);
    return R.zero();
  }
}

Residue ResidueRing::frac(long num, long denom) {
  assert(denom);
  if (denom < 0) {
    num = -num;
    denom = -denom;
  }
  if (!(num % denom)) {
    return Residue(num / denom, *this);
  } else {
    return Residue(num, *this) * inverse(Residue(denom, *this));
  }
}

inline Residue ResidueRing::convert(long x) {
  return Residue(x, *this);
}

inline Residue ResidueRing::convert(const Bignum& x) {
  return Residue(x, *this);
}

std::string Residue::to_str() const {
  return "Mod(" + val.to_str() + "," + modulus().to_str() + ")";
}

std::ostream& operator<<(std::ostream& os, const Residue& x) {
  return os << x.to_str();
}

std::istream& operator>>(std::istream& is, Residue& x) {
  std::string word;
  is >> word;
  x = dec_string(word);
  return is;
}
}  // namespace arith

// ******************************************************

namespace ellcurve {
using namespace arith;

const Bignum& P25519() {
  static Bignum P25519 = (Bignum(1) << 255) - 19;
  return P25519;
}

ResidueRing& Fp25519() {
  static ResidueRing Fp25519(P25519());
  return Fp25519;
}
}  // namespace ellcurve

// ******************************************************

namespace ellcurve {
using namespace arith;

class MontgomeryCurve {
  ResidueRing& ring;
  int A_short;   // v^2 = u^2 + Au + 1
  int Gu_short;  // u(G)
  int a_short;   // (A+2)/4
  Residue A;
  Residue Gu;
  Bignum P;
  Bignum L;
  Bignum Order;
  Bignum cofactor;
  int cofactor_short;

  void init();

 public:
  MontgomeryCurve(int _A, int _Gu, ResidueRing& _R)
      : ring(_R)
      , A_short(_A)
      , Gu_short(_Gu)
      , a_short((_A + 2) / 4)
      , A(_A, _R)
      , Gu(_Gu, _R)
      , P(_R.get_modulus())
      , cofactor_short(0) {
    init();
  }

  const Residue& get_gen_u() const {
    return Gu;
  }
  const Bignum& get_ell() const {
    return L;
  }
  const Bignum& get_order() const {
    return Order;
  }
  ResidueRing& get_base_ring() const {
    return ring;
  }
  const Bignum& get_p() const {
    return P;
  }

  void set_order_cofactor(const Bignum& order, int cof);

  struct PointXZ {
    Residue X, Z;
    PointXZ(Residue x, Residue z) : X(x), Z(z) {
      x.same_ring(z);
    }
    PointXZ(ResidueRing& r) : X(r.one()), Z(r.zero()) {
    }
    explicit PointXZ(Residue u) : X(u), Z(u.ring_of().one()) {
    }
    explicit PointXZ(Residue y, bool) : X(y.ring_of().one() - y), Z(y + y.ring_of().one()) {
    }
    PointXZ(const PointXZ& P) : X(P.X), Z(P.Z) {
    }
    PointXZ& operator=(const PointXZ& P) {
      X = P.X;
      Z = P.Z;
      return *this;
    }
    Residue get_u() const {
      return X * inverse(Z);
    }
    Residue get_v(bool sign_v = false) const;
    bool is_infty() const {
      return Z.is_zero();
    }
    Residue get_y() const {
      return (X - Z) * inverse(X + Z);
    }
    bool export_point_y(unsigned char buffer[32]) const;
    bool export_point_u(unsigned char buffer[32]) const;
    void zeroize() {
      X = Z = Z.ring_of().zero();
    }
  };

  PointXZ power_gen_xz(const Bignum& n) const;
  PointXZ power_xz(const Residue& u, const Bignum& n) const;
  PointXZ power_xz(const PointXZ& P, const Bignum& n) const;
  PointXZ add_xz(const PointXZ& P, const PointXZ& Q) const;
  PointXZ double_xz(const PointXZ& P) const;

  PointXZ import_point_u(const unsigned char point[32]) const;
  PointXZ import_point_y(const unsigned char point[32]) const;
};

void MontgomeryCurve::init() {
  assert(!((a_short + 2) & 3) && a_short >= 0);
}

void MontgomeryCurve::set_order_cofactor(const Bignum& order, int cof) {
  assert(order > 0);
  assert(cof >= 0);
  assert(cof == 0 || (order % cof) == 0);
  Order = order;
  cofactor = cofactor_short = cof;
  if (cof > 0) {
    L = order / cof;
    assert(is_prime(L));
  }
  assert(!power_gen_xz(1).is_infty());
  assert(power_gen_xz(Order).is_infty());
}

// computes u(P+Q)*u(P-Q) as X/Z
MontgomeryCurve::PointXZ MontgomeryCurve::add_xz(const MontgomeryCurve::PointXZ& P,
                                                 const MontgomeryCurve::PointXZ& Q) const {
  Residue u = (P.X + P.Z) * (Q.X - Q.Z);
  Residue v = (P.X - P.Z) * (Q.X + Q.Z);
  return MontgomeryCurve::PointXZ(sqr(u + v), sqr(u - v));
}

// computes u(2P) as X/Z
MontgomeryCurve::PointXZ MontgomeryCurve::double_xz(const MontgomeryCurve::PointXZ& P) const {
  Residue u = sqr(P.X + P.Z);
  Residue v = sqr(P.X - P.Z);
  Residue w = u - v;
  return PointXZ(u * v, w * (v + Residue(a_short, ring) * w));
}

MontgomeryCurve::PointXZ MontgomeryCurve::power_gen_xz(const Bignum& n) const {
  return power_xz(Gu, n);
}

MontgomeryCurve::PointXZ MontgomeryCurve::power_xz(const Residue& u, const Bignum& n) const {
  return power_xz(PointXZ(u), n);
}

// computes u([n]P) in form X/Z
MontgomeryCurve::PointXZ MontgomeryCurve::power_xz(const PointXZ& A, const Bignum& n) const {
  assert(n >= 0);
  if (n == 0) {
    return PointXZ(ring);
  }

  int k = n.num_bits();
  PointXZ P(A);
  PointXZ Q(double_xz(P));
  for (int i = k - 2; i >= 0; --i) {
    PointXZ PQ(add_xz(P, Q));
    PQ.X *= A.Z;
    PQ.Z *= A.X;
    if (n[i]) {
      P = PQ;
      Q = double_xz(Q);
    } else {
      Q = PQ;
      P = double_xz(P);
    }
  }
  return P;
}

bool MontgomeryCurve::PointXZ::export_point_y(unsigned char buffer[32]) const {
  if ((X + Z).is_zero()) {
    std::memset(buffer, 0xff, 32);
    return false;
  } else {
    get_y().extract().export_lsb(buffer, 32);
    return true;
  }
}

bool MontgomeryCurve::PointXZ::export_point_u(unsigned char buffer[32]) const {
  if (Z.is_zero()) {
    std::memset(buffer, 0xff, 32);
    return false;
  } else {
    get_u().extract().export_lsb(buffer, 32);
    return true;
  }
}

MontgomeryCurve::PointXZ MontgomeryCurve::import_point_u(const unsigned char point[32]) const {
  Bignum u;
  u.import_lsb(point, 32);
  u[255] = 0;
  return PointXZ(Residue(u, ring));
}

MontgomeryCurve::PointXZ MontgomeryCurve::import_point_y(const unsigned char point[32]) const {
  Bignum y;
  y.import_lsb(point, 32);
  y[255] = 0;
  return PointXZ(Residue(y, ring), true);
}

MontgomeryCurve& Curve25519() {
  static MontgomeryCurve Curve25519(486662, 9, Fp25519());
  static bool init = false;
  if (!init) {
    Curve25519.set_order_cofactor(hex_string{"80000000000000000000000000000000a6f7cef517bce6b2c09318d2e7ae9f68"}, 8);
    init = true;
  }
  return Curve25519;
}
}  // namespace ellcurve

// ******************************************************

namespace ellcurve {
using namespace arith;

class TwEdwardsCurve;

class TwEdwardsCurve {
 public:
  struct SegrePoint {
    Residue XY, X, Y, Z;  // if x=X/Z and y=Y/T, stores (xy,x,y,1)*Z*T
    SegrePoint(ResidueRing& R) : XY(R), X(R), Y(R), Z(R) {
    }
    SegrePoint(const Residue& x, const Residue& y) : XY(x * y), X(x), Y(y), Z(y.ring_of().one()) {
    }
    SegrePoint(const TwEdwardsCurve& E, const Residue& y, bool x_sign);
    SegrePoint(const SegrePoint& P) : XY(P.XY), X(P.X), Y(P.Y), Z(P.Z) {
    }
    SegrePoint& operator=(const SegrePoint& P) {
      XY = P.XY;
      X = P.X;
      Y = P.Y;
      Z = P.Z;
      return *this;
    }
    bool is_zero() const {
      return X.is_zero() && (Y == Z);
    }
    bool is_valid() const {
      return (XY * Z == X * Y) && !(XY.is_zero() && X.is_zero() && Y.is_zero() && Z.is_zero());
    }
    bool is_finite() const {
      return !Z.is_zero();
    }
    bool is_normalized() const {
      return Z == Z.ring_of().one();
    }
    SegrePoint& normalize() {
      auto f = inverse(Z);
      XY *= f;
      X *= f;
      Y *= f;
      Z = Z.ring_of().one();
      return *this;
    }
    SegrePoint& zeroize() {
      XY = X = Y = Z = Z.ring_of().zero();
      return *this;
    }
    bool export_point(unsigned char buffer[32], bool need_x = true) const;
    bool export_point_y(unsigned char buffer[32]) const {
      return export_point(buffer, false);
    }
    bool export_point_u(unsigned char buffer[32]) const;
    Residue get_y() const {
      return Y * inverse(Z);
    }
    Residue get_x() const {
      return X * inverse(Z);
    }
    Residue get_u() const {
      return (Z + Y) * inverse(Z - Y);
    }
    void negate() {
      XY.negate();
      X.negate();
    }
  };

 private:
  ResidueRing& ring;
  Residue D;
  Residue D2;
  Residue Gy;
  Bignum P;
  Bignum L;
  Bignum Order;
  Bignum cofactor;
  int cofactor_short;
  SegrePoint G;
  SegrePoint O;
  void init();

 public:
  TwEdwardsCurve(const Residue& _D, const Residue& _Gy, ResidueRing& _R)
      : ring(_R), D(_D), D2(_D + _D), Gy(_Gy), P(_R.get_modulus()), cofactor_short(0), G(_R), O(_R) {
    init();
  }

  const Residue& get_gen_y() const {
    return Gy;
  }
  const Bignum& get_ell() const {
    return L;
  }
  const Bignum& get_order() const {
    return Order;
  }
  ResidueRing& get_base_ring() const {
    return ring;
  }
  const Bignum& get_p() const {
    return P;
  }
  const SegrePoint& get_base_point() const {
    return G;
  }

  void set_order_cofactor(const Bignum& order, int cof);
  bool recover_x(Residue& x, const Residue& y, bool x_sign) const;

  void add_points(SegrePoint& R, const SegrePoint& P, const SegrePoint& Q) const;
  SegrePoint add_points(const SegrePoint& P, const SegrePoint& Q) const;
  void double_point(SegrePoint& R, const SegrePoint& P) const;
  SegrePoint double_point(const SegrePoint& P) const;
  SegrePoint power_point(const SegrePoint& A, const Bignum& n) const;
  SegrePoint power_gen(const Bignum& n) const;

  SegrePoint import_point(const unsigned char point[32], bool& ok) const;
};

std::ostream& operator<<(std::ostream& os, const TwEdwardsCurve::SegrePoint& P) {
  return os << "[" << P.XY << ":" << P.X << ":" << P.Y << ":" << P.Z << "]";
}

void TwEdwardsCurve::init() {
  assert(D != ring.zero() && D != ring.convert(-1));
  O.X = O.Z = ring.one();
  G = SegrePoint(*this, Gy, 0);
  assert(!G.XY.is_zero());
}

void TwEdwardsCurve::set_order_cofactor(const Bignum& order, int cof) {
  assert(order > 0);
  assert(cof >= 0);
  assert(cof == 0 || (order % cof) == 0);
  Order = order;
  cofactor = cofactor_short = cof;
  if (cof > 0) {
    L = order / cof;
    assert(is_prime(L));
    assert(!power_gen(1).is_zero());
    assert(power_gen(L).is_zero());
  }
}

TwEdwardsCurve::SegrePoint::SegrePoint(const TwEdwardsCurve& E, const Residue& y, bool x_sign)
    : XY(y), X(E.get_base_ring()), Y(y), Z(E.get_base_ring().one()) {
  Residue x(y.ring_of());
  if (E.recover_x(x, y, x_sign)) {
    XY *= x;
    X = x;
  } else {
    XY = Y = Z = E.get_base_ring().zero();
  }
}

bool TwEdwardsCurve::recover_x(Residue& x, const Residue& y, bool x_sign) const {
  // recovers x from equation -x^2+y^2 = 1+d*x^2*y^2
  Residue z = inverse(ring.one() + D * sqr(y));
  if (z.is_zero()) {
    return false;
  }
  z *= sqr(y) - ring.one();
  Residue t = sqrt(z);
  if (sqr(t) == z) {
    x = (t.extract().odd() == x_sign) ? t : -t;
    //std::cout << "x=" << x << ", y=" << y << std::endl;
    return true;
  } else {
    return false;
  }
}

void TwEdwardsCurve::add_points(SegrePoint& Res, const SegrePoint& P, const SegrePoint& Q) const {
  Residue a((P.X + P.Y) * (Q.X + Q.Y));
  Residue b((P.X - P.Y) * (Q.X - Q.Y));
  Residue c(P.Z * Q.Z * ring.convert(2));
  Residue d(P.XY * Q.XY * D2);
  Residue x_num(a - b);   // 2(x1y2+x2y1)
  Residue y_num(a + b);   // 2(x1x2+y1y2)
  Residue x_den(c + d);   // 2(1+dx1x2y1y2)
  Residue y_den(c - d);   // 2(1-dx1x2y1y2)
  Res.X = x_num * y_den;  // x = x_num/x_den, y = y_num/y_den
  Res.Y = y_num * x_den;
  Res.XY = x_num * y_num;
  Res.Z = x_den * y_den;
}

TwEdwardsCurve::SegrePoint TwEdwardsCurve::add_points(const SegrePoint& P, const SegrePoint& Q) const {
  SegrePoint Res(ring);
  add_points(Res, P, Q);
  return Res;
}

void TwEdwardsCurve::double_point(SegrePoint& Res, const SegrePoint& P) const {
  add_points(Res, P, P);
}

TwEdwardsCurve::SegrePoint TwEdwardsCurve::double_point(const SegrePoint& P) const {
  SegrePoint Res(ring);
  double_point(Res, P);
  return Res;
}

// computes u([n]P) in form (xy,x,y,1)*Z
TwEdwardsCurve::SegrePoint TwEdwardsCurve::power_point(const SegrePoint& A, const Bignum& n) const {
  assert(n >= 0);
  if (n == 0) {
    return O;
  }

  int k = n.num_bits();
  SegrePoint P(A);
  SegrePoint Q(double_point(A));
  for (int i = k - 2; i >= 0; --i) {
    if (n[i]) {
      add_points(P, P, Q);
      double_point(Q, Q);
    } else {
      // we do more operations than necessary for uniformicity
      add_points(Q, P, Q);
      double_point(P, P);
    }
  }
  return P;
}

TwEdwardsCurve::SegrePoint TwEdwardsCurve::power_gen(const Bignum& n) const {
  return power_point(G, n);
}

bool TwEdwardsCurve::SegrePoint::export_point(unsigned char buffer[32], bool need_x) const {
  if (!is_normalized()) {
    if (Z.is_zero()) {
      std::memset(buffer, 0xff, 32);
      return false;
    }
    Residue f(inverse(Z));
    Bignum y((Y * f).extract());
    assert(!y[255]);
    if (need_x) {
      y[255] = (X * f).extract().odd();
    }
    y.export_lsb(buffer, 32);
  } else {
    Bignum y(Y.extract());
    assert(!y[255]);
    if (need_x) {
      y[255] = X.extract().odd();
    }
    y.export_lsb(buffer, 32);
  }
  return true;
}

bool TwEdwardsCurve::SegrePoint::export_point_u(unsigned char buffer[32]) const {
  if (Z == Y) {
    std::memset(buffer, 0xff, 32);
    return false;
  }
  Residue f(inverse(Z - Y));
  ((Z + Y) * f).extract().export_lsb(buffer, 32);
  assert(!(buffer[31] & 0x80));
  return true;
}

TwEdwardsCurve::SegrePoint TwEdwardsCurve::import_point(const unsigned char point[32], bool& ok) const {
  Bignum y;
  y.import_lsb(point, 32);
  bool x_sign = y[255];
  y[255] = 0;
  Residue yr(y, ring);
  Residue xr(ring);
  ok = recover_x(xr, yr, x_sign);
  return ok ? SegrePoint(xr, yr) : SegrePoint(ring);
}

TwEdwardsCurve& Ed25519() {
  static TwEdwardsCurve Ed25519(Fp25519().frac(-121665, 121666), Fp25519().frac(4, 5), Fp25519());
  static bool init = false;
  if (!init) {
    Ed25519.set_order_cofactor(hex_string{"80000000000000000000000000000000a6f7cef517bce6b2c09318d2e7ae9f68"}, 8);
    init = true;
  }
  return Ed25519;
}
}  // namespace ellcurve

// ******************************************************

namespace openssl {
#include <openssl/evp.h>
}

namespace digest {
using namespace openssl;

struct OpensslEVP_SHA1 {
  enum { digest_bytes = 20 };
  static const EVP_MD* get_evp() {
    return EVP_sha1();
  }
};

struct OpensslEVP_SHA256 {
  enum { digest_bytes = 32 };
  static const EVP_MD* get_evp() {
    return EVP_sha256();
  }
};

struct OpensslEVP_SHA512 {
  enum { digest_bytes = 64 };
  static const EVP_MD* get_evp() {
    return EVP_sha512();
  }
};

template <typename H>
class HashCtx {
  EVP_MD_CTX ctx;
  void init();
  void clear();

 public:
  enum { digest_bytes = H::digest_bytes };
  HashCtx() {
    init();
  }
  HashCtx(const void* data, std::size_t len) {
    init();
    feed(data, len);
  }
  ~HashCtx() {
    clear();
  }
  void feed(const void* data, std::size_t len);
  std::size_t extract(unsigned char buffer[digest_bytes]);
  std::string extract();
};

template <typename H>
void HashCtx<H>::init() {
  EVP_MD_CTX_init(&ctx);
  EVP_DigestInit_ex(&ctx, H::get_evp(), 0);
}

template <typename H>
void HashCtx<H>::clear() {
  EVP_MD_CTX_cleanup(&ctx);
}

template <typename H>
void HashCtx<H>::feed(const void* data, std::size_t len) {
  EVP_DigestUpdate(&ctx, data, len);
}

template <typename H>
std::size_t HashCtx<H>::extract(unsigned char buffer[digest_bytes]) {
  unsigned olen = 0;
  EVP_DigestFinal_ex(&ctx, buffer, &olen);
  assert(olen == digest_bytes);
  return olen;
}

template <typename H>
std::string HashCtx<H>::extract() {
  unsigned char buffer[digest_bytes];
  unsigned olen = 0;
  EVP_DigestFinal_ex(&ctx, buffer, &olen);
  assert(olen == digest_bytes);
  return std::string((char*)buffer, olen);
}

typedef HashCtx<OpensslEVP_SHA1> SHA1;
typedef HashCtx<OpensslEVP_SHA256> SHA256;
typedef HashCtx<OpensslEVP_SHA512> SHA512;

template <typename T>
std::size_t hash_str(unsigned char buffer[T::digest_bytes], const void* data, std::size_t size) {
  T hasher(data, size);
  return hasher.extract(buffer);
}

template <typename T>
std::size_t hash_two_str(unsigned char buffer[T::digest_bytes], const void* data1, std::size_t size1, const void* data2,
                         std::size_t size2) {
  T hasher(data1, size1);
  hasher.feed(data2, size2);
  return hasher.extract(buffer);
}

template <typename T>
std::string hash_str(const void* data, std::size_t size) {
  T hasher(data, size);
  return hasher.extract();
}

template <typename T>
std::string hash_two_str(const void* data1, std::size_t size1, const void* data2, std::size_t size2) {
  T hasher(data1, size1);
  hasher.feed(data2, size2);
  return hasher.extract();
}
}  // namespace digest

// ******************************************************

namespace openssl {
#include <openssl/rand.h>
}

#include <fcntl.h>
#include <unistd.h>

namespace prng {

int os_get_random_bytes(void* buf, int n) {
  using namespace std;
  int r = 0, h = open("/dev/random", O_RDONLY | O_NONBLOCK);
  if (h >= 0) {
    r = read(h, buf, n);
    if (r > 0) {
      //std::cerr << "added " << r << " bytes of real entropy to secure random numbers seed" << std::endl;
    } else {
      r = 0;
    }
    close(h);
  }

  if (r < n) {
    h = open("/dev/urandom", O_RDONLY);
    if (h < 0) {
      return r;
    }
    int s = read(h, (char*)buf + r, n - r);
    close(h);
    if (s < 0) {
      return r;
    }
    r += s;
  }

  if (r >= 8) {
    *(long*)buf ^= lrand48();
    srand48(*(long*)buf);
  }

  return r;
}
}  // namespace prng

namespace prng {
using namespace openssl;

class RandomGen {
 public:
  struct rand_error {};
  void randomize(bool force = true);
  void seed_add(const void* data, std::size_t size, double entropy = 0);
  bool ok() const {
    return RAND_status();
  }
  RandomGen() {
    randomize(false);
  }
  RandomGen(const void* seed, std::size_t size) {
    seed_add(seed, size);
    randomize(false);
  }
  bool rand_bytes(void* data, std::size_t size, bool strong = false);
  bool strong_rand_bytes(void* data, std::size_t size) {
    return rand_bytes(data, size, true);
  }
  template <class T>
  bool rand_obj(T& obj) {
    return rand_bytes(&obj, sizeof(T));
  }
  template <class T>
  bool rand_objs(T* ptr, std::size_t count) {
    return rand_bytes(ptr, sizeof(T) * count);
  }
  std::string rand_string(std::size_t size, bool strong = false);
};

void RandomGen::seed_add(const void* data, std::size_t size, double entropy) {
  RAND_add(data, size, entropy > 0 ? entropy : size);
}

void RandomGen::randomize(bool force) {
  if (!force && ok()) {
    return;
  }
  unsigned char buffer[128];
  int n = os_get_random_bytes(buffer, 128);
  seed_add(buffer, n);
  assert(ok());
}

bool RandomGen::rand_bytes(void* data, std::size_t size, bool strong) {
  int res = (strong ? RAND_bytes : RAND_pseudo_bytes)((unsigned char*)data, size);
  if (res != 0 && res != 1) {
    throw rand_error();
  }
  return res;
}

std::string RandomGen::rand_string(std::size_t size, bool strong) {
  char buffer[size];
  if (!rand_bytes(buffer, size, strong)) {
    throw rand_error();
  }
  return std::string(buffer, size);
}

RandomGen& rand_gen() {
  static RandomGen MainPRNG;
  return MainPRNG;
}

}  // namespace prng

// ******************************************************

namespace crypto {
namespace Ed25519 {

const int privkey_bytes = 32;
const int pubkey_bytes = 32;
const int sign_bytes = 64;
const int shared_secret_bytes = 32;

bool all_bytes_same(const unsigned char* str, std::size_t size) {
  unsigned char c = str[0];
  for (std::size_t i = 0; i < size; i++) {
    if (str[i] != c) {
      return false;
    }
  }
  return true;
}

class PublicKey {
  enum { pk_empty, pk_xz, pk_init } inited;
  unsigned char pubkey[pubkey_bytes];
  ellcurve::TwEdwardsCurve::SegrePoint PubKey;
  ellcurve::MontgomeryCurve::PointXZ PubKey_xz;

 public:
  PublicKey() : inited(pk_empty), PubKey(ellcurve::Fp25519()), PubKey_xz(ellcurve::Fp25519()) {
  }
  PublicKey(const unsigned char pub_key[pubkey_bytes]);
  PublicKey(const ellcurve::TwEdwardsCurve::SegrePoint& Pub_Key);

  bool import_public_key(const unsigned char pub_key[pubkey_bytes]);
  bool import_public_key(const ellcurve::TwEdwardsCurve::SegrePoint& Pub_Key);
  bool export_public_key(unsigned char pubkey_buffer[pubkey_bytes]) const;
  bool check_message_signature(unsigned char signature[sign_bytes], const unsigned char* message, std::size_t msg_size);

  void clear();
  bool ok() const {
    return inited == pk_init;
  }

  const unsigned char* get_pubkey_ptr() const {
    return inited == pk_init ? pubkey : 0;
  }
  const ellcurve::TwEdwardsCurve::SegrePoint& get_point() const {
    return PubKey;
  }
  const ellcurve::MontgomeryCurve::PointXZ& get_point_xz() const {
    return PubKey_xz;
  }
};

void PublicKey::clear(void) {
  if (inited != pk_empty) {
    std::memset(pubkey, 0, pubkey_bytes);
    PubKey.zeroize();
    PubKey_xz.zeroize();
  }
  inited = pk_empty;
}

PublicKey::PublicKey(const unsigned char pub_key[pubkey_bytes])
    : inited(pk_empty), PubKey(ellcurve::Fp25519()), PubKey_xz(ellcurve::Fp25519()) {
  import_public_key(pub_key);
}

PublicKey::PublicKey(const ellcurve::TwEdwardsCurve::SegrePoint& Pub_Key)
    : inited(pk_empty), PubKey(ellcurve::Fp25519()), PubKey_xz(ellcurve::Fp25519()) {
  import_public_key(Pub_Key);
}

bool PublicKey::import_public_key(const unsigned char pub_key[pubkey_bytes]) {
  clear();
  if (all_bytes_same(pub_key, pubkey_bytes)) {
    return false;
  }
  bool ok = false;
  PubKey = ellcurve::Ed25519().import_point(pub_key, ok);
  if (!ok) {
    clear();
    return false;
  }
  std::memcpy(pubkey, pub_key, pubkey_bytes);
  PubKey_xz.X = PubKey.Z + PubKey.Y;
  PubKey_xz.Z = PubKey.Z - PubKey.Y;
  inited = pk_init;
  return true;
}

bool PublicKey::import_public_key(const ellcurve::TwEdwardsCurve::SegrePoint& Pub_Key) {
  clear();
  if (!Pub_Key.is_valid()) {
    return false;
  }
  PubKey = Pub_Key;
  PubKey_xz.X = PubKey.Z + PubKey.Y;
  PubKey_xz.Z = PubKey.Z - PubKey.Y;
  inited = pk_init;

  if (!PubKey.export_point(pubkey)) {
    clear();
    return false;
  }
  return true;
}

bool PublicKey::export_public_key(unsigned char pubkey_buffer[pubkey_bytes]) const {
  if (inited != pk_init) {
    std::memset(pubkey_buffer, 0, pubkey_bytes);
    return false;
  } else {
    std::memcpy(pubkey_buffer, pubkey, pubkey_bytes);
    return true;
  }
}

bool PublicKey::check_message_signature(unsigned char signature[sign_bytes], const unsigned char* message,
                                        std::size_t msg_size) {
  if (inited != pk_init) {
    return false;
  }
  unsigned char hash[64];
  {
    digest::SHA512 hasher(signature, 32);
    hasher.feed(pubkey, 32);
    hasher.feed(message, msg_size);
    hasher.extract(hash);
  }
  auto& E = ellcurve::Ed25519();
  const arith::Bignum& L = E.get_ell();
  arith::Bignum H, S;
  S.import_lsb(signature + 32, 32);
  H.import_lsb(hash, 64);
  H %= L;
  H = L - H;
  auto sG = E.power_gen(S);
  auto hA = E.power_point(PubKey, H);
  auto pR1 = E.add_points(sG, hA);
  unsigned char pR1_bytes[32];
  if (!pR1.export_point(pR1_bytes)) {
    return false;
  }
  return !std::memcmp(pR1_bytes, signature, 32);
}

class PrivateKey {
 public:
  struct priv_key_no_copy {};
  PrivateKey() : inited(false) {
    std::memset(privkey, 0, privkey_bytes);
  }
  PrivateKey(const unsigned char pk[privkey_bytes]) : inited(false) {
    std::memset(privkey, 0, privkey_bytes);
    import_private_key(pk);
  }
  ~PrivateKey() {
    clear();
  }
  bool random_private_key(bool strong = false);
  bool import_private_key(const unsigned char pk[privkey_bytes]);
  bool export_private_key(unsigned char pk[privkey_bytes]) const;  // careful!
  bool export_public_key(unsigned char pubk[pubkey_bytes]) const {
    return PubKey.export_public_key(pubk);
  }
  void clear();
  bool ok() const {
    return inited;
  }

  // used for EdDSA (sign)
  bool sign_message(unsigned char signature[sign_bytes], const unsigned char* message, std::size_t msg_size);
  // used for ECDH (encrypt / decrypt)
  bool compute_shared_secret(unsigned char secret[shared_secret_bytes], const PublicKey& Pub);
  // used for EC asymmetric decryption
  bool compute_temp_shared_secret(unsigned char secret[shared_secret_bytes],
                                  const unsigned char temp_pub_key[pubkey_bytes]);

  const PublicKey& get_public_key() const {
    return PubKey;
  }

 private:
  bool inited;
  unsigned char privkey[privkey_bytes];
  unsigned char priv_salt[32];
  arith::Bignum priv_exp;
  PublicKey PubKey;

  bool process_private_key();
  PrivateKey(const PrivateKey&) {
    throw priv_key_no_copy();
  }
  PrivateKey& operator=(const PrivateKey&) {
    throw priv_key_no_copy();
  }
};

bool PrivateKey::random_private_key(bool strong) {
  inited = false;
  if (!prng::rand_gen().rand_bytes(privkey, privkey_bytes, strong)) {
    clear();
    return false;
  }
  return process_private_key();
}

void PrivateKey::clear(void) {
  std::memset(privkey, 0, privkey_bytes);
  std::memset(priv_salt, 0, sizeof(priv_salt));
  priv_exp.clear();
  PubKey.clear();
  inited = false;
}

bool PrivateKey::import_private_key(const unsigned char pk[privkey_bytes]) {
  clear();
  if (all_bytes_same(pk, privkey_bytes)) {
    return false;
  }
  std::memcpy(privkey, pk, privkey_bytes);
  return process_private_key();
}

bool PrivateKey::export_private_key(unsigned char pk[privkey_bytes]) const {  // careful!
  if (!inited) {
    std::memset(pk, 0, privkey_bytes);
    return false;
  } else {
    std::memcpy(pk, privkey, privkey_bytes);
    return true;
  }
}

bool PrivateKey::process_private_key() {
  unsigned char buff[64];
  digest::hash_str<digest::SHA512>(buff, privkey, privkey_bytes);
  std::memcpy(priv_salt, buff + 32, 32);
  buff[0] &= -8;
  buff[31] = ((buff[31] | 0x40) & ~0x80);
  priv_exp.import_lsb(buff, 32);
  PubKey = ellcurve::Ed25519().power_gen(priv_exp);
  inited = PubKey.ok();
  if (!inited) {
    clear();
  }
  return inited;
}

bool PrivateKey::compute_shared_secret(unsigned char secret[shared_secret_bytes], const PublicKey& Pub) {
  if (!inited || !Pub.ok()) {
    std::memset(secret, 0, shared_secret_bytes);
    *(long*)secret = lrand48();
    return false;
  }
  auto P = ellcurve::Curve25519().power_xz(Pub.get_point_xz(), priv_exp);
  if (P.is_infty()) {
    std::memset(secret, 0, shared_secret_bytes);
    *(long*)secret = lrand48();
    return false;
  }
  P.export_point_y(secret);
  return true;
}

bool PrivateKey::compute_temp_shared_secret(unsigned char secret[shared_secret_bytes],
                                            const unsigned char temp_pub_key[pubkey_bytes]) {
  PublicKey tempPubkey(temp_pub_key);
  if (!tempPubkey.ok()) {
    return false;
  }
  return compute_shared_secret(secret, tempPubkey);
}

bool PrivateKey::sign_message(unsigned char signature[sign_bytes], const unsigned char* message, std::size_t msg_size) {
  if (!inited) {
    std::memset(signature, 0, sign_bytes);
    return false;
  }
  unsigned char r_bytes[64];
  digest::hash_two_str<digest::SHA512>(r_bytes, priv_salt, 32, message, msg_size);
  const arith::Bignum& L = ellcurve::Ed25519().get_ell();
  arith::Bignum eR;
  eR.import_lsb(r_bytes, 64);
  eR %= L;

  auto pR = ellcurve::Ed25519().power_gen(eR);

  assert(pR.export_point(signature, 32));
  {
    digest::SHA512 hasher(signature, 32);
    hasher.feed(PubKey.get_pubkey_ptr(), 32);
    hasher.feed(message, msg_size);
    hasher.extract(r_bytes);
  }
  arith::Bignum S;
  S.import_lsb(r_bytes, 64);
  S %= L;
  S *= priv_exp;
  S += eR;
  S %= L;
  S.export_lsb(signature + 32, 32);
  return true;
}

// use one TempKeyGenerator object a lot of times
class TempKeyGenerator {
  enum { salt_size = 64 };
  unsigned char random_salt[salt_size];
  unsigned char buffer[privkey_bytes];

 public:
  TempKeyGenerator() {
    prng::rand_gen().strong_rand_bytes(random_salt, salt_size);
  }
  ~TempKeyGenerator() {
    std::memset(random_salt, 0, salt_size);
    std::memset(buffer, 0, privkey_bytes);
  }

  unsigned char* get_temp_private_key(unsigned char* to, const unsigned char* message, std::size_t size,
                                      const unsigned char* rand = 0, std::size_t rand_size = 0);  // rand may be 0
  void create_temp_private_key(PrivateKey& pk, const unsigned char* message, std::size_t size,
                               const unsigned char* rand = 0, std::size_t rand_size = 0);

  // sets temp_pub_key and shared_secret for one-time asymmetric encryption of message
  bool create_temp_shared_secret(unsigned char temp_pub_key[pubkey_bytes], unsigned char secret[shared_secret_bytes],
                                 const PublicKey& recipientPubKey, const unsigned char* message, std::size_t size,
                                 const unsigned char* rand = 0, std::size_t rand_size = 0);
};

unsigned char* TempKeyGenerator::get_temp_private_key(unsigned char* to, const unsigned char* message, std::size_t size,
                                                      const unsigned char* rand,
                                                      std::size_t rand_size) {  // rand may be 0
  digest::SHA256 hasher(message, size);
  hasher.feed(random_salt, salt_size);
  if (rand && rand_size) {
    hasher.feed(rand, rand_size);
  }
  if (!to) {
    to = buffer;
  }
  hasher.extract(to);
  //++ *((long *)random_salt);
  return to;
}

void TempKeyGenerator::create_temp_private_key(PrivateKey& pk, const unsigned char* message, std::size_t size,
                                               const unsigned char* rand, std::size_t rand_size) {
  pk.import_private_key(get_temp_private_key(buffer, message, size, rand, rand_size));
  std::memset(buffer, 0, privkey_bytes);
}

bool TempKeyGenerator::create_temp_shared_secret(unsigned char temp_pub_key[pubkey_bytes],
                                                 unsigned char shared_secret[shared_secret_bytes],
                                                 const PublicKey& recipientPubKey, const unsigned char* message,
                                                 std::size_t size, const unsigned char* rand, std::size_t rand_size) {
  PrivateKey tmpPk;
  create_temp_private_key(tmpPk, message, size, rand, rand_size);
  return tmpPk.export_public_key(temp_pub_key) && tmpPk.compute_shared_secret(shared_secret, recipientPubKey);
}

}  // namespace Ed25519
}  // namespace crypto

// ******************************************************

void print_buffer(const unsigned char buffer[32]) {
  for (int i = 0; i < 32; i++) {
    char buff[4];
    sprintf(buff, "%02x", buffer[i]);
    std::cout << buff;
  }
}

std::string buffer_to_hex(const unsigned char* buffer, std::size_t size = 32) {
  char str[2 * size + 1];
  for (std::size_t i = 0; i < size; i++) {
    sprintf(str + 2 * i, "%02x", buffer[i]);
  }
  return str;
}

int main(void) {
  arith::Bignum x = (3506824292LL << 31);
  x = (2948877059LL << 31);
  arith::Bignum L = (((36 * x + 36) * x + 18) * x + 6) * x + 1;
  arith::Bignum P = L + 6 * sqr(x);
  std::cout << "x= " << x << "; l= " << L << "; p= " << P << std::endl;
  std::cout << "x= " << x.to_hex() << "; l= " << L.to_hex() << "; p= " << P.to_hex() << std::endl;
  std::cout << "x mod 3=" << x % 3 << "; p mod 9=" << P % 9 << "; x/2^31=" << (x >> 31).to_hex() << "=" << (x >> 31)
            << std::endl;

  crypto::Ed25519::PrivateKey PK1, PK2, PK3;
  PK1.random_private_key();
  PK2.random_private_key();
  unsigned char priv2_export[32];
  bool ok = PK2.export_private_key(priv2_export);
  std::cout << "PK2 = " << ok << " " << buffer_to_hex(priv2_export) << std::endl;
  PK3.import_private_key(priv2_export);
  std::cout << "PK3 = " << PK3.ok() << std::endl;

  unsigned char pub_export[32];
  ok = PK1.export_public_key(pub_export);
  std::cout << "PubK1 = " << ok << " " << buffer_to_hex(pub_export) << std::endl;
  crypto::Ed25519::PublicKey PubK1(pub_export);
  ok = PK2.export_public_key(pub_export);
  std::cout << "PubK2 = " << ok << " " << buffer_to_hex(pub_export) << std::endl;
  crypto::Ed25519::PublicKey PubK2(pub_export);
  ok = PK3.export_public_key(pub_export);
  std::cout << "PubK3 = " << ok << " " << buffer_to_hex(pub_export) << std::endl;
  crypto::Ed25519::PublicKey PubK3(pub_export);
  ok = PubK1.export_public_key(pub_export);
  std::cout << "PubK1 = " << ok << " " << buffer_to_hex(pub_export) << std::endl;

  unsigned char secret12[32], secret21[32];
  ok = PK1.compute_shared_secret(secret12, PK3.get_public_key());
  std::cout << "secret(PK1,PubK2)=" << ok << " " << buffer_to_hex(secret12) << std::endl;
  ok = PK2.compute_shared_secret(secret21, PubK1);
  std::cout << "secret(PK2,PubK1)=" << ok << " " << buffer_to_hex(secret21) << std::endl;

  unsigned char signature[64];
  ok = PK1.sign_message(signature, (const unsigned char*)"abc", 3);
  std::cout << "PK1.signature=" << ok << " " << buffer_to_hex(signature) << std::endl;

  // signature[63] ^= 1;
  ok = PubK1.check_message_signature(signature, (const unsigned char*)"abc", 3);
  std::cout << "PubK1.check_signature=" << ok << std::endl;

  unsigned char temp_pubkey[32];
  crypto::Ed25519::TempKeyGenerator TKG;  // use one generator a lot of times

  TKG.create_temp_shared_secret(temp_pubkey, secret12, PubK1, (const unsigned char*)"abc", 3);
  std::cout << "secret12=" << buffer_to_hex(secret12) << "; temp_pubkey=" << buffer_to_hex(temp_pubkey) << std::endl;

  PK1.compute_temp_shared_secret(secret21, temp_pubkey);
  std::cout << "secret21=" << buffer_to_hex(secret21) << std::endl;
}
