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

#include "td/utils/optional.h"

#include "fec/fec.h"

#include "RldpReceiver.h"

#include <map>

namespace ton {
namespace rldp2 {
struct InboundTransfer {
  struct Part {
    std::unique_ptr<td::fec::Decoder> decoder;
    RldpReceiver receiver;
    size_t offset;
  };

  explicit InboundTransfer(size_t total_size) : data_(total_size) {
  }

  size_t total_size() const;
  std::map<td::uint32, Part> &parts();
  bool is_part_completed(td::uint32 part_i);
  td::Result<Part *> get_part(td::uint32 part_i, const ton::fec::FecType &fec_type);
  void finish_part(td::uint32 part_i, td::Slice data);
  td::optional<td::Result<td::BufferSlice>> try_finish();

 private:
  std::map<td::uint32, Part> parts_;
  td::uint32 next_part_{0};
  size_t offset_{0};
  td::BufferSlice data_;
};
}  // namespace rldp2
}  // namespace ton
