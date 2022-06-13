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
#include <array>
#include <string>
#include <iostream>
#include <sstream>

namespace modint {

enum { mod_cnt = 32 };

// mod_cnt = 9 => integers -2^268 .. 2^268
// mod_cnt = 18 => integers -2^537 .. 2^537
// mod_cnt = 32 => integers -2^955 .. 2^955
constexpr int mod[mod_cnt] = {999999937, 999999929, 999999893, 999999883, 999999797, 999999761, 999999757, 999999751,
                              999999739, 999999733, 999999677, 999999667, 999999613, 999999607, 999999599, 999999587,
                              999999541, 999999527, 999999503, 999999491, 999999487, 999999433, 999999391, 999999353,
                              99999337,  999999323, 999999229, 999999223, 999999197, 999999193, 999999191, 999999181};

// invm[i][j] = mod[i]^(-1) modulo mod[j]
int invm[mod_cnt][mod_cnt];

int gcdx(int a, int b, int& u, int& v);

template <int N = mod_cnt>
struct ModArray;

template <int N = mod_cnt>
struct MixedRadix;

template <int N>
struct ArrayRawDumpRef;

template <typename T, int N>
std::ostream& raw_dump_array(std::ostream& os, const std::array<T, N>& arr) {
  os << '[';
  for (auto x : arr) {
    os << ' ' << x;
  }
  return os << " ]";
}

template <int N>
struct MixedRadix {
  enum { n = N };
  int a[N];
  MixedRadix(int v) {
    set_int(v);
  }
  MixedRadix() = default;
  MixedRadix(const MixedRadix&) = default;
  MixedRadix(std::initializer_list<int> l) {
    auto sz = std::min(l.size(), (std::size_t)N);
    std::copy(l.begin(), l.begin() + sz, a);
    std::fill(a + sz, a + N, 0);
  }
  MixedRadix(const std::array<int, N>& arr) {
    std::copy(arr.begin(), arr.end(), a);
  }
  template <int M>
  MixedRadix(const MixedRadix<M>& other) {
    static_assert(M >= N);
    std::copy(other.a, other.a + N, a);
  }
  MixedRadix(const ModArray<N>& other);
  MixedRadix(const ModArray<N>& other, bool sgnd);

  MixedRadix& set_zero() {
    std::fill(a, a + N, 0);
    return *this;
  }
  MixedRadix& set_one() {
    a[0] = 1;
    std::fill(a + 1, a + N, 0);
    return *this;
  }
  MixedRadix& set_int(int v) {
    a[0] = v;
    std::fill(a + 1, a + N, 0);
    return *this;
  }

  MixedRadix copy() const {
    return MixedRadix{*this};
  }

  static const int* mod_array() {
    return mod;
  }

  static int modulus(int i) {
    return mod[i];
  }

  int sgn() const {
    int i = N - 1;
    while (i >= 0 && !a[i]) {
      --i;
    }
    return i < 0 ? 0 : (a[i] > 0 ? 1 : -1);
  }

  int cmp(const MixedRadix& other) const {
    int i = N - 1;
    while (i >= 0 && a[i] == other.a[i]) {
      --i;
    }
    return i < 0 ? 0 : (a[i] > other.a[i] ? 1 : -1);
  }

  bool is_small() const {
    return !a[N - 1] || a[N - 1] == -1;
  }

  bool operator==(const MixedRadix& other) const {
    return std::equal(a, a + N, other.a);
  }

  bool operator!=(const MixedRadix& other) const {
    return !std::equal(a, a + N, other.a);
  }

  bool operator<(const MixedRadix& other) const {
    return cmp(other) < 0;
  }

  bool operator<=(const MixedRadix& other) const {
    return cmp(other) <= 0;
  }

  bool operator>(const MixedRadix& other) const {
    return cmp(other) > 0;
  }

  bool operator>=(const MixedRadix& other) const {
    return cmp(other) >= 0;
  }

  explicit operator bool() const {
    return sgn();
  }

  bool operator!() const {
    return !sgn();
  }

  MixedRadix& negate() {
    int i = 0;
    while (i < N - 1 && !a[i]) {
      i++;
    }
    a[i]--;
    for (; i < N; i++) {
      a[i] = mod[i] - a[i] - 1;
    }
    a[N - 1] -= mod[N - 1];
    return *this;
  }

  static const MixedRadix& pow2(int power);
  static MixedRadix negpow2(int power) {
    return -pow2(power);
  }

  template <int M>
  const MixedRadix<M>& as_shorter() const {
    static_assert(M <= N);
    return *reinterpret_cast<const MixedRadix<M>*>(this);
  }

  MixedRadix& import_mod_array(const int* data, bool sgnd = true) {
    for (int i = 0; i < N; i++) {
      a[i] = data[i] % mod[i];
    }
    for (int i = 0; i < N; i++) {
      if (a[i] < 0) {
        a[i] += mod[i];
      }
      for (int j = i + 1; j < N; j++) {
        a[j] = (int)((long long)(a[j] - a[i]) * invm[i][j] % mod[j]);
      }
    }
    if (sgnd && a[N - 1] > (mod[N - 1] >> 1)) {
      a[N - 1] -= mod[N - 1];
    }
    return *this;
  }

  MixedRadix& operator=(const MixedRadix&) = default;

  template <int M>
  MixedRadix& operator=(const MixedRadix<M>& other) {
    static_assert(M >= N);
    std::copy(other.a, other.a + N, a);
  }

  MixedRadix& import_mod_array(const ModArray<N>& other, bool sgnd = true);

  MixedRadix& operator=(const ModArray<N>& other) {
    return import_mod_array(other);
  }

  MixedRadix& set_sum(const MixedRadix& x, const MixedRadix& y, int factor = 1) {
    long long carry = 0;
    for (int i = 0; i < N; i++) {
      long long acc = x.a[i] + carry + (long long)factor * y.a[i];
      carry = acc / mod[i];
      a[i] = (int)(acc - carry * mod[i]);
      if (a[i] < 0) {
        a[i] += mod[i];
        --carry;
      }
    }
    if (a[N - 1] >= 0 && carry == -1) {
      a[N - 1] -= mod[N - 1];
    }
    return *this;
  }

  MixedRadix& operator+=(const MixedRadix& other) {
    return set_sum(*this, other);
  }

  MixedRadix& operator-=(const MixedRadix& other) {
    return set_sum(*this, other, -1);
  }

  static const MixedRadix& zero();
  static const MixedRadix& one();

  MixedRadix& operator*=(int factor) {
    return set_sum(zero(), *this, factor);
  }

  MixedRadix operator-() const {
    MixedRadix copy{*this};
    copy.negate();
    return copy;
  }

  MixedRadix operator+(const MixedRadix& other) const {
    MixedRadix res;
    res.set_sum(*this, other);
    return res;
  }

  MixedRadix operator-(const MixedRadix& other) const {
    MixedRadix res;
    res.set_sum(*this, other, -1);
    return res;
  }

  MixedRadix operator*(int factor) const {
    MixedRadix res;
    res.set_sum(zero(), *this, factor);
    return res;
  }

  int operator%(int b) const {
    int x = a[N - 1] % b;
    for (int i = N - 2; i >= 0; --i) {
      x = ((long long)x * mod[i] + a[i]) % b;
    }
    return ((x ^ b) < 0 && x) ? x + b : x;
  }

  explicit operator double() const {
    double acc = 0.;
    for (int i = N - 1; i >= 0; --i) {
      acc = acc * mod[i] + a[i];
    }
    return acc;
  }

  explicit operator long long() const {
    long long acc = 0.;
    for (int i = N - 1; i >= 0; --i) {
      acc = acc * mod[i] + a[i];
    }
    return acc;
  }

  MixedRadix& to_base(int base) {
    int k = N - 1;
    while (k > 0 && !a[k]) {
      --k;
    }
    if (k <= 0) {
      return *this;
    }
    for (int i = k - 1; i >= 0; --i) {
      // a[i..k] := a[i+1..k] * mod[i] + a[i]
      long long carry = a[i];
      for (int j = i; j < k; j++) {
        long long t = (long long)a[j + 1] * mod[i] + carry;
        carry = t / base;
        a[j] = (int)(t - carry * base);
      }
      a[k] = (int)carry;
    }
    return *this;
  }

  std::ostream& print_dec_destroy(std::ostream& os) {
    int s = sgn();
    if (s < 0) {
      os << '-';
      negate();
    } else if (!s) {
      os << '0';
      return os;
    }
    to_base(1000000000);
    int i = N - 1;
    while (!a[i] && i > 0) {
      --i;
    }
    os << a[i];
    while (--i >= 0) {
      char buff[12];
      sprintf(buff, "%09d", a[i]);
      os << buff;
    }
    return os;
  }

  std::ostream& print_dec(std::ostream& os) const& {
    MixedRadix copy{*this};
    return copy.print_dec_destroy(os);
  }

  std::ostream& print_dec(std::ostream& os) && {
    return print_dec_destroy(os);
  }

  std::string to_dec_string_destroy() {
    std::ostringstream os;
    print_dec_destroy(os);
    return std::move(os).str();
  }

  std::string to_dec_string() const& {
    MixedRadix copy{*this};
    return copy.to_dec_string_destroy();
  }

  std::string to_dec_string() && {
    return to_dec_string_destroy();
  }

  bool to_binary_destroy(unsigned char* arr, int size, bool sgnd = true) {
    if (size <= 0) {
      return false;
    }
    int s = (sgnd ? sgn() : 1);
    memset(arr, 0, size);
    if (s < 0) {
      negate();
    } else if (!s) {
      return true;
    }
    to_base(1 << 30);
    long long acc = 0;
    int bits = 0, j = size;
    for (int i = 0; i < N; i++) {
      if (!j && a[i]) {
        return false;
      }
      acc += ((long long)a[i] << bits);
      bits += 30;
      while (bits >= 8 && j > 0) {
        arr[--j] = (unsigned char)(acc & 0xff);
        bits -= 8;
        acc >>= 8;
      }
    }
    while (j > 0) {
      arr[--j] = (unsigned char)(acc & 0xff);
      acc >>= 8;
    }
    if (acc) {
      return false;
    }
    if (!sgnd) {
      return true;
    }
    if (s >= 0) {
      return arr[0] <= 0x7f;
    }
    j = size - 1;
    while (j >= 0 && !arr[j]) {
      --j;
    }
    assert(j >= 0);
    arr[j] = (unsigned char)(-arr[j]);
    while (--j >= 0) {
      arr[j] = (unsigned char)~arr[j];
    }
    return arr[0] >= 0x80;
  }

  bool to_binary(unsigned char* arr, int size, bool sgnd = true) const& {
    MixedRadix copy{*this};
    return copy.to_binary_destroy(arr, size, sgnd);
  }

  bool to_binary(unsigned char* arr, int size, bool sgnd = true) && {
    return to_binary_destroy(arr, size, sgnd);
  }

  std::ostream& raw_dump(std::ostream& os) const {
    return raw_dump_array<int, N>(os, a);
  }

  ArrayRawDumpRef<N> dump() const {
    return {a};
  }
};

template <int N>
struct ModArray {
  enum { n = N };
  int a[N];
  ModArray(int v) {
    set_int(v);
  }
  ModArray(long long v) {
    set_long(v);
  }
  ModArray(long v) {
    set_long(v);
  }
  ModArray() = default;
  ModArray(const ModArray&) = default;
  ModArray(std::initializer_list<int> l) {
    auto sz = std::min(l.size(), (std::size_t)N);
    std::copy(l.begin(), l.begin() + sz, a);
    std::fill(a + sz, a + N, 0);
  }
  ModArray(const std::array<int, N>& arr) {
    std::copy(arr.begin(), arr.end(), a);
  }
  template <int M>
  ModArray(const ModArray<M>& other) {
    static_assert(M >= N);
    std::copy(other.a, other.a + N, a);
  }
  ModArray(const int* p) : a(p) {
  }
  ModArray(std::string str) {
    assert(from_dec_string(str) && "not a decimal number");
  }

  ModArray& set_zero() {
    std::fill(a, a + N, 0);
    return *this;
  }
  ModArray& set_one() {
    std::fill(a, a + N, 1);
    return *this;
  }

  ModArray& set_int(int v) {
    if (v >= 0) {
      std::fill(a, a + N, v);
    } else {
      for (int i = 0; i < N; i++) {
        a[i] = mod[i] + v;
      }
    }
    return *this;
  }

  ModArray& set_long(long long v) {
    for (int i = 0; i < N; i++) {
      a[i] = v % mod[i];
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  ModArray copy() const {
    return ModArray{*this};
  }

  static const int* mod_array() {
    return mod;
  }

  static int modulus(int i) {
    return mod[i];
  }

  static const ModArray& zero();
  static const ModArray& one();

  ModArray& operator=(const ModArray&) = default;

  template <int M>
  ModArray& operator=(const ModArray<M>& other) {
    static_assert(M >= N);
    std::copy(other.a, other.a + N, a);
    return *this;
  }

  ModArray& negate() {
    for (int i = 0; i < N; i++) {
      a[i] = (a[i] ? mod[i] - a[i] : 0);
    }
    return *this;
  }

  ModArray& norm_neg() {
    for (int i = 0; i < N; i++) {
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  ModArray& normalize() {
    for (int i = 0; i < N; i++) {
      a[i] %= mod[i];
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  bool is_zero() const {
    for (int i = 0; i < N; i++) {
      if (a[i]) {
        return false;
      }
    }
    return true;
  }

  explicit operator bool() const {
    return !is_zero();
  }

  bool operator!() const {
    return is_zero();
  }

  bool operator==(const ModArray& other) const {
    return std::equal(a, a + N, other.a);
  }

  bool operator!=(const ModArray& other) const {
    return !std::equal(a, a + N, other.a);
  }

  bool operator==(long long val) const {
    for (int i = 0; i < N; i++) {
      int r = (int)(val % mod[i]);
      if (a[i] != (r < 0 ? r + mod[i] : r)) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(long long val) const {
    return !operator==(val);
  }

  long long try_get_long() const {
    return (long long)(MixedRadix<3>(*this));
  }

  bool fits_long() const {
    return operator==(try_get_long());
  }

  explicit operator long long() const {
    auto v = try_get_long();
    return operator==(v) ? v : -0x8000000000000000;
  }

  ModArray& set_sum(const ModArray& x, const ModArray& y) {
    for (int i = 0; i < N; i++) {
      a[i] = x.a[i] + y.a[i];
      if (a[i] >= mod[i]) {
        a[i] -= mod[i];
      }
    }
    return *this;
  }

  ModArray& operator+=(const ModArray& other) {
    for (int i = 0; i < N; i++) {
      a[i] += other.a[i];
      if (a[i] >= mod[i]) {
        a[i] -= mod[i];
      }
    }
    return *this;
  }

  ModArray& operator+=(long long v) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)((a[i] + v) % mod[i]);
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  ModArray& operator-=(const ModArray& other) {
    for (int i = 0; i < N; i++) {
      a[i] -= other.a[i];
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  ModArray& operator-=(long long v) {
    return (operator+=)(-v);
  }

  ModArray& mul_arr(const int other[]) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)(((long long)a[i] * other[i]) % mod[i]);
    }
    return *this;
  }

  ModArray& operator*=(const ModArray& other) {
    return mul_arr(other.a);
  }

  template <int M>
  ModArray& operator*=(const ModArray<M>& other) {
    static_assert(M >= N);
    return mul_arr(other.a);
  }

  ModArray& operator*=(int v) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)(((long long)a[i] * v) % mod[i]);
    }
    return (v >= 0 ? *this : norm_neg());
  }

  ModArray& operator*=(long long v) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)(((long long)a[i] * (v % mod[i])) % mod[i]);
    }
    return (v >= 0 ? *this : norm_neg());
  }

  ModArray& mul_add(int v, long long w) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)(((long long)a[i] * v + w) % mod[i]);
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  // *this = (*this * other) + w
  ModArray& mul_add(const ModArray& other, long long w) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)(((long long)a[i] * other.a[i] + w) % mod[i]);
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  // *this = (*this << shift) + w
  ModArray& lshift_add(int shift, long long w) {
    return mul_add(pow2(shift), w);
  }

  // *this = *this + other * w
  ModArray& add_mul(const ModArray& other, long long w) {
    for (int i = 0; i < N; i++) {
      a[i] = (int)((a[i] + other.a[i] * w) % mod[i]);
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return *this;
  }

  // *this += w << shift
  ModArray& add_lshift(int shift, long long w) {
    return add_mul(pow2(shift), w);
  }

  ModArray operator+(const ModArray& other) const {
    ModArray copy{*this};
    copy += other;
    return copy;
  }

  ModArray operator-(const ModArray& other) const {
    ModArray copy{*this};
    copy -= other;
    return copy;
  }

  ModArray operator+(long long other) const {
    ModArray copy{*this};
    copy += other;
    return copy;
  }

  ModArray operator-(long long other) const {
    ModArray copy{*this};
    copy += -other;
    return copy;
  }

  ModArray operator-() const {
    ModArray copy{*this};
    copy.negate();
    return copy;
  }

  ModArray operator*(const ModArray& other) const {
    ModArray copy{*this};
    copy *= other;
    return copy;
  }

  ModArray operator*(long long other) const {
    ModArray copy{*this};
    copy *= other;
    return copy;
  }

  bool invert() {
    for (int i = 0; i < N; i++) {
      int t;
      if (gcdx(a[i], mod[i], a[i], t) != 1) {
        return false;
      }
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return true;
  }

  bool try_divide(const ModArray& other) {
    for (int i = 0; i < N; i++) {
      int q, t;
      if (gcdx(other.a[i], mod[i], q, t) != 1) {
        return false;
      }
      a[i] = (int)((long long)a[i] * q % mod[i]);
      if (a[i] < 0) {
        a[i] += mod[i];
      }
    }
    return true;
  }

  ModArray& operator/=(const ModArray& other) {
    if (!try_divide(other)) {
      assert(false); // division by zero?
    }
    return *this;
  }

  ModArray operator/(const ModArray& other) {
    ModArray copy{*this};
    copy /= other;
    return copy;
  }

  static const ModArray& pow2(int power);
  static const ModArray& negpow2(int power);

  ModArray& operator<<=(int lshift) {
    return operator*=(pow2(lshift));
  }

  ModArray operator<<(int lshift) const {
    return operator*(pow2(lshift));
  }

  ModArray& operator>>=(int rshift) {
    return operator/=(pow2(rshift));
  }

  ModArray operator>>(int rshift) const {
    return operator/(pow2(rshift));
  }

  template <int M>
  const ModArray<M>& as_shorter() const {
    static_assert(M <= N);
    return *reinterpret_cast<const ModArray<M>*>(this);
  }

  MixedRadix<N>& to_mixed_radix(MixedRadix<N>& dest, bool sgnd = true) const {
    return dest.import_mod_array(a, sgnd);
  }

  MixedRadix<N> to_mixed_radix(bool sgnd = true) const {
    return MixedRadix<N>(*this, sgnd);
  }

  int operator%(int div) const {
    return to_mixed_radix() % div;
  }

  explicit operator double() const {
    return (double)to_mixed_radix();
  }

  std::string to_dec_string() const {
    return MixedRadix<N>(*this).to_dec_string();
  }

  std::ostream& print_dec(std::ostream& os, bool sgnd = true) const {
    return MixedRadix<N>(*this, sgnd).print_dec(os);
  }

  bool to_binary(unsigned char* arr, int size, bool sgnd = true) const {
    return MixedRadix<N>(*this, sgnd).to_binary(arr, size, sgnd);
  }

  template <std::size_t M>
  bool to_binary(std::array<unsigned char, M>& arr, bool sgnd = true) const {
    return to_binary(arr.data(), M, sgnd);
  }

  bool from_dec_string(const char* start, const char* end) {
    set_zero();
    if (start >= end) {
      return false;
    }
    bool sgn = (*start == '-');
    if (sgn && ++start == end) {
      return false;
    }
    int acc = 0, pow = 1;
    while (start < end) {
      if (*start < '0' || *start > '9') {
        return false;
      }
      acc = acc * 10 + (*start++ - '0');
      pow *= 10;
      if (pow >= 1000000000) {
        mul_add(pow, acc);
        pow = 1;
        acc = 0;
      }
    }
    if (pow > 1) {
      mul_add(pow, acc);
    }
    if (sgn) {
      negate();
    }
    return true;
  }

  bool from_dec_string(std::string str) {
    return from_dec_string(str.data(), str.data() + str.size());
  }

  ModArray& from_binary(const unsigned char* arr, int size, bool sgnd = true) {
    set_zero();
    if (size <= 0) {
      return *this;
    }
    int i = 0, pow = 0;
    long long acc = (sgnd && arr[0] >= 0x80 ? -1 : 0);
    while (i < size && arr[i] == (unsigned char)acc) {
      i++;
    }
    for (; i < size; i++) {
      pow += 8;
      acc = (acc << 8) + arr[i];
      if (pow >= 56) {
        lshift_add(pow, acc);
        acc = pow = 0;
      }
    }
    if (pow || acc) {
      lshift_add(pow, acc);
    }
    return *this;
  }

  template <std::size_t M>
  ModArray& from_binary(const std::array<unsigned char, M>& arr, bool sgnd = true) {
    return from_binary(arr.data(), M, sgnd);
  }

  std::ostream& raw_dump(std::ostream& os) const {
    return raw_dump_array<int, N>(os, a);
  }

  ArrayRawDumpRef<N> dump() const {
    return {a};
  }
};

template <int N>
MixedRadix<N>::MixedRadix(const ModArray<N>& other) {
  import_mod_array(other.a);
}

template <int N>
MixedRadix<N>::MixedRadix(const ModArray<N>& other, bool sgnd) {
  import_mod_array(other.a, sgnd);
}

template <int N>
MixedRadix<N>& MixedRadix<N>::import_mod_array(const ModArray<N>& other, bool sgnd) {
  return import_mod_array(other.a, sgnd);
}

template <int N>
std::ostream& operator<<(std::ostream& os, const ModArray<N>& x) {
  return x.print_dec(os);
}

template <int N>
std::ostream& operator<<(std::ostream& os, const MixedRadix<N>& x) {
  return x.print_dec(os);
}

template <int N>
std::ostream& operator<<(std::ostream& os, MixedRadix<N>&& x) {
  return x.print_dec_destroy(os);
}

template <int N>
struct ArrayRawDumpRef {
  const std::array<int, N>& ref;
  ArrayRawDumpRef(const std::array<int, N>& _ref) : ref(_ref){};
};

template <int N>
std::ostream& operator<<(std::ostream& os, ArrayRawDumpRef<N> rd_ref) {
  return raw_dump_array<int, N>(os, rd_ref.ref);
};

constexpr int pow2_cnt = 1001;

ModArray<mod_cnt> Zero(0), One(1), Pow2[pow2_cnt], NegPow2[pow2_cnt];
MixedRadix<mod_cnt> Zero_mr(0), One_mr(1), Pow2_mr[pow2_cnt], NegPow2_mr[pow2_cnt];

template <int N>
const MixedRadix<N>& MixedRadix<N>::pow2(int power) {
  return Pow2_mr[power].as_shorter<N>();
}

/*
template <int N>
const MixedRadix<N>& MixedRadix<N>::negpow2(int power) {
  return NegPow2_mr[power].as_shorter<N>();
}
*/

template <int N>
const ModArray<N>& ModArray<N>::pow2(int power) {
  return Pow2[power].as_shorter<N>();
}

template <int N>
const ModArray<N>& ModArray<N>::negpow2(int power) {
  return NegPow2[power].as_shorter<N>();
}

template <int N>
const ModArray<N>& ModArray<N>::zero() {
  return Zero.as_shorter<N>();
}

template <int N>
const ModArray<N>& ModArray<N>::one() {
  return One.as_shorter<N>();
}

template <int N>
const MixedRadix<N>& MixedRadix<N>::zero() {
  return Zero_mr.as_shorter<N>();
}

template <int N>
const MixedRadix<N>& MixedRadix<N>::one() {
  return One_mr.as_shorter<N>();
}

void init_pow2() {
  Pow2[0].set_one();
  Pow2_mr[0].set_one();
  for (int i = 1; i < pow2_cnt; i++) {
    Pow2[i].set_sum(Pow2[i - 1], Pow2[i - 1]);
    Pow2_mr[i].set_sum(Pow2_mr[i - 1], Pow2_mr[i - 1]);
  }
  for (int i = 0; i < pow2_cnt; i++) {
    NegPow2[i] = -Pow2[i];
    NegPow2_mr[i] = -Pow2_mr[i];
  }
}

int gcdx(int a, int b, int& u, int& v) {
  int a1 = 1, a2 = 0, b1 = 0, b2 = 1;
  while (b) {
    int q = a / b;
    int t = a - q * b;
    a = b;
    b = t;
    t = a1 - q * b1;
    a1 = b1;
    b1 = t;
    t = a2 - q * b2;
    a2 = b2;
    b2 = t;
  }
  u = a1;
  v = a2;
  return a;
}

void init_invm() {
  for (int i = 0; i < mod_cnt; i++) {
    assert(mod[i] > 0 && mod[i] <= (1 << 30));
    for (int j = 0; j < i; j++) {
      if (gcdx(mod[i], mod[j], invm[i][j], invm[j][i]) != 1) {
        assert(false);
      }
      if (invm[i][j] < 0) {
        invm[i][j] += mod[j];
      }
      if (invm[j][i] < 0) {
        invm[j][i] += mod[i];
      }
    }
  }
}

void init() {
  init_invm();
  init_pow2();
}

}  // namespace modint
