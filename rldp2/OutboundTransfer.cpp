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

#include "OutboundTransfer.h"

namespace ton {
namespace rldp2 {
size_t OutboundTransfer::total_size() const {
  return data_.size();
}
std::map<td::uint32, OutboundTransfer::Part> &OutboundTransfer::parts(const RldpSender::Config &config) {
  while (parts_.size() < 20) {
    auto offset = next_part_ * part_size();
    if (offset >= data_.size()) {
      break;
    }
    td::BufferSlice D = data_.from_slice(data_.as_slice().substr(offset).truncate(part_size()));
    ton::fec::FecType fec_type = td::fec::RaptorQEncoder::Parameters{D.size(), symbol_size(), 0};
    auto encoder = fec_type.create_encoder(std::move(D)).move_as_ok();
    auto symbols_count = fec_type.symbols_count();
    parts_.emplace(next_part_, Part{std::move(encoder), RldpSender(config, symbols_count), std::move(fec_type)});
    next_part_++;
  }
  return parts_;
}

void OutboundTransfer::drop_part(td::uint32 part_i) {
  parts_.erase(part_i);
}

OutboundTransfer::Part *OutboundTransfer::get_part(td::uint32 part_i) {
  auto it = parts_.find(part_i);
  if (it == parts_.end()) {
    return nullptr;
  }
  return &it->second;
}

bool OutboundTransfer::is_done() const {
  return next_part_ * part_size() >= data_.size() && parts_.empty();
}
}  // namespace rldp2
}  // namespace ton
