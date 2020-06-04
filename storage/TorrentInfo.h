#pragma once

#include "td/utils/Slice.h"
#include "td/utils/UInt.h"

#include "vm/cells.h"

namespace ton {
// torrent_info depth:# piece_size:uint32 file_size:uint64 root_hash:(## 256) header_size:uint64 header_hash:(## 256) description:Text = TorrentInfo;
struct TorrentInfo {
  td::uint32 depth{0};
  td::uint32 piece_size{768 * 128};
  td::uint64 file_size{0};
  td::Bits256 root_hash;
  td::uint64 header_size;
  td::Bits256 header_hash;
  std::string description;

  bool pack(vm::CellBuilder &cb) const;
  bool unpack(vm::CellSlice &cs);

  void init_cell();
  vm::Cell::Hash get_hash() const;
  td::Ref<vm::Cell> as_cell() const;

  struct PieceInfo {
    td::uint64 offset;
    td::uint64 size;
  };

  td::uint64 pieces_count() const;
  PieceInfo get_piece_info(td::uint64 piece_i) const;

 private:
  td::Ref<vm::Cell> cell_;
};

}  // namespace ton
