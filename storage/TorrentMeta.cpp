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

#include "TorrentMeta.h"

#include "TorrentHeader.hpp"

#include "td/utils/crypto.h"
#include "td/utils/tl_helpers.h"
#include "td/utils/UInt.h"

#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cellslice.h"

namespace ton {

td::Result<TorrentMeta> TorrentMeta::deserialize(td::Slice data) {
  TorrentMeta res;
  TRY_STATUS(td::unserialize(res, data));
  if (res.header) {
    td::Bits256 header_hash;
    td::sha256(td::serialize(res.header.value()), header_hash.as_slice());
    if (header_hash != res.info.header_hash) {
      return td::Status::Error("Header hash mismatch");
    }
  }
  if (res.root_proof.not_null()) {
    auto root = vm::MerkleProof::virtualize(res.root_proof, 1);
    if (root.is_null()) {
      return td::Status::Error("Root proof is not a merkle proof");
    }
    if (root->get_hash().as_slice() != res.info.root_hash.as_slice()) {
      return td::Status::Error("Root proof hash mismatch");
    }
  }
  res.info.init_cell();
  return res;
}

std::string TorrentMeta::serialize() const {
  return td::serialize(*this);
}

template <class StorerT>
void TorrentMeta::store(StorerT &storer) const {
  using td::store;
  td::uint32 flags = 0;

  if (root_proof.not_null()) {
    flags |= 1;
  }

  if (header) {
    flags |= 2;
  }
  store(flags, storer);

  const auto info_boc = vm::std_boc_serialize(info.as_cell()).move_as_ok();
  td::uint32 info_boc_size = td::narrow_cast<td::uint32>(info_boc.size());

  td::BufferSlice root_proof_boc;
  td::uint32 root_proof_boc_size{0};
  if ((flags & 1) != 0) {
    root_proof_boc = vm::std_boc_serialize(root_proof).move_as_ok();
    root_proof_boc_size = td::narrow_cast<td::uint32>(root_proof_boc.size());
  }

  store(info_boc_size, storer);
  if ((flags & 1) != 0) {
    store(root_proof_boc_size, storer);
  }
  storer.store_slice(info_boc.as_slice());
  if ((flags & 1) != 0) {
    storer.store_slice(root_proof_boc.as_slice());
  }

  if ((flags & 2) != 0) {
    store(header.value(), storer);
  }
}

template <class ParserT>
void TorrentMeta::parse(ParserT &parser) {
  using td::parse;
  td::uint32 flags;
  parse(flags, parser);

  td::uint32 info_boc_size;
  td::uint32 root_proof_boc_size;
  parse(info_boc_size, parser);
  if ((flags & 1) != 0) {
    parse(root_proof_boc_size, parser);
  }
  auto info_boc_str = parser.template fetch_string_raw<std::string>(info_boc_size);
  auto r_info_cell = vm::std_boc_deserialize(info_boc_str);
  if (r_info_cell.is_error()) {
    parser.set_error(r_info_cell.error().to_string());
    return;
  }

  if ((flags & 1) != 0) {
    auto root_proof_str = parser.template fetch_string_raw<std::string>(root_proof_boc_size);
    auto r_root_proof = vm::std_boc_deserialize(root_proof_str);
    if (r_root_proof.is_error()) {
      parser.set_error(r_root_proof.error().to_string());
      return;
    }
    root_proof = r_root_proof.move_as_ok();
  }

  auto cs = vm::load_cell_slice(r_info_cell.move_as_ok());
  if (!info.unpack(cs)) {
    parser.set_error("Failed to parse TorrentInfo");
    return;
  }

  if ((flags & 2) != 0) {
    TorrentHeader new_header;
    parse(new_header, parser);
    header = std::move(new_header);
  }
}
}  // namespace ton
