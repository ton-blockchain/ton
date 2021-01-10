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
#pragma once

#include "td/utils/buffer.h"
#include "td/fec/fec.h"

namespace td {
namespace fec {

class SlowRaptorQEncoder : public Encoder {
 public:
  ~SlowRaptorQEncoder();
  static std::unique_ptr<SlowRaptorQEncoder> create(BufferSlice data, size_t max_symbol_size);

  Symbol gen_symbol(uint32 id) override;
  Info get_info() const override;
  void prepare_more_symbols() override;

  struct Parameters {
    size_t symbols_count;
    size_t symbol_size;
    size_t data_size;
  };
  SlowRaptorQEncoder(BufferSlice data, size_t max_symbol_size);

  Parameters get_parameters() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class SlowRaptorQDecoder : public Decoder {
 public:
  ~SlowRaptorQDecoder();
  static std::unique_ptr<SlowRaptorQDecoder> create(SlowRaptorQEncoder::Parameters parameters);

  bool may_try_decode() const override;
  Result<DataWithEncoder> try_decode(bool need_encoder) override;

  void add_symbol(Symbol symbol) override;

  SlowRaptorQDecoder(SlowRaptorQEncoder::Parameters parameters);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fec
}  // namespace td
