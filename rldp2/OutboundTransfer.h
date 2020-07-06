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

#include "RldpSender.h"
#include "fec/fec.h"

#include <map>

namespace ton {
namespace rldp2 {
struct OutboundTransfer {
 public:
  struct Part {
    std::unique_ptr<td::fec::Encoder> encoder;
    RldpSender sender;
    ton::fec::FecType fec_type;
  };

  OutboundTransfer(td::BufferSlice data) : data_(std::move(data)) {
  }

  size_t total_size() const;
  std::map<td::uint32, Part> &parts(const RldpSender::Config &config);
  void drop_part(td::uint32 part_i);
  Part *get_part(td::uint32 part_i);
  bool is_done() const;

 private:
  td::BufferSlice data_;
  std::map<td::uint32, Part> parts_;
  td::uint32 next_part_{0};

  static size_t part_size() {
    return 2000000;
  }
  static size_t symbol_size() {
    return 768;
  }
};
}  // namespace rldp2
}  // namespace ton
