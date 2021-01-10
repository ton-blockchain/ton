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
#include "td/fec/raptorq/Decoder.h"

namespace td {
namespace raptorq {
Result<std::unique_ptr<Decoder>> Decoder::create(Encoder::Parameters p) {
  TRY_RESULT(rfc_p, Rfc::get_parameters(p.symbols_count));
  return std::make_unique<Decoder>(rfc_p, p.symbol_size, p.data_size);
}
Decoder::Decoder(const Rfc::Parameters &p, size_t symbol_size, size_t data_size) : p_(p) {
  mask_ = vector<bool>(p.K, false);
  mask_size_ = 0;
  data_ = BufferSlice(p.K * symbol_size);
  data_size_ = data_size;
  symbol_size_ = symbol_size;
}

bool Decoder::may_try_decode() const {
  return may_decode_;
}

Status Decoder::add_symbol(SymbolRef symbol) {
  if (symbol.data.size() != symbol_size_) {
    return Status::Error("Symbol has invalid length");
  }
  if (symbol.id < p_.K) {
    add_small_symbol(symbol);
    return Status::OK();
  }
  if (mask_size_ + slow_symbols_set_.size() >= p_.K + 10) {
    return Status::OK();
  }
  add_big_symbol(symbol);
  return Status::OK();
}

Result<Decoder::DataWithEncoder> Decoder::try_decode(bool need_encoder) {
  if (!may_decode_) {
    return Status::Error("Need more symbols");
  }

  optional<RawEncoder> raw_encoder;
  if (mask_size_ < p_.K) {
    flush_symbols();
    may_decode_ = false;
    TRY_RESULT(C, Solver::run(p_, symbols_));
    raw_encoder = RawEncoder(p_, std::move(C));
    for (uint32 i = 0; i < p_.K; i++) {
      if (!mask_[i]) {
        (*raw_encoder).gen_symbol(i, data_.as_slice().substr(i * symbol_size_, symbol_size_));
        mask_[i] = true;
        mask_size_++;
      }
    }
  }

  auto data = data_.from_slice(data_.as_slice().truncate(data_size_));

  std::unique_ptr<Encoder> encoder;
  if (need_encoder) {
    encoder = std::make_unique<Encoder>(p_, symbol_size_, data.copy(), std::move(raw_encoder));
  }

  return DataWithEncoder{std::move(data), std::move(encoder)};
}

void Decoder::add_small_symbol(SymbolRef symbol) {
  if (mask_[symbol.id]) {
    return;
  }
  mask_size_++;
  mask_[symbol.id] = true;
  auto slice = data_.as_slice().substr(symbol.id * symbol_size_, symbol_size_);
  slice.copy_from(symbol.data);

  if (flush_symbols_) {
    symbols_.push_back({symbol.id, slice});
  }
  update_may_decode();
}

void Decoder::add_big_symbol(SymbolRef symbol) {
  if (!slow_path_) {
    on_first_slow_path();
  }
  symbol.id += p_.K_padded - p_.K;

  if (slow_symbols_set_.size() == slow_symbols_) {
    // Got at least p.K + 10 different symbols
    return;
  }
  size_t offset = slow_symbols_set_.size() * symbol_size_;
  if (!slow_symbols_set_.insert(symbol.id).second) {
    return;
  }
  auto slice = buffer_.as_slice().substr(offset, symbol_size_);
  slice.copy_from(symbol.data);
  symbols_.push_back({symbol.id, slice});
  update_may_decode();
}

void Decoder::update_may_decode() {
  size_t total_symbols = mask_size_ + slow_symbols_set_.size();
  if (total_symbols < p_.K) {
    return;
  }
  may_decode_ = true;
}

void Decoder::on_first_slow_path() {
  slow_path_ = true;

  slow_symbols_ = p_.K + 10 - mask_size_;
  buffer_ = BufferSlice(slow_symbols_ * symbol_size_);
  symbols_.reserve(p_.K + 10);
}

void Decoder::flush_symbols() {
  if (flush_symbols_) {
    return;
  }
  flush_symbols_ = true;
  zero_symbol_ = std::string(symbol_size_, '\0');
  for (uint32 i = p_.K; i < p_.K_padded; i++) {
    symbols_.push_back({i, Slice(zero_symbol_)});
  }
  for (uint32 i = 0; i < p_.K; i++) {
    if (mask_[i]) {
      symbols_.push_back({i, data_.as_slice().substr(i * symbol_size_, symbol_size_)});
    }
  }
}
}  // namespace raptorq
}  // namespace td
