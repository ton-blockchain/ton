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
#include <algorithm>
#include <limits>

#include "td/utils/Random.h"
#include "td/utils/SipHash.h"
#include "td/utils/Slice.h"
#include "td/utils/UInt.h"
#include "td/utils/base64.h"
#include "td/utils/benchmark.h"
#include "td/utils/common.h"
#include "td/utils/crypto.h"
#include "td/utils/logging.h"
#include "td/utils/tests.h"

static td::vector<td::string> strings{"", "1", "short test string", td::string(1000000, 'a')};

#if TD_HAVE_OPENSSL
#if TD_HAVE_ZLIB
TEST(Crypto, Aes) {
  td::Random::Xorshift128plus rnd(123);
  td::UInt256 key;
  rnd.bytes(as_mutable_slice(key));
  td::string plaintext(16, '\0');
  td::string encrypted(16, '\0');
  td::string decrypted(16, '\0');
  rnd.bytes(plaintext);

  td::AesState encryptor;
  encryptor.init(as_slice(key), true);
  td::AesState decryptor;
  decryptor.init(as_slice(key), false);

  encryptor.encrypt(td::as_slice(plaintext).ubegin(), td::as_slice(encrypted).ubegin(), 16);
  decryptor.decrypt(td::as_slice(encrypted).ubegin(), td::as_slice(decrypted).ubegin(), 16);

  CHECK(decrypted == plaintext);
  CHECK(decrypted != encrypted);
  CHECK(td::crc32(encrypted) == 178892237);
}

TEST(Crypto, AesCtrState) {
  td::vector<td::uint32> answers1{0u,         1141589763u, 596296607u,  3673001485u, 2302125528u,
                                  330967191u, 2047392231u, 3537459563u, 307747798u,  2149598133u};
  td::vector<td::uint32> answers2{0u,         2053451992u, 1384063362u, 3266188502u, 2893295118u,
                                  780356167u, 1904947434u, 2043402406u, 472080809u,  1807109488u};

  std::size_t i = 0;
  for (auto length : {0, 1, 31, 32, 33, 9999, 10000, 10001, 999999, 1000001}) {
    td::uint32 seed = length;
    td::string s(length, '\0');
    for (auto &c : s) {
      seed = seed * 123457567u + 987651241u;
      c = static_cast<char>((seed >> 23) & 255);
    }

    td::UInt256 key;
    for (auto &c : key.raw) {
      seed = seed * 123457567u + 987651241u;
      c = (seed >> 23) & 255;
    }
    td::UInt128 iv;
    for (auto &c : iv.raw) {
      seed = seed * 123457567u + 987651241u;
      c = (seed >> 23) & 255;
    }

    td::AesCtrState state;
    state.init(as_slice(key), as_slice(iv));
    td::string t(length, '\0');
    state.encrypt(s, t);
    ASSERT_EQ(answers1[i], td::crc32(t));
    state.init(as_slice(key), as_slice(iv));
    state.decrypt(t, t);
    ASSERT_STREQ(td::base64_encode(s), td::base64_encode(t));

    for (auto &c : iv.raw) {
      c = 0xFF;
    }
    state.init(as_slice(key), as_slice(iv));
    state.encrypt(s, t);
    ASSERT_EQ(answers2[i], td::crc32(t));

    i++;
  }
}

TEST(Crypto, AesIgeState) {
  td::vector<td::uint32> answers1{0u, 2045698207u, 2423540300u, 525522475u, 1545267325u};

  std::size_t i = 0;
  for (auto length : {0, 16, 32, 256, 1024}) {
    td::uint32 seed = length;
    td::string s(length, '\0');
    for (auto &c : s) {
      seed = seed * 123457567u + 987651241u;
      c = static_cast<char>((seed >> 23) & 255);
    }

    td::UInt256 key;
    for (auto &c : key.raw) {
      seed = seed * 123457567u + 987651241u;
      c = (seed >> 23) & 255;
    }
    td::UInt256 iv;
    for (auto &c : iv.raw) {
      seed = seed * 123457567u + 987651241u;
      c = (seed >> 23) & 255;
    }

    td::AesIgeState state;
    state.init(as_slice(key), as_slice(iv), true);
    td::string t(length, '\0');
    state.encrypt(s, t);

    ASSERT_EQ(answers1[i], td::crc32(t));

    state.init(as_slice(key), as_slice(iv), false);
    state.decrypt(t, t);
    ASSERT_STREQ(td::base64_encode(s), td::base64_encode(t));
    i++;
  }
}

TEST(Crypto, AesCbcState) {
  td::vector<td::uint32> answers1{0u, 3617355989u, 3449188102u, 186999968u, 4244808847u};

  std::size_t i = 0;
  for (auto length : {0, 16, 32, 256, 1024}) {
    td::uint32 seed = length;
    td::string s(length, '\0');
    for (auto &c : s) {
      seed = seed * 123457567u + 987651241u;
      c = static_cast<char>((seed >> 23) & 255);
    }

    td::UInt256 key;
    for (auto &c : key.raw) {
      seed = seed * 123457567u + 987651241u;
      c = (seed >> 23) & 255;
    }
    td::UInt128 iv;
    for (auto &c : iv.raw) {
      seed = seed * 123457567u + 987651241u;
      c = (seed >> 23) & 255;
    }

    td::AesCbcState state(as_slice(key), as_slice(iv));
    //state.init(as_slice(key), as_slice(iv), true);
    td::string t(length, '\0');
    state.encrypt(s, t);

    ASSERT_EQ(answers1[i], td::crc32(t));

    //state.init(as_slice(key), as_slice(iv), false);
    state = td::AesCbcState(as_slice(key), as_slice(iv));
    state.decrypt(t, t);
    ASSERT_STREQ(td::base64_encode(s), td::base64_encode(t));
    i++;
  }
}
#endif

TEST(Crypto, Sha256State) {
  for (auto length : {0, 1, 31, 32, 33, 9999, 10000, 10001, 999999, 1000001}) {
    auto s = td::rand_string(std::numeric_limits<char>::min(), std::numeric_limits<char>::max(), length);
    td::UInt256 baseline;
    td::sha256(s, as_mutable_slice(baseline));

    td::Sha256State state;
    state.init();
    td::Sha256State state2 = std::move(state);
    auto v = td::rand_split(s);
    for (auto &x : v) {
      state2.feed(x);
    }
    state = std::move(state2);
    td::UInt256 result;
    state.extract(as_mutable_slice(result));
    ASSERT_TRUE(baseline == result);
  }
}

TEST(Crypto, PBKDF) {
  td::vector<td::string> passwords{"", "qwerty", td::string(1000, 'a')};
  td::vector<td::string> salts{"", "qwerty", td::string(1000, 'a')};
  td::vector<int> iteration_counts{1, 2, 1000};
  td::vector<td::Slice> answers{
      "984LZT0tcqQQjPWr6RL/3Xd2Ftu7J6cOggTzri0Pb60=", "lzmEEdaupDp3rO+SImq4J41NsGaL0denanJfdoCsRcU=",
      "T8WKIcEAzhg1uPmZHXOLVpZdFLJOF2H73/xprF4LZno=", "NHxAnMhPOATsb1wV0cGDlAIs+ofzI6I4I8eGJeWN9Qw=",
      "fjYi7waEPjbVYEuZ61/Nm2hbk/vRdShoJoXg4Ygnqe4=", "GhW6e95hGJSf+ID5IrSbvzWyBZ1l35A+UoL55Uh/njk=",
      "BueLDpqSCEc0GWk83WgMwz3UsWwfvVKcvllETSB/Yq8=", "hgHgJZNWRh78PyPdVJsK8whgHOHQbNQiyaTuGDX2IFo=",
      "T2xdyNT1GlcA4+MVNzOe7NCgSAAzNkanNsmuoSr+4xQ=", "/f6t++GUPE+e63+0TrlInL+UsmzRSAAFopa8BBBmb2w=",
      "8Zn98QEAKS9wPOUlN09+pfm0SWs1IGeQxQkNMT/1k48=", "sURLQ/6UX/KVYedyQB21oAtMJ+STZ4iwpxfQtqmWkLw=",
      "T9t/EJXFpPs2Lhca7IVGphTC/OdEloPMHw1UhDnXcyQ=", "TIrtN05E9KQL6Lp/wjtbsFS+KkWZ8jlGK0ErtaoitOg=",
      "+1KcMBjyUNz5VMaIfE5wkGwS6I+IQ5FhK+Ou2HgtVoQ=", "h36ci1T0vGllCl/xJxq6vI7n28Bg40dilzWOKg6Jt8k=",
      "9uwsHJsotTiTqqCYftN729Dg7QI2BijIjV2MvSEUAeE=", "/l+vd/XYgbioh1SfLMaGRr13udmY6TLSlG4OYmytwGU=",
      "7qfZZBbMRLtgjqq7GHgWa/UfXPajW8NXpJ6/T3P1rxI=", "ufwz94p28WnoOFdbrb1oyQEzm/v0CV2b0xBVxeEPJGA=",
      "T/PUUBX2vGMUsI6httlhbMHlGPMvqFBNzayU5voVlaw=", "viMvsvTg9GfQymF3AXZ8uFYTDa3qLrqJJk9w/74iZfg=",
      "HQF+rOZMW4DAdgZz8kAMe28eyIi0rs3a3u/mUeGPNfs=", "7lBVA+GnSxWF/eOo+tyyTB7niMDl1MqP8yzo+xnHTyw=",
      "aTWb7HQAxaTKhSiRPY3GuM1GVmq/FPuwWBU/TUpdy70=", "fbg8M/+Ht/oU+UAZ4dQcGPo+wgCCHaA+GM4tm5jnWcY=",
      "DJbCGFMIR/5neAlpda8Td5zftK4NGekVrg2xjrKW/4c="};

  std::size_t pos = 0;
  for (auto &password : passwords) {
    for (auto &salt : salts) {
      for (auto &iteration_count : iteration_counts) {
        char result[32];
        td::pbkdf2_sha256(password, salt, iteration_count, {result, 32});
        ASSERT_STREQ(answers[pos], td::base64_encode({result, 32}));
        pos++;
      }
    }
  }
}

TEST(Crypto, sha1) {
  td::vector<td::Slice> answers{"2jmj7l5rSw0yVb/vlWAYkK/YBwk=", "NWoZK3kTsExUV00Ywo1G5jlUKKs=",
                                "uRysQwoax0pNJeBC3+zpQzJy1rA=", "NKqXPNTE2qT2Husr260nMWU0AW8="};

  for (std::size_t i = 0; i < strings.size(); i++) {
    unsigned char output[20];
    td::sha1(strings[i], output);
    ASSERT_STREQ(answers[i], td::base64_encode(td::Slice(output, 20)));
  }
}

TEST(Crypto, sha256) {
  td::vector<td::Slice> answers{
      "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "a4ayc/80/OGda4BO/1o/V0etpOqiLx1JwB5S3beHW0s=",
      "yPMaY7Q8PKPwCsw64UnDD5mhRcituEJgzLZMvr0O8pY=", "zcduXJkU+5KBocfihNc+Z/GAmkiklyAOBG05zMcRLNA="};

  for (std::size_t i = 0; i < strings.size(); i++) {
    td::string output(32, '\0');
    td::sha256(strings[i], output);
    ASSERT_STREQ(answers[i], td::base64_encode(output));
  }
}

TEST(Crypto, md5) {
  td::vector<td::Slice> answers{
      "1B2M2Y8AsgTpgAmY7PhCfg==", "xMpCOKC5I4INzFCab3WEmw==", "vwBninYbDRkgk+uA7GMiIQ==", "dwfWrk4CfHDuoqk1wilvIQ=="};

  for (std::size_t i = 0; i < strings.size(); i++) {
    td::string output(16, '\0');
    TD_SUPPRESS("-Wdeprecated-declarations", td::md5(strings[i], output));
    ASSERT_STREQ(answers[i], td::base64_encode(output));
  }
}
#endif

#if TD_HAVE_ZLIB
TEST(Crypto, crc32) {
  td::vector<td::uint32> answers{0u, 2212294583u, 3013144151u, 3693461436u};

  for (std::size_t i = 0; i < strings.size(); i++) {
    ASSERT_EQ(answers[i], td::crc32(strings[i]));
  }
}
#endif

#if TD_HAVE_CRC32C
TEST(Crypto, crc32c) {
  td::vector<td::uint32> answers{0u, 2432014819u, 1077264849u, 1131405888u};

  for (std::size_t i = 0; i < strings.size(); i++) {
    ASSERT_EQ(answers[i], td::crc32c(strings[i]));

    auto v = td::rand_split(strings[i]);
    td::uint32 a = 0;
    td::uint32 b = 0;
    for (auto &x : v) {
      a = td::crc32c_extend(a, x);
      auto x_crc = td::crc32c(x);
      b = td::crc32c_extend(b, x_crc, x.size());
    }
    ASSERT_EQ(answers[i], a);
    ASSERT_EQ(answers[i], b);
  }
}

TEST(Crypto, crc32c_benchmark) {
  class Crc32cExtendBenchmark : public td::Benchmark {
   public:
    explicit Crc32cExtendBenchmark(size_t chunk_size) : chunk_size_(chunk_size) {
    }
    td::string get_description() const override {
      return PSTRING() << "Crc32c with chunk_size=" << chunk_size_;
    }
    void start_up_n(int n) override {
      if (n > (1 << 20)) {
        cnt_ = n / (1 << 20);
        n = (1 << 20);
      } else {
        cnt_ = 1;
      }
      data_ = td::string(n, 'a');
    }
    void run(int n) override {
      td::uint32 res = 0;
      for (int i = 0; i < cnt_; i++) {
        td::Slice data(data_);
        while (!data.empty()) {
          auto head = data.substr(0, chunk_size_);
          data = data.substr(head.size());
          res = td::crc32c_extend(res, head);
        }
      }
      td::do_not_optimize_away(res);
    }

   private:
    size_t chunk_size_;
    td::string data_;
    int cnt_;
  };
  bench(Crc32cExtendBenchmark(2));
  bench(Crc32cExtendBenchmark(8));
  bench(Crc32cExtendBenchmark(32));
  bench(Crc32cExtendBenchmark(128));
  bench(Crc32cExtendBenchmark(65536));
}
#endif

TEST(Crypto, crc64) {
  td::vector<td::uint64> answers{0ull, 3039664240384658157ull, 17549519902062861804ull, 8794730974279819706ull};

  for (std::size_t i = 0; i < strings.size(); i++) {
    ASSERT_EQ(answers[i], td::crc64(strings[i]));
  }
}

TEST(Crypto, crc16) {
  td::vector<td::uint16> answers{0, 9842, 25046, 37023};

  for (std::size_t i = 0; i < strings.size(); i++) {
    ASSERT_EQ(answers[i], td::crc16(strings[i]));
  }
}

static td::Slice rsa_private_key = R"ABCD(
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDeYT5/prmLEa2Q
tZND+UwTmif8kl2VlXaMCjj1k1lJJq8BqS8cVM2vPnOPzFoiC2LYykhm4kk7goCC
ZH6wez9yakg28fcq0Ycv0x8DL1K+VKHJuwIhVfQs//IY1/cBOrMESc+NQowPbv1t
TIFxBO2gebnpLuseht8ix7XtpGC4qAaHN2aEvT2cRsnA76TAK1RVxf1OYGUFBDzY
318WpVZfVIjcQ7K9+eU6b2Yb84VLlvJXw3e1rvw+fBzx2EjpD4zhXy11YppWDyV6
HEb2hs3cGS/LbHfHvdcSfil2omaJP97MDEEY2HFxjR/E5CEf2suvPzX4XS3RE+S3
2aEJaaQbAgMBAAECggEAKo3XRNwls0wNt5xXcvF4smOUdUuY5u/0AHZQUgYBVvM1
GA9E+ZnsxjUgLgs/0DX3k16aHj39H4sohksuxxy+lmlqKkGBN8tioC85RwW+Qre1
QgIsNS7ai+XqcQCavrx51z88nV53qNhnXIwAVR1JT6Ubg1i8G1pZxrEKyk/jRlJd
mGjf6vjitH//PPkghPJ/D42k93YRcy+duOgqYDQpLZp8DiEGfYrX10B1H7HrWLV+
Wp5KO1YXtKgQUplj6kYy72bVajbxYTvzgjaaKsh74jBO0uT3tHTtXG0dcKGb0VR/
cqP/1H/lC9bAnAqAGefNusGJQZIElvTsrpIQXOeZsQKBgQD2W04S+FjqYYFjnEFX
6eL4it01afs5M3/C6CcI5JQtN6p+Na4NCSILol33xwhakn87zqdADHawBYQVQ8Uw
dPurl805wfkzN3AbfdDmtx0IJ8vK4HFpktRjfpwBVhlVtm1doAYFqqsuCF2vWW1t
mM2YOSq4AnRHCeBb/P6kRIW0MwKBgQDnFawKKqiC4tuyBOkkEhexlm7x9he0md7D
3Z2hc3Bmdcq1niw4wBq3HUxGLReGCcSr5epKSQwkunlTn5ZSC6Rmbe4zxsGIwbb3
5W3342swBaoxEIuBokBvZ/xUOXVwiqKj+S/NzVkZcnT6K9V/HnUCQR+JBbQxFQaX
iiezcjKoeQKBgCIVUcDoIQ0UPl10ocmy7xbpx177calhSZzCl5vwW9vBptHdRV5C
VDZ92ThNjgdR205/8b23u7fwm2yBusdQd/0ufFMwVfTTB6yWBI/W56pYLya7VJWB
nebB/n1k1w53tbvNRugDy7kLqUJ4Qd521ILp7dIVbNbjM+omH2jEnibnAoGBAIM5
a1jaoJay/M86uqohHBNcuePtO8jzF+1iDAGC7HFCsrov+CzB6mnR2V6AfLtBEM4M
4d8NXDf/LKawGUy+D72a74m3dG+UkbJ0Nt5t5pB+pwb1vkL/QFgDVOb/OhGOqI01
FFBqLA6nUIZAHhzxzsBY+u90rb6xkey8J49faiUBAoGAaMgOgEvQB5H19ZL5tMkl
A/DKtTz/NFzN4Zw/vNPVb7eNn4jg9M25d9xqvL4acOa+nuV3nLHbcUWE1/7STXw1
gT58CvoEmD1AiP95nup+HKHENJ1DWMgF5MDfVQwGCvWP5/Qy89ybr0eG8HjbldbN
MpSmzz2wOz152oGdOd3syT4=
-----END PRIVATE KEY-----
)ABCD";

static td::Slice rsa_public_key = R"ABCD(
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA3mE+f6a5ixGtkLWTQ/lM
E5on/JJdlZV2jAo49ZNZSSavAakvHFTNrz5zj8xaIgti2MpIZuJJO4KAgmR+sHs/
cmpINvH3KtGHL9MfAy9SvlShybsCIVX0LP/yGNf3ATqzBEnPjUKMD279bUyBcQTt
oHm56S7rHobfIse17aRguKgGhzdmhL09nEbJwO+kwCtUVcX9TmBlBQQ82N9fFqVW
X1SI3EOyvfnlOm9mG/OFS5byV8N3ta78Pnwc8dhI6Q+M4V8tdWKaVg8lehxG9obN
3Bkvy2x3x73XEn4pdqJmiT/ezAxBGNhxcY0fxOQhH9rLrz81+F0t0RPkt9mhCWmk
GwIDAQAB
-----END PUBLIC KEY-----
)ABCD";

TEST(Crypto, rsa) {
  auto value = td::rand_string('a', 'z', 200);
  auto encrypted_value = td::rsa_encrypt_pkcs1_oaep(rsa_public_key, value).move_as_ok();
  auto decrypted_value = td::rsa_decrypt_pkcs1_oaep(rsa_private_key, encrypted_value.as_slice()).move_as_ok();
  ASSERT_TRUE(decrypted_value.as_slice().truncate(value.size()) == value);
}

TEST(Crypto, sip_hash13) {
  auto hash = [](td::Slice data) { return td::sip_hash13(data, 0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL); };

  // The SipHash authors publish vectors for 2-4 only, so these come from an independent
  // implementation of the 1-3 parameters written from the specification. They are what makes this
  // SipHash-1-3 rather than a keyed hash that merely mixes well: every property below still holds
  // if a rotation constant is mistyped, but these digests do not.
  ASSERT_EQ(0xabac0158050fc4dcULL, hash(""));
  ASSERT_EQ(0x1c2697ab786a6237ULL, hash("a"));
  ASSERT_EQ(0x6fce24e8af8146ebULL, hash("abc"));
  ASSERT_EQ(0xe393c48ea7bc21efULL, hash("0123456789abcdef"));

  ASSERT_EQ(hash("abc"), hash("abc"));
  ASSERT_TRUE(hash("abc") != td::sip_hash13("abc", 1, 2));
  ASSERT_TRUE(hash("") != hash(td::Slice("\0", 1)));
  ASSERT_TRUE(hash(td::Slice("\0", 1)) != hash(td::Slice("\0\0", 2)));

  // Every bit of a connection-id sized input must reach the digest.
  td::string base(20, '\0');
  auto base_hash = hash(base);
  for (size_t bit = 0; bit < base.size() * 8; bit++) {
    auto flipped = base;
    flipped[bit / 8] = static_cast<char>(flipped[bit / 8] ^ (1 << (bit % 8)));
    ASSERT_TRUE(hash(flipped) != base_hash);
  }

  // Random keys must spread: a table of 4096 buckets holding 40k keys averages 10 per bucket, and a
  // hash that ignored part of the input would pile them up instead.
  constexpr size_t bucket_count = 4096;
  constexpr size_t key_count = 40000;
  td::vector<size_t> buckets(bucket_count);
  for (size_t i = 0; i < key_count; i++) {
    td::string key(20, '\0');
    td::Random::secure_bytes(td::MutableSlice(key));
    buckets[hash(key) % bucket_count]++;
  }
  auto worst = *std::max_element(buckets.begin(), buckets.end());
  LOG_CHECK(worst < 40) << "worst bucket has " << worst << " of " << key_count << " keys";
}

TEST(Crypto, sip_hash13_benchmark) {
  // The size that matters is a connection id: this hash sits on every packet's lookup.
  class SipHash13Benchmark : public td::Benchmark {
   public:
    explicit SipHash13Benchmark(size_t size) : size_(size) {
    }
    td::string get_description() const override {
      return PSTRING() << "sip_hash13 of " << size_ << " bytes";
    }
    void start_up() override {
      data_ = td::string(size_, 'a');
    }
    void run(int n) override {
      td::uint64 res = 0;
      for (int i = 0; i < n; i++) {
        res ^= td::sip_hash13(data_, 1, 2);
      }
      td::do_not_optimize_away(res);
    }

   private:
    size_t size_;
    td::string data_;
  };

  // What it replaced, kept here so the cost of being keyed stays visible.
  class MultiplyHashBenchmark : public td::Benchmark {
   public:
    explicit MultiplyHashBenchmark(size_t size) : size_(size) {
    }
    td::string get_description() const override {
      return PSTRING() << "h * 31 + byte of " << size_ << " bytes";
    }
    void start_up() override {
      data_ = td::string(size_, 'a');
    }
    void run(int n) override {
      td::uint64 res = 0;
      for (int i = 0; i < n; i++) {
        td::uint64 h = 0;
        for (char c : data_) {
          h = h * 31 + static_cast<unsigned char>(c);
        }
        res ^= h;
      }
      td::do_not_optimize_away(res);
    }

   private:
    size_t size_;
    td::string data_;
  };

  for (auto size : {size_t{8}, size_t{20}, size_t{64}}) {
    td::bench(SipHash13Benchmark(size));
    td::bench(MultiplyHashBenchmark(size));
  }
}
