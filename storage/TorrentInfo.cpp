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

#include "TorrentInfo.h"

#include "vm/cells/CellString.h"
#include "vm/cellslice.h"

#include "td/utils/misc.h"

namespace ton {
bool TorrentInfo::pack(vm::CellBuilder &cb) const {
  return cb.store_long_bool(piece_size, 32) && cb.store_long_bool(file_size, 64) && cb.store_bits_bool(root_hash) &&
         cb.store_long_bool(header_size, 64) && cb.store_bits_bool(header_hash) &&
         vm::CellText::store(cb, description).is_ok();
}

bool TorrentInfo::unpack(vm::CellSlice &cs) {
  return cs.fetch_uint_to(32, piece_size) && cs.fetch_uint_to(64, file_size) && cs.fetch_bits_to(root_hash) &&
         cs.fetch_uint_to(64, header_size) && cs.fetch_bits_to(header_hash) && vm::CellText::fetch_to(cs, description);
}

td::Bits256 TorrentInfo::get_hash() const {
  return as_cell()->get_hash().bits();
}

void TorrentInfo::init_cell() {
  vm::CellBuilder cb;
  CHECK(pack(cb));
  cell_ = cb.finalize();
}

td::Ref<vm::Cell> TorrentInfo::as_cell() const {
  CHECK(cell_.not_null())
  return cell_;
}

td::uint64 TorrentInfo::pieces_count() const {
  return (file_size + piece_size - 1) / piece_size;
}

TorrentInfo::PieceInfo TorrentInfo::get_piece_info(td::uint64 piece_i) const {
  PieceInfo info;
  info.offset = piece_size * piece_i;
  CHECK(info.offset < file_size);
  info.size = td::min(static_cast<td::uint64>(piece_size), file_size - info.offset);
  return info;
}

td::Status TorrentInfo::validate() const {
  if (piece_size == 0) {
    return td::Status::Error("Piece size is 0");
  }
  if (header_size > file_size) {
    return td::Status::Error("Header is too big");
  }
  if (description.size() > 1024) {
    return td::Status::Error("Description is too long");
  }
  return td::Status::OK();
}
}  // namespace ton
