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
#include "td/fec/fec.h"

#include "td/fec/raptorq/Encoder.h"
#include "td/fec/raptorq/Decoder.h"

#include "td/fec/online/Encoder.h"
#include "td/fec/online/Decoder.h"

namespace td {
namespace fec {
std::unique_ptr<RoundRobinEncoder> RoundRobinEncoder::create(BufferSlice data, size_t max_symbol_size) {
  CHECK(max_symbol_size > 0);
  return std::make_unique<RoundRobinEncoder>(std::move(data), max_symbol_size);
}

Symbol RoundRobinEncoder::gen_symbol(uint32 id) {
  id %= narrow_cast<uint32>(symbols_count_);
  auto offset = symbol_size_ * id;
  return Symbol{id, data_.from_slice(data_.as_slice().substr(offset).truncate(symbol_size_))};
};

RoundRobinEncoder::Parameters RoundRobinEncoder::get_parameters() const {
  Parameters parameters;
  parameters.symbol_size = symbol_size_;
  parameters.data_size = data_.size();
  parameters.symbols_count = symbols_count_;
  return parameters;
}

RoundRobinEncoder::RoundRobinEncoder(BufferSlice data, size_t max_symbol_size)
    : data_(std::move(data)), symbol_size_(max_symbol_size) {
  symbols_count_ = (data_.size() + symbol_size_ - 1) / symbol_size_;
  rounded_data_size_ = symbols_count_ * symbol_size_;
}

std::unique_ptr<RoundRobinDecoder> RoundRobinDecoder::create(RoundRobinEncoder::Parameters parameters) {
  return std::make_unique<RoundRobinDecoder>(parameters);
}
bool RoundRobinDecoder::may_try_decode() const {
  return left_symbols_ == 0;
}

Result<DataWithEncoder> RoundRobinDecoder::try_decode(bool need_encoder) {
  if (!may_try_decode()) {
    return Status::Error("Not ready");
  }
  std::unique_ptr<Encoder> encoder;
  if (need_encoder) {
    encoder = RoundRobinEncoder::create(data_.copy(), symbol_size_);
  }
  return DataWithEncoder{data_.clone(), std::move(encoder)};
}
Status RoundRobinDecoder::add_symbol(Symbol symbol) {
  if (symbol.data.size() != symbol_size_) {
    return Status::Error("Symbol has invalid length");
  }
  auto pos = symbol.id % symbols_count_;
  if (data_mask_[pos]) {
    return td::Status::OK();
  }
  data_mask_[pos] = true;
  auto offset = pos * symbol_size_;
  data_.as_slice().substr(offset).truncate(symbol_size_).copy_from(symbol.data.as_slice());
  left_symbols_--;
  return td::Status::OK();
}

RoundRobinDecoder::RoundRobinDecoder(RoundRobinEncoder::Parameters parameters)
    : data_(BufferSlice(parameters.data_size))
    , data_mask_(parameters.symbols_count, false)
    , left_symbols_(parameters.symbols_count)
    , symbol_size_(parameters.symbol_size)
    , symbols_count_(parameters.symbols_count) {
}

std::unique_ptr<RaptorQEncoder> RaptorQEncoder::create(BufferSlice data, size_t max_symbol_size) {
  auto encoder = raptorq::Encoder::create(max_symbol_size, std::move(data)).move_as_ok();
  return std::make_unique<RaptorQEncoder>(std::move(encoder));
}

Symbol RaptorQEncoder::gen_symbol(uint32 id) {
  BufferSlice data(encoder_->get_parameters().symbol_size);
  encoder_->gen_symbol(id, data.as_slice()).ensure();
  return Symbol{id, std::move(data)};
}

RaptorQEncoder::Info RaptorQEncoder::get_info() const {
  auto info = encoder_->get_info();
  return {info.symbol_count, info.ready_symbol_count};
}

void RaptorQEncoder::prepare_more_symbols() {
  encoder_->precalc();
}

RaptorQEncoder::Parameters RaptorQEncoder::get_parameters() const {
  Parameters res;
  auto p = encoder_->get_parameters();
  res.symbol_size = p.symbol_size;
  res.data_size = p.data_size;
  res.symbols_count = p.symbols_count;
  return res;
}

RaptorQEncoder::RaptorQEncoder(std::unique_ptr<raptorq::Encoder> encoder) : encoder_(std::move(encoder)) {
}
RaptorQEncoder::~RaptorQEncoder() = default;

std::unique_ptr<RaptorQDecoder> RaptorQDecoder::create(RaptorQEncoder::Parameters parameters) {
  raptorq::Encoder::Parameters p;
  p.symbol_size = parameters.symbol_size;
  p.data_size = parameters.data_size;
  p.symbols_count = parameters.symbols_count;
  return std::make_unique<RaptorQDecoder>(raptorq::Decoder::create(p).move_as_ok());
}

bool RaptorQDecoder::may_try_decode() const {
  return decoder_->may_try_decode();
}

Result<DataWithEncoder> RaptorQDecoder::try_decode(bool need_encoder) {
  TRY_RESULT(data_with_encoder, decoder_->try_decode(need_encoder));
  DataWithEncoder res;
  res.data = std::move(data_with_encoder.data);
  res.encoder = std::make_unique<RaptorQEncoder>(std::move(data_with_encoder.encoder));
  return std::move(res);
}

Status RaptorQDecoder::add_symbol(Symbol symbol) {
  return decoder_->add_symbol({symbol.id, symbol.data.as_slice()});
}
RaptorQDecoder::RaptorQDecoder(std::unique_ptr<raptorq::Decoder> decoder) : decoder_(std::move(decoder)) {
}
RaptorQDecoder::~RaptorQDecoder() = default;

std::unique_ptr<OnlineEncoder> OnlineEncoder::create(BufferSlice data, size_t max_symbol_size) {
  auto encoder = online_code::Encoder::create(max_symbol_size, std::move(data)).move_as_ok();
  return std::make_unique<OnlineEncoder>(std::move(encoder));
}

Symbol OnlineEncoder::gen_symbol(uint32 id) {
  BufferSlice data(encoder_->get_parameters().symbol_size);
  encoder_->gen_symbol(id, data.as_slice());
  return Symbol{id, std::move(data)};
}

OnlineEncoder::Parameters OnlineEncoder::get_parameters() const {
  Parameters res;
  auto p = encoder_->get_parameters();
  res.symbol_size = p.symbol_size;
  res.data_size = p.data_size;
  res.symbols_count = p.symbols_count;

  return res;
}

OnlineEncoder::OnlineEncoder(std::unique_ptr<online_code::Encoder> encoder) : encoder_(std::move(encoder)) {
}
OnlineEncoder::~OnlineEncoder() = default;

std::unique_ptr<OnlineDecoder> OnlineDecoder::create(OnlineEncoder::Parameters parameters) {
  online_code::Encoder::Parameters p;
  p.symbol_size = parameters.symbol_size;
  p.data_size = parameters.data_size;
  p.symbols_count = parameters.symbols_count;
  return std::make_unique<OnlineDecoder>(online_code::Decoder::create(p).move_as_ok(), parameters.symbol_size);
}

bool OnlineDecoder::may_try_decode() const {
  return decoder_->is_ready();
}

Result<DataWithEncoder> OnlineDecoder::try_decode(bool need_encoder) {
  if (!may_try_decode()) {
    return Status::Error("Not ready yet");
  }
  std::unique_ptr<Encoder> encoder;
  auto data = decoder_->get_data();
  if (need_encoder) {
    encoder = RoundRobinEncoder::create(data.copy(), symbol_size_);
  }
  return DataWithEncoder{std::move(data), nullptr};
}

Status OnlineDecoder::add_symbol(Symbol symbol) {
  return decoder_->add_symbol({symbol.id, symbol.data.as_slice()});
}
OnlineDecoder::OnlineDecoder(std::unique_ptr<online_code::Decoder> decoder, size_t symbol_size)
    : decoder_(std::move(decoder)), symbol_size_(symbol_size) {
}
OnlineDecoder::~OnlineDecoder() = default;

}  // namespace fec
}  // namespace td
