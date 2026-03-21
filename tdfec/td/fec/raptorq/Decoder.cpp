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
  small_symbols_mask_.assign(p.K, false);
  data_size_ = data_size;
  symbol_size_ = symbol_size;
}

bool Decoder::may_try_decode() const {
  return may_decode_;
}

Status Decoder::add_symbol(fec::Symbol symbol) {
  if (symbol.data.size() != symbol_size_) {
    return Status::Error("Symbol has invalid length");
  }
  if (symbol.id >= (1 << 24)) {
    return Status::Error("Too big symbol id");
  }
  if (result_) {
    return Status::OK();
  }
  if (symbol.id < p_.K) {
    if (small_symbols_mask_[symbol.id]) {
      return Status::OK();
    }
    small_symbols_mask_[symbol.id] = true;
    ++small_symbols_count_;
  } else {
    if (symbols_.size() >= p_.K + 10) {
      return Status::OK();
    }
    symbol.id += p_.K_padded - p_.K;
    if (!big_symbols_set_.insert(symbol.id).second) {
      return Status::OK();
    }
  }
  if (made_symbol_refs_) {
    symbol_refs_.push_back({symbol.id, symbol.data.as_slice()});
  }
  symbols_.push_back(std::move(symbol));
  may_decode_ = symbols_.size() >= p_.K;
  return Status::OK();
}

Result<Decoder::DataWithEncoder> Decoder::try_decode(bool need_encoder) {
  if (!may_decode_) {
    return Status::Error("Need more symbols");
  }

  optional<RawEncoder> raw_encoder;
  if (!result_) {
    BufferSlice data(data_size_);
    if (small_symbols_count_ == p_.K) {
      for (auto &[i, s] : symbols_) {
        if (i < p_.K) {
          MutableSlice to =
              data.as_slice().substr(i * symbol_size_, std::min(symbol_size_, data_size_ - i * symbol_size_));
          to.copy_from(s.as_slice().substr(0, to.size()));
        }
      }
    } else {
      make_symbol_refs();
      may_decode_ = false;
      TRY_RESULT(C, Solver::run(p_, symbol_refs_));
      raw_encoder = RawEncoder(p_, std::move(C));
      for (uint32 i = 0; i < p_.K; i++) {
        (*raw_encoder)
            .gen_symbol(
                i, data.as_slice().substr(i * symbol_size_, std::min(symbol_size_, data_size_ - i * symbol_size_)));
      }
    }
    result_ = std::move(data);
  }

  BufferSlice data = result_.value().clone();
  std::unique_ptr<Encoder> encoder;
  if (need_encoder) {
    encoder = std::make_unique<Encoder>(p_, symbol_size_, data.copy(), std::move(raw_encoder));
  }

  return DataWithEncoder{std::move(data), std::move(encoder)};
}

void Decoder::make_symbol_refs() {
  if (made_symbol_refs_) {
    return;
  }
  made_symbol_refs_ = true;
  for (auto &[i, s] : symbols_) {
    symbol_refs_.push_back({i, s.as_slice()});
  }
  zero_symbol_ = BufferSlice(symbol_size_);
  zero_symbol_.as_slice().fill('\0');
  for (uint32 i = p_.K; i < p_.K_padded; i++) {
    symbol_refs_.push_back({i, zero_symbol_.as_slice()});
  }
}
}  // namespace raptorq
}  // namespace td
