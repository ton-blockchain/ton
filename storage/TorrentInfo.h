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

#include "td/utils/Slice.h"
#include "td/utils/UInt.h"

#include "vm/cells.h"
#include "td/utils/optional.h"

namespace ton {
// torrent_info piece_size:uint32 file_size:uint64 root_hash:(## 256) header_size:uint64 header_hash:(## 256)
//              description:Text = TorrentInfo;
struct TorrentInfo {
  td::uint32 piece_size{768 * 128};
  td::uint64 file_size{0};
  td::Bits256 root_hash;
  td::uint64 header_size;
  td::Bits256 header_hash;
  std::string description;

  bool pack(vm::CellBuilder &cb) const;
  bool unpack(vm::CellSlice &cs);

  void init_cell();
  td::Bits256 get_hash() const;
  td::Ref<vm::Cell> as_cell() const;

  struct PieceInfo {
    td::uint64 offset;
    td::uint64 size;
  };

  td::uint64 pieces_count() const;
  PieceInfo get_piece_info(td::uint64 piece_i) const;

  td::Status validate() const;

 private:
  td::Ref<vm::Cell> cell_;
};

}  // namespace ton
