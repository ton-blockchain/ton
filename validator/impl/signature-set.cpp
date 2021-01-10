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
#include "signature-set.hpp"
#include "auto/tl/ton_api.hpp"
#include "adnl/utils.hpp"
#include "vm/dict.h"
#include "vm/boc.h"

namespace ton {

namespace validator {
using td::Ref;

BlockSignatureSetQ *BlockSignatureSetQ::make_copy() const {
  std::vector<BlockSignature> vec;
  auto &sigs = signatures();
  for (auto &s : sigs) {
    vec.emplace_back(BlockSignature{s.node, s.signature.clone()});
  }
  return new BlockSignatureSetQ{std::move(vec)};
}

td::BufferSlice BlockSignatureSetQ::serialize() const {
  if (!signatures().size()) {
    return {};
  }
  Ref<vm::Cell> root;
  CHECK(serialize_to(root));
  //std::cerr << "serializing BlockSignatureSet: ";
  //vm::CellSlice{vm::NoVm{}, root}.print_rec(std::cerr);
  //std::cerr << std::endl;
  auto res = vm::std_boc_serialize(std::move(root));
  LOG_CHECK(res.is_ok()) << res.move_as_error();
  return res.move_as_ok();
}

bool BlockSignatureSetQ::serialize_to(Ref<vm::Cell> &ref) const {
  auto &sigs = signatures();
  vm::Dictionary dict{16};  // HashmapE 16 CryptoSignaturePair
  for (unsigned i = 0; i < sigs.size(); i++) {
    vm::CellBuilder cb;
    if (!(cb.store_bits_bool(sigs[i].node)                      // sig_pair$_ node_id_short:bits256
          && cb.store_long_bool(5, 4)                           //   ed25519_signature#5
          && sigs[i].signature.size() == 64                     // signature must be 64 bytes long
          && cb.store_bytes_bool(sigs[i].signature.data(), 64)  // R:bits256 s:bits256
          && dict.set_builder(td::BitArray<16>{i}, cb, vm::Dictionary::SetMode::Add))) {
      return false;
    }
  }
  ref = std::move(dict).extract_root_cell();
  CHECK(sigs.size());
  CHECK(ref.not_null());
  return true;
}

Ref<BlockSignatureSet> BlockSignatureSetQ::fetch(td::BufferSlice data) {
  if (!data.size()) {
    return Ref<BlockSignatureSetQ>{true, std::vector<BlockSignature>{}};
  }
  auto res = vm::std_boc_deserialize(std::move(data));
  if (res.is_error()) {
    return {};
  }
  return fetch(res.move_as_ok());
}

Ref<BlockSignatureSet> BlockSignatureSetQ::fetch(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return {};
  }
  try {
    std::vector<BlockSignature> vec;
    vm::Dictionary dict{std::move(cell), 16};  // HashmapE 16 CryptoSignaturePair
    unsigned i = 0;
    if (!dict.check_for_each([&](Ref<vm::CellSlice> cs_ref, td::ConstBitPtr key, int n) -> bool {
          if (key.get_int(n) != i || cs_ref->size_ext() != 256 + 4 + 256 + 256) {
            return false;
          }
          vm::CellSlice cs{*cs_ref};
          NodeIdShort node_id;
          unsigned char signature[64];
          if (!(cs.fetch_bits_to(node_id)         // sig_pair$_ node_id_short:bits256
                && cs.fetch_ulong(4) == 5         // ed25519_signature#5
                && cs.fetch_bytes(signature, 64)  // R:bits256 s:bits256
                && !cs.size_ext())) {
            return false;
          }
          vec.emplace_back(BlockSignature{node_id, td::BufferSlice{td::Slice{signature, 64}}});
          ++i;
          return i <= max_signatures;
        })) {
      return {};
    }
    return Ref<BlockSignatureSetQ>{true, std::move(vec)};
  } catch (vm::VmError) {
    return {};
  }
}

}  // namespace validator

}  // namespace ton
