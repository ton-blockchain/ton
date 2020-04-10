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
//#include "fec.h"
//#include "raptorq/Solver.h"
#include "td/fec/fec.h"
#include "td/fec/raptorq/Encoder.h"
#include "td/fec/raptorq/Decoder.h"
#if USE_LIBRAPTORQ
#include "LibRaptorQ.h"
#endif
#include "td/utils/tests.h"

#include <string>
td::Slice get_long_string() {
  const size_t max_symbol_size = 200;
  const size_t symbols_count = 100;
  static std::string data = td::rand_string('a', 'z', max_symbol_size * symbols_count);
  return data;
}

TEST(Fec, Simd) {
  constexpr size_t size = td::Simd::alignment() * 1024;
  alignas(td::Simd::alignment()) td::uint8 a[size];
  alignas(td::Simd::alignment()) td::uint8 a_copy[size];
  alignas(td::Simd::alignment()) td::uint8 b[size];
  alignas(td::Simd::alignment()) td::uint8 d[8 * size];

  td::Random::Xorshift128plus rnd(123);
  for (auto k_size : {1, 2, 10, 1024}) {
    auto a_size = k_size * td::Simd::alignment();
    LOG(ERROR) << a_size;
    for (size_t i = 0; i < a_size; i++) {
      a_copy[i] = rnd() & 255;
      b[i] = rnd() & 255;
    }

    std::vector<std::string> res;
    std::vector<std::string> other_res;
    bool is_first = true;

    auto save_str = [&](auto str) {
      if (is_first) {
        res.push_back(str);
      } else {
        other_res.push_back(str);
      }
    };
    auto save_a = [&] { save_str(td::Slice(a, a_size).str()); };
    auto save_d = [&] { save_str(td::Slice(d, a_size * 8).str()); };

    auto run = [&](auto simd) {
      LOG(ERROR) << simd.get_name();
      std::memcpy(a, a_copy, a_size);
      simd.gf256_add(a, b, a_size);
      save_a();

      for (td::uint32 o = 0; o < 256; o++) {
        std::memcpy(a, a_copy, a_size);
        simd.gf256_add_mul(a, b, td::uint8(o), a_size);
        save_a();
        std::memcpy(a, a_copy, a_size);
        simd.gf256_mul(a, td::uint8(o), a_size);
        save_a();
      }
      std::memcpy(a, a_copy, a_size);
      simd.gf256_from_gf2(d, a, a_size);
      save_d();

      if (is_first) {
        is_first = false;
      } else {
        CHECK(res == other_res);
        other_res.clear();
      }
    };
    run(td::Simd_null());
#if TD_SSE3
    run(td::Simd_sse());
#endif
#if TD_AVX2
    run(td::Simd_avx());
#endif
    run(td::Simd());
  }
}

static const td::Slice tmp = get_long_string();
TEST(Fec, RaptorQFirstSymbols) {
  auto data = get_long_string();
  auto encoder = td::raptorq::Encoder::create(200, td::BufferSlice(data)).move_as_ok();

  auto parameters = encoder->get_parameters();
  auto decoder = td::raptorq::Decoder::create(parameters).move_as_ok();
  std::string symbol(parameters.symbol_size, '\0');
  std::string new_symbol(parameters.symbol_size, '\0');

  encoder->precalc();
  for (td::uint32 i = 0; i < 2; i++) {
    encoder->gen_symbol(i + (1 << 21), symbol);
    decoder->add_symbol({i + (1 << 21), td::Slice(symbol)});
  }

  for (td::uint32 i = 0; i < parameters.symbols_count; i++) {
    td::uint32 id = i;
    encoder->gen_symbol(id, symbol);
    decoder->add_symbol({id, td::Slice(symbol)});
    if (decoder->may_try_decode()) {
      auto r = decoder->try_decode(true);
      if (r.is_ok()) {
        ASSERT_EQ(r.ok().data, data);
        auto new_encoder = std::move(r.move_as_ok().encoder);
        new_encoder->precalc();
        auto check_id = [&](td::uint32 id) {
          encoder->gen_symbol(id, symbol);
          new_encoder->gen_symbol(id, new_symbol);
          ASSERT_EQ(symbol, new_symbol);
        };
        check_id(0);
        check_id(1);
        check_id(1000000);
        LOG(ERROR) << "ok";
        return;
      } else {
        LOG(WARNING) << "SKIP";
      }
    }
  }
  UNREACHABLE();
}

TEST(Fec, RaptorQRandomSymbols) {
  auto data = get_long_string();
  auto encoder = td::raptorq::Encoder::create(200, td::BufferSlice(data)).move_as_ok();
  encoder->precalc();

  auto parameters = encoder->get_parameters();
  auto decoder = td::raptorq::Decoder::create(parameters).move_as_ok();
  std::string symbol(parameters.symbol_size, '\0');
  for (size_t i = 0; i < parameters.symbols_count + 10; i++) {
    auto id = td::Random::fast_uint32();
    encoder->gen_symbol(id, symbol);
    decoder->add_symbol({id, td::Slice(symbol)});
    if (decoder->may_try_decode()) {
      auto r = decoder->try_decode(false);
      if (r.is_ok()) {
        ASSERT_EQ(r.ok().data, data);
        return;
      }
    }
  }
  UNREACHABLE();
}

template <class Encoder, class Decoder>
void fec_test(td::Slice data, size_t max_symbol_size) {
  LOG(ERROR) << "!";
  auto encoder = Encoder::create(td::BufferSlice(data), max_symbol_size);

  LOG(ERROR) << "?";
  std::vector<td::fec::Symbol> symbols;
  auto parameters = encoder->get_parameters();
  auto decoder = Decoder::create(parameters);

  LOG(ERROR) << "?";
  size_t sent_symbols = 0;
  for (td::uint32 i = 0; i < data.size() / max_symbol_size * 20; i++) {
    if (td::Random::fast(0, 5) != 0) {
      if (encoder->get_info().ready_symbol_count <= i) {
        encoder->prepare_more_symbols();
      }
      decoder->add_symbol(encoder->gen_symbol(i));
      sent_symbols++;
      if (decoder->may_try_decode()) {
        auto res = decoder->try_decode(false);
        if (res.is_ok()) {
          ASSERT_EQ(res.ok().data.as_slice(), data);
          LOG(ERROR) << sent_symbols << " / " << parameters.symbols_count;
          return;
        }
      }
    }
  }
  UNREACHABLE();
}

TEST(Fec, RoundRobin) {
  const size_t max_symbol_size = 200;
  std::string data = td::rand_string('a', 'z', max_symbol_size * 400);
  fec_test<td::fec::RoundRobinEncoder, td::fec::RoundRobinDecoder>(data, max_symbol_size);
}

TEST(Fec, Online) {
  const size_t max_symbol_size = 200;
  std::string data = td::rand_string('a', 'z', max_symbol_size * 50000);
  fec_test<td::fec::OnlineEncoder, td::fec::OnlineDecoder>(data, max_symbol_size);
}

#if USE_LIBRAPTORQ
TEST(Fec, SlowRaptorQ) {
  const size_t max_symbol_size = 200;
  std::string data = td::rand_string('a', 'z', max_symbol_size * 200);
  fec_test<td::fec::SlowRaptorQEncoder, td::fec::SlowRaptorQDecoder>(data, max_symbol_size);
}
#endif

TEST(Fec, RaptorQFull) {
  const size_t max_symbol_size = 200;
  std::string data = td::rand_string('a', 'z', max_symbol_size * 50000);
  fec_test<td::fec::RaptorQEncoder, td::fec::RaptorQDecoder>(data, max_symbol_size);
}

#if USE_LIBRAPTORQ
TEST(Fec, RaptorQEncoder) {
  const size_t max_symbol_size = 200;
  std::string data = td::rand_string('a', 'z', max_symbol_size * 200);
  auto reference_encoder = td::fec::SlowRaptorQEncoder::create(td::BufferSlice(data), max_symbol_size);
  auto checked_encoder = td::fec::RaptorQEncoder::create(td::BufferSlice(data), max_symbol_size);
  reference_encoder->prepare_more_symbols();
  checked_encoder->prepare_more_symbols();
  for (td::uint32 i = 0; i < 1000000; i++) {
    auto reference_symbol = reference_encoder->gen_symbol(i);
    auto checked_symbol = checked_encoder->gen_symbol(i);
    ASSERT_EQ(reference_symbol.data.as_slice(), checked_symbol.data.as_slice());
  }
}
#endif
