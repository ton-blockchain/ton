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

namespace td {
namespace raptorq {
class Encoder;
class Decoder;
}  // namespace raptorq
namespace online_code {
class Encoder;
class Decoder;
}  // namespace online_code
}  // namespace td

namespace td {

namespace fec {

struct Symbol {
  uint32 id;
  BufferSlice data;
};

class Encoder;

struct DataWithEncoder {
  BufferSlice data;
  std::unique_ptr<Encoder> encoder;
};

class Encoder {
 public:
  virtual Symbol gen_symbol(uint32 id) = 0;

  struct Info {
    uint32 symbol_count;
    uint32 ready_symbol_count;
  };

  virtual Info get_info() const {
    return Info{static_cast<uint32>(-1), static_cast<uint32>(-1)};
  }

  // Concurrent calls are forbidden
  virtual void prepare_more_symbols() {
  }

  // struct Parameters;
  // Parameters get_parameters();
  virtual ~Encoder() {
  }
};

class Decoder {
 public:
  virtual ~Decoder() {
  }
  virtual bool may_try_decode() const = 0;
  virtual Result<DataWithEncoder> try_decode(bool need_encoder) = 0;
  virtual Status add_symbol(Symbol symbol) = 0;
};

class RoundRobinEncoder : public Encoder {
 public:
  static std::unique_ptr<RoundRobinEncoder> create(BufferSlice data, size_t max_symbol_size);

  Symbol gen_symbol(uint32 id) override;

  struct Parameters {
    size_t data_size;
    size_t symbol_size;
    size_t symbols_count;
  };

  Parameters get_parameters() const;

  RoundRobinEncoder(BufferSlice data, size_t max_symbol_size);

 private:
  BufferSlice data_;
  size_t symbol_size_;
  size_t symbols_count_;
  size_t rounded_data_size_;
};

class RoundRobinDecoder : public Decoder {
 public:
  static std::unique_ptr<RoundRobinDecoder> create(RoundRobinEncoder::Parameters parameters);
  bool may_try_decode() const override;
  Result<DataWithEncoder> try_decode(bool need_encoder) override;
  Status add_symbol(Symbol symbol) override;
  RoundRobinDecoder(RoundRobinEncoder::Parameters parameters);

 private:
  BufferSlice data_;
  vector<bool> data_mask_;
  size_t left_symbols_;
  size_t symbol_size_;
  size_t symbols_count_;
};

class RaptorQEncoder : public Encoder {
 public:
  static std::unique_ptr<RaptorQEncoder> create(BufferSlice data, size_t max_symbol_size);

  Symbol gen_symbol(uint32 id) override;

  Info get_info() const override;
  void prepare_more_symbols() override;

  struct Parameters {
    size_t data_size;
    size_t symbol_size;
    size_t symbols_count;
  };

  Parameters get_parameters() const;

  RaptorQEncoder(std::unique_ptr<raptorq::Encoder> encoder);
  ~RaptorQEncoder();

 private:
  std::unique_ptr<raptorq::Encoder> encoder_;
};

class RaptorQDecoder : public Decoder {
 public:
  static std::unique_ptr<RaptorQDecoder> create(RaptorQEncoder::Parameters parameters);
  bool may_try_decode() const override;
  Result<DataWithEncoder> try_decode(bool need_encoder) override;
  Status add_symbol(Symbol symbol) override;
  RaptorQDecoder(std::unique_ptr<raptorq::Decoder> decoder);
  ~RaptorQDecoder();

 private:
  std::unique_ptr<raptorq::Decoder> decoder_;
  BufferSlice res_;
};

class OnlineEncoder : public Encoder {
 public:
  static std::unique_ptr<OnlineEncoder> create(BufferSlice data, size_t max_symbol_size);

  Symbol gen_symbol(uint32 id) override;

  struct Parameters {
    size_t data_size;
    size_t symbol_size;
    size_t symbols_count;
  };

  Parameters get_parameters() const;

  OnlineEncoder(std::unique_ptr<online_code::Encoder> encoder);
  ~OnlineEncoder();

 private:
  std::unique_ptr<online_code::Encoder> encoder_;
};

class OnlineDecoder : public Decoder {
 public:
  static std::unique_ptr<OnlineDecoder> create(OnlineEncoder::Parameters parameters);
  bool may_try_decode() const override;
  Result<DataWithEncoder> try_decode(bool need_encoder) override;
  Status add_symbol(Symbol symbol) override;
  OnlineDecoder(std::unique_ptr<online_code::Decoder> decoder, size_t symbol_size);
  ~OnlineDecoder();

 private:
  std::unique_ptr<online_code::Decoder> decoder_;
  size_t symbol_size_;
  BufferSlice res_;
};
}  // namespace fec

}  // namespace td
