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
#include "LibRaptorQ.h"
#include "RaptorQ/RaptorQ_v1_hdr.hpp"

namespace RaptorQ = RaptorQ__v1;

namespace td {
namespace fec {
class SlowRaptorQEncoder::Impl {
 public:
  Impl(BufferSlice data, size_t max_symbol_size)
      : encoder_(create_encoder(data.size(), max_symbol_size)), data_(std::move(data)), symbol_size_(max_symbol_size) {
    encoder_->set_data(data_.as_slice().ubegin(), data_.as_slice().uend());
  }
  Symbol gen_symbol(uint32 id) {
    BufferSlice symbol_data(symbol_size_);
    auto *begin = symbol_data.as_slice().ubegin();
    auto *end = symbol_data.as_slice().uend();
    encoder_->encode(begin, end, id);
    return {id, symbol_data.from_slice(Slice(symbol_data.as_slice().ubegin(), begin))};
  }

  Parameters get_parameters() const {
    return Parameters{encoder_->symbols(), encoder_->symbol_size(), data_.size()};
  }

  Info get_info() const {
    return {1 << 24, computed_ ? encoder_->max_repair() : encoder_->symbols()};
  }

  void prepare_more_symbols() {
    encoder_->compute_sync();
    computed_ = true;
  }

 private:
  using Encoder = RaptorQ::Encoder<uint8 *, uint8 *>;
  std::unique_ptr<Encoder> encoder_;
  BufferSlice data_;
  size_t symbol_size_;
  bool computed_{false};

  static std::unique_ptr<Encoder> create_encoder(size_t data_size, size_t symbol_size) {
    auto min_symbols = (data_size + symbol_size - 1) / symbol_size;
    RaptorQ::Block_Size block = RaptorQ::Block_Size::Block_10;
    for (auto blk : *RaptorQ::blocks) {
      // RaptorQ::blocks is a pointer to an array, just scan it to find your
      // block.
      if (static_cast<uint16>(blk) >= min_symbols) {
        block = blk;
        break;
      }
    }
    CHECK(static_cast<uint16>(block) >= min_symbols);
    return std::make_unique<Encoder>(block, symbol_size);
  }
};
class SlowRaptorQDecoder::Impl {
 public:
  using Decoder = RaptorQ::Decoder<uint8 *, uint8 *>;
  Impl(SlowRaptorQEncoder::Parameters parameters)
      : decoder_(RaptorQ::Block_Size(parameters.symbols_count), parameters.symbol_size, Decoder::Report::COMPLETE)
      , data_size_(parameters.data_size) {
  }

  bool may_try_decode() const {
    return decoder_.can_decode();
  }
  Result<DataWithEncoder> try_decode(bool need_encoder) {
    decoder_.decode_once();
    if (!decoder_.ready()) {
      return Status::Error("Not ready");
    }

    BufferSlice data(data_size_);
    auto begin = data.as_slice().ubegin();
    decoder_.decode_bytes(begin, data.as_slice().uend(), 0, 0);

    std::unique_ptr<Encoder> encoder;
    if (need_encoder) {
      encoder = SlowRaptorQEncoder::create(data.copy(), decoder_.symbol_size());
    }

    return DataWithEncoder{std::move(data), std::move(encoder)};
  }
  void add_symbol(Symbol symbol) {
    auto begin = symbol.data.as_slice().ubegin();
    decoder_.add_symbol(begin, symbol.data.as_slice().uend(), symbol.id);
  }

 private:
  Decoder decoder_;
  size_t data_size_;
};

SlowRaptorQEncoder::~SlowRaptorQEncoder() = default;
std::unique_ptr<SlowRaptorQEncoder> SlowRaptorQEncoder::create(BufferSlice data, size_t max_symbol_size) {
  return std::make_unique<SlowRaptorQEncoder>(std::move(data), max_symbol_size);
}

SlowRaptorQEncoder::SlowRaptorQEncoder(BufferSlice data, size_t max_symbol_size) {
  impl_ = std::make_unique<Impl>(std::move(data), max_symbol_size);
}

Symbol SlowRaptorQEncoder::gen_symbol(uint32 id) {
  return impl_->gen_symbol(id);
}

SlowRaptorQEncoder::Parameters SlowRaptorQEncoder::get_parameters() const {
  return impl_->get_parameters();
}

Encoder::Info SlowRaptorQEncoder::get_info() const {
  return impl_->get_info();
}

void SlowRaptorQEncoder::prepare_more_symbols() {
  impl_->prepare_more_symbols();
}

SlowRaptorQDecoder::~SlowRaptorQDecoder() = default;
std::unique_ptr<SlowRaptorQDecoder> SlowRaptorQDecoder::create(SlowRaptorQEncoder::Parameters parameters) {
  return std::make_unique<SlowRaptorQDecoder>(std::move(parameters));
}
bool SlowRaptorQDecoder::may_try_decode() const {
  return impl_->may_try_decode();
}

Result<DataWithEncoder> SlowRaptorQDecoder::try_decode(bool need_encoder) {
  return impl_->try_decode(need_encoder);
}
void SlowRaptorQDecoder::add_symbol(Symbol symbol) {
  impl_->add_symbol(std::move(symbol));
}

SlowRaptorQDecoder::SlowRaptorQDecoder(SlowRaptorQEncoder::Parameters parameters) {
  impl_ = std::make_unique<Impl>(std::move(parameters));
}
}  // namespace fec
}  // namespace td
