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
#include "td/fec/raptorq/Encoder.h"

#include "td/fec/raptorq/Solver.h"

namespace td {
namespace raptorq {

Result<std::unique_ptr<Encoder>> Encoder::create(size_t symbol_size, BufferSlice data) {
  TRY_RESULT(p, Rfc::get_parameters((data.size() + symbol_size - 1) / symbol_size));
  return std::make_unique<Encoder>(p, symbol_size, std::move(data));
}

Encoder::Encoder(Rfc::Parameters p, size_t symbol_size, BufferSlice data, optional<RawEncoder> raw_encoder)
    : p_(p)
    , symbol_size_(symbol_size)
    , data_(std::move(data))
    , raw_encoder_{std::move(raw_encoder)}
    , has_encoder_{bool(raw_encoder_)} {
}

Encoder::Parameters Encoder::get_parameters() const {
  Parameters res;
  res.symbol_size = symbol_size_;
  res.data_size = data_.size();
  res.symbols_count = p_.K;
  return res;
}

Encoder::Info Encoder::get_info() const {
  Info res;
  res.symbol_count = 1 << 24;
  res.ready_symbol_count = has_precalc() ? res.symbol_count : p_.K;
  return res;
}

Status Encoder::gen_symbol(uint32 id, MutableSlice slice) {
  if (id < p_.K) {
    slice.copy_from(first_symbols_.symbols()[id].data);
    return Status::OK();
  }
  if (!has_precalc()) {
    return Status::Error("Precalc is not finished");
  }
  (*raw_encoder_).gen_symbol(id + p_.K_padded - p_.K, slice);
  return Status::OK();
}

bool Encoder::has_precalc() const {
  return has_encoder_.load(std::memory_order_acquire);
}

void Encoder::precalc() {
  if (has_precalc()) {
    return;
  }
  auto r_C = Solver::run(p_, first_symbols_.symbols());
  LOG_IF(FATAL, r_C.is_error()) << r_C.error();
  raw_encoder_ = RawEncoder(p_, r_C.move_as_ok());
  has_encoder_ = true;
}

}  // namespace raptorq
}  // namespace td
