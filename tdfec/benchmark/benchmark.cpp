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
#include "td/utils/benchmark.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/Time.h"
#include "td/utils/tests.h"

#include "td/fec/fec.h"
#include "td/fec/algebra/Octet.h"
#include "td/fec/algebra/GaussianElimination.h"
#include "td/fec/algebra/Simd.h"
#include <cstdio>

template <class Simd, size_t size = 256>
class Simd_gf256_from_gf2 : public td::Benchmark {
 public:
  explicit Simd_gf256_from_gf2(std::string description) : description_(std::move(description)) {
    for (size_t i = 0; i < size; i++) {
      src[i] = td::uint8(td::Random::fast(0, 255));
    }
  }
  std::string get_description() const override {
    return PSTRING() << "gf256_from_gf2 " << description_ << " " << size;
  }
  void run(int n) override {
    for (int i = 0; i < n; i++) {
      Simd::gf256_from_gf2(dest, src, size);
    }
    td::do_not_optimize_away(dest[0]);
  }

 private:
  alignas(32) td::uint8 dest[8 * size];
  alignas(32) td::uint8 src[size];
  std::string description_;
};
template <class Simd, size_t size = 256 * 8>
class Simd_gf256_add : public td::Benchmark {
 public:
  explicit Simd_gf256_add(std::string description) : description_(std::move(description)) {
    for (size_t i = 0; i < size; i++) {
      src[i] = td::uint8(td::Random::fast(0, 255));
    }
  }
  std::string get_description() const override {
    return PSTRING() << "gf256_add " << description_ << " " << size;
  }
  void run(int n) override {
    for (int i = 0; i < n; i++) {
      Simd::gf256_add(dest, src, size);
    }
    td::do_not_optimize_away(dest[0]);
  }

 private:
  alignas(32) td::uint8 dest[size];
  alignas(32) td::uint8 src[size];
  std::string description_;
};

template <class Simd, size_t size = 256 * 8>
class Simd_gf256_add_mul : public td::Benchmark {
 public:
  explicit Simd_gf256_add_mul(std::string description) : description_(std::move(description)) {
    for (size_t i = 0; i < size; i++) {
      src[i] = td::uint8(td::Random::fast(0, 255));
    }
  }
  std::string get_description() const override {
    return PSTRING() << "gf256_add_mul " << description_ << " " << size;
  }
  void run(int n) override {
    for (int i = 0; i < n; i++) {
      Simd::gf256_add_mul(dest, src, 211, size);
    }
    td::do_not_optimize_away(dest[0]);
  }

 private:
  alignas(32) td::uint8 dest[size];
  alignas(32) td::uint8 src[size];
  std::string description_;
};

template <class Simd, size_t size = 256 * 8>
class Simd_gf256_mul : public td::Benchmark {
 public:
  explicit Simd_gf256_mul(std::string description) : description_(std::move(description)) {
    for (size_t i = 0; i < size; i++) {
      src[i] = td::uint8(td::Random::fast(0, 255));
    }
  }
  std::string get_description() const override {
    return PSTRING() << "gf256_mul " << description_ << " " << size;
  }
  void run(int n) override {
    for (int i = 0; i < n; i++) {
      Simd::gf256_mul(src, 211, size);
    }
    td::do_not_optimize_away(dest[0]);
  }

 private:
  alignas(32) td::uint8 dest[size];
  alignas(32) td::uint8 src[size];
  std::string description_;
};

class GaussBenchmark : public td::Benchmark {
 public:
  GaussBenchmark(size_t n) : n_(n) {
    for (size_t i = 0; i < A_.rows(); i++) {
      for (size_t j = 0; j < A_.cols(); j++) {
        A_.set(i, j, td::Octet(td::uint8(td::Random::fast_uint32())));
      }
    }
    for (size_t i = 0; i < D_.rows(); i++) {
      for (size_t j = 0; j < D_.cols(); j++) {
        D_.set(i, j, td::Octet(td::uint8(td::Random::fast_uint32())));
      }
    }
  }
  std::string get_description() const override {
    return PSTRING() << "GaussBenchmark " << n_;
  }

  void run(int n) override {
    for (int j = 0; j < n; j++) {
      auto A = A_.copy();
      auto D = D_.copy();
      td::GaussianElimination::run(std::move(A), std::move(D));
    }
  }

 private:
  size_t n_;
  td::MatrixGF256 A_{n_, n_};
  td::MatrixGF256 D_{n_, n_ / 3};
};

class SolverBenchmark : public td::Benchmark {
 public:
  SolverBenchmark(size_t data_size, size_t symbol_size) {
    data_ = td::BufferSlice(td::rand_string('a', 'z', td::narrow_cast<int>(data_size)));
    symbol_size_ = symbol_size;
  }
  std::string get_description() const override {
    return PSTRING() << "SolverBenchmark " << data_.size() << " " << symbol_size_;
  }

  void run(int n) override {
    for (int j = 0; j < n; j++) {
      td::fec::RaptorQEncoder::create(data_.clone(), symbol_size_);
    }
  }

 private:
  td::BufferSlice data_;
  size_t symbol_size_;
};

template <class Encoder, class Decoder>
class FecBenchmark : public td::Benchmark {
 public:
  FecBenchmark(td::uint32 symbol_size, td::uint32 symbols_count, std::string name)
      : symbol_size_(symbol_size), symbols_count_(symbols_count), name_(std::move(name)) {
    data_ = td::BufferSlice(symbols_count_ * symbol_size_);
  }
  std::string get_description() const override {
    return PSTRING() << "FecBenchmark " << name_ << " " << td::tag("symbols_count", symbols_count_)
                     << td::tag("symbol_size", symbol_size_);
  }
  void run(int n) override {
    for (int i = 0; i < n; i++) {
      auto encoder = Encoder::create(data_.clone(), symbol_size_);

      std::vector<td::fec::Symbol> symbols;
      auto parameters = encoder->get_parameters();
      auto decoder = Decoder::create(parameters);

      size_t sent_symbols = 0;
      for (td::uint32 j = 0; j < data_.size() / symbol_size_ * 20; j++) {
        if (td::Random::fast(0, 5) != 0) {
          if (encoder->get_info().ready_symbol_count <= j) {
            encoder->prepare_more_symbols();
          }
          decoder->add_symbol(encoder->gen_symbol(j));
          sent_symbols++;
          if (decoder->may_try_decode()) {
            auto res = decoder->try_decode(false);
            if (res.is_ok()) {
              if (sent_symbols > static_cast<size_t>(static_cast<double>(parameters.symbols_count) * 1.05)) {
                LOG(ERROR) << sent_symbols << " / " << parameters.symbols_count;
              }
              //ASSERT_EQ(res.data.as_slice(), data);
              break;
            }
          }
        }
      }
    }
  }
  size_t symbol_size_;
  size_t symbols_count_;
  std::string name_;
  td::BufferSlice data_;
};

template <template <class T, size_t size> class O, size_t size = 256 * 8>
void bench_simd() {
  bench(O<td::Simd_null, size>("baseline"));
#if TD_SSSE3
  bench(O<td::Simd_sse, size>("SSE"));
#endif
#if TD_AVX2
  bench(O<td::Simd_avx, size>("AVX"));
#endif
}

void run_encode_benchmark() {
  constexpr size_t TARGET_TOTAL_BYTES = 100 * 1024 * 1024;
  constexpr size_t SYMBOLS_COUNT[11] = {10, 100, 250, 500, 1000, 2000, 4000, 10000, 20000, 40000, 56403};

  td::uint64 junk = 0;
  for (int it = 0; it < 11; it++) {
    //for (int it = 0; true;) {
    auto symbol_count = SYMBOLS_COUNT[it];
    auto symbol_size = 512;
    auto elements = symbol_count * symbol_size;
    td::BufferSlice data(elements);

    td::Random::Xorshift128plus rnd(123);
    for (auto &c : data.as_slice()) {
      c = static_cast<td::uint8>(rnd());
    }

    double now = td::Time::now();
    auto iterations = TARGET_TOTAL_BYTES / elements;
    for (size_t i = 0; i < iterations; i++) {
      auto encoder = td::fec::RaptorQEncoder::create(data.clone(), symbol_size);
      encoder->prepare_more_symbols();
      junk += encoder->gen_symbol(10000000).data.as_slice()[0];
    }
    double elapsed = td::Time::now() - now;
    double throughput = ((double)elements * (double)iterations * 8.0) / 1024 / 1024 / elapsed;
    fprintf(stderr, "symbol count = %d, encoded %d MB in %.3lfsecs, throughtput: %.1lfMbit/s\n", (int)symbol_count,
            (int)(elements * iterations / 1024 / 1024), elapsed, throughput);
  }
  td::do_not_optimize_away(junk);
}

int main(void) {
  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(ERROR));
  run_encode_benchmark();
  bench_simd<Simd_gf256_mul, 32>();
  bench_simd<Simd_gf256_add_mul, 32>();
  bench_simd<Simd_gf256_add, 32>();
  bench_simd<Simd_gf256_from_gf2, 32>();
  bench_simd<Simd_gf256_mul>();
  bench_simd<Simd_gf256_add_mul>();
  bench_simd<Simd_gf256_add>();
  bench_simd<Simd_gf256_from_gf2, 256>();
  bench(GaussBenchmark(15));
  bench(GaussBenchmark(1000));

  bench(FecBenchmark<td::fec::RaptorQEncoder, td::fec::RaptorQDecoder>(512, 20, "RaptorQ"));

  bench(FecBenchmark<td::fec::RaptorQEncoder, td::fec::RaptorQDecoder>(200, 1000, "RaptorQ"));
  bench(FecBenchmark<td::fec::OnlineEncoder, td::fec::OnlineDecoder>(200, 1000, "Online"));
  for (int symbol_size = 32; symbol_size <= 8192; symbol_size *= 2) {
    bench(FecBenchmark<td::fec::OnlineEncoder, td::fec::OnlineDecoder>(symbol_size, 50000, "Online"));
    bench(FecBenchmark<td::fec::RaptorQEncoder, td::fec::RaptorQDecoder>(symbol_size, 50000, "RaptorQ"));
  }

  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(WARNING));
  bench(SolverBenchmark(50000 * 200, 200));
  return 0;
}
