#include "TorrentInfo.h"

#include "vm/cells/CellString.h"
#include "vm/cellslice.h"

#include "td/utils/misc.h"

namespace ton {
bool TorrentInfo::pack(vm::CellBuilder &cb) const {
  return cb.store_long_bool(depth, 32) && cb.store_long_bool(piece_size, 32) && cb.store_long_bool(file_size, 64) &&
         cb.store_bits_bool(root_hash) && cb.store_long_bool(header_size, 64) && cb.store_bits_bool(header_hash) &&
         vm::CellText::store(cb, description).is_ok();
}

bool TorrentInfo::unpack(vm::CellSlice &cs) {
  return cs.fetch_uint_to(32, depth) && cs.fetch_uint_to(32, piece_size) && cs.fetch_uint_to(64, file_size) &&
         cs.fetch_bits_to(root_hash) && cs.fetch_uint_to(64, header_size) && cs.fetch_bits_to(header_hash) &&
         vm::CellText::fetch_to(cs, description);
}

vm::Cell::Hash TorrentInfo::get_hash() const {
  return as_cell()->get_hash();
}

void TorrentInfo::init_cell() {
  vm::CellBuilder cb;
  CHECK(pack(cb));
  cell_ = cb.finalize();
}

td::Ref<vm::Cell> TorrentInfo::as_cell() const {
  CHECK(cell_.not_null());
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
}  // namespace ton
