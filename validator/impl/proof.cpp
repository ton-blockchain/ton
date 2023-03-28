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
#include "proof.hpp"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"
#include "validator-set.hpp"

namespace ton {
namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

td::Result<Ref<ProofLink>> ProofQ::export_as_proof_link() const {
  TRY_RESULT(root, vm::std_boc_deserialize(data_));
  block::gen::BlockProof::Record proof;
  if (!(tlb::unpack_cell(std::move(root), proof))) {
    return td::Status::Error("cannot unpack BlockProof");
  }
  proof.signatures = vm::load_cell_slice_ref(vm::CellBuilder().store_long(0, 1).finalize());
  if (!(tlb::pack_cell(root, proof))) {
    return td::Status::Error("cannot pack new BlockProof");
  }
  TRY_RESULT(data, vm::std_boc_serialize(std::move(root)));
  return Ref<ProofLink>(td::make_ref<ProofLinkQ>(id_, std::move(data)));
}

td::Result<BlockSeqno> ProofLinkQ::prev_key_mc_seqno() const {
  //if (!id_.is_masterchain()) {
  //  return td::Status::Error(
  //      -668, "cannot compute previous key masterchain block from ProofLink of non-masterchain block "s + id_.to_str());
  //}
  TRY_RESULT(virt, get_virtual_root(true));
  try {
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (!(tlb::unpack_cell(std::move(virt.root), blk) && tlb::unpack_cell(blk.info, info) && !info.version)) {
      return td::Status::Error(-668,
                               "cannot unpack block header in the Merkle proof for masterchain block "s + id_.to_str());
    }
    return info.prev_key_block_seqno;
  } catch (vm::VmVirtError &) {
    return td::Status::Error(-668, "virtualization error in masterchain block proof for "s + id_.to_str());
  }
}

td::Result<td::Ref<ConfigHolder>> ProofLinkQ::get_key_block_config() const {
  if (!id_.is_masterchain()) {
    return td::Status::Error(
        -668, "cannot compute previous key masterchain block from ProofLink of non-masterchain block "s + id_.to_str());
  }
  TRY_RESULT(virt, get_virtual_root(true));
  try {
    TRY_RESULT(cfg, block::Config::extract_from_key_block(std::move(virt.root), block::Config::needValidatorSet));
    return td::make_ref<ConfigHolderQ>(std::move(cfg), std::move(virt.boc));
  } catch (vm::VmVirtError &) {
    return td::Status::Error(-668,
                             "virtualization error while traversing masterchain block proof for "s + id_.to_str());
  }
}

td::Result<ProofLink::BasicHeaderInfo> ProofLinkQ::get_basic_header_info() const {
  BasicHeaderInfo res;
  TRY_RESULT(virt, get_virtual_root(true));
  try {
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (!(tlb::unpack_cell(std::move(virt.root), blk) && tlb::unpack_cell(blk.info, info) && !info.version)) {
      return td::Status::Error(-668,
                               "cannot unpack block header in the Merkle proof for masterchain block "s + id_.to_str());
    }
    res.cc_seqno = info.gen_catchain_seqno;
    res.utime = info.gen_utime;
    res.end_lt = info.end_lt;
    res.validator_set_hash = info.gen_validator_list_hash_short;
    res.prev_key_mc_seqno = info.prev_key_block_seqno;
    return res;
  } catch (vm::VmVirtError &) {
    return td::Status::Error(-668, "virtualization error in masterchain block proof for "s + id_.to_str());
  }
}

td::Result<ProofLinkQ::VirtualizedProof> ProofLinkQ::get_virtual_root(bool lazy) const {
  if (data_.empty()) {
    return td::Status::Error(-668, "block proof is empty");
  }
  std::shared_ptr<vm::StaticBagOfCellsDb> boc;
  Ref<vm::Cell> root;
  if (lazy) {
    vm::StaticBagOfCellsDbLazy::Options options;
    options.check_crc32c = true;
    auto res = vm::StaticBagOfCellsDbLazy::create(td::BufferSliceBlobView::create(data_.clone()), options);
    if (res.is_error()) {
      return res.move_as_error();
    }
    boc = res.move_as_ok();
    TRY_RESULT(rc, boc->get_root_count());
    if (rc != 1) {
      return td::Status::Error(-668, "masterchain block proof BoC is invalid");
    }
    TRY_RESULT(t_root, boc->get_root_cell(0));
    root = std::move(t_root);
  } else {
    TRY_RESULT(t_root, vm::std_boc_deserialize(data_.as_slice()));
    root = std::move(t_root);
  }
  if (root.is_null()) {
    return td::Status::Error(-668, "cannot extract root cell out of a masterchain block proof BoC");
  }
  block::gen::BlockProof::Record proof;
  BlockIdExt proof_blk_id;
  if (!(tlb::unpack_cell(root, proof) && block::tlb::t_BlockIdExt.unpack(proof.proof_for.write(), proof_blk_id))) {
    return td::Status::Error(-668, "masterchain block proof is invalid");
  }
  if (proof_blk_id != id_) {
    return td::Status::Error(-668, "masterchain block proof is for another block");
  }
  auto virt_root = vm::MerkleProof::virtualize(proof.root, 1);
  if (virt_root.is_null()) {
    return td::Status::Error(-668, "block proof for block "s + proof_blk_id.to_str() +
                                       " does not contain a valid Merkle proof for the block header");
  }
  RootHash virt_hash{virt_root->get_hash().bits()};
  if (virt_hash != proof_blk_id.root_hash) {
    return td::Status::Error(-668, "block proof for block "s + proof_blk_id.to_str() +
                                       " contains a Merkle proof with incorrect root hash: expected " +
                                       proof_blk_id.root_hash.to_hex() + ", found " + virt_hash.to_hex());
  }
  return VirtualizedProof{std::move(virt_root), proof.signatures->prefetch_ref(), std::move(boc)};
}

td::Result<Ref<vm::Cell>> ProofQ::get_signatures_root() const {
  if (data_.empty()) {
    return td::Status::Error(-668, "block proof is empty");
  }
  TRY_RESULT(root, vm::std_boc_deserialize(data_.as_slice()));
  block::gen::BlockProof::Record proof;
  BlockIdExt proof_blk_id;
  if (!(tlb::unpack_cell(root, proof) && block::tlb::t_BlockIdExt.unpack(proof.proof_for.write(), proof_blk_id))) {
    return td::Status::Error(-668, "masterchain block proof is invalid");
  }
  if (proof_blk_id != id_) {
    return td::Status::Error(-668, "masterchain block proof is for another block");
  }
  return proof.signatures->prefetch_ref();
}

td::Result<td::Ref<vm::Cell>> create_block_state_proof(td::Ref<vm::Cell> root) {
  if (root.is_null()) {
    return td::Status::Error("root is null");
  }
  vm::MerkleProofBuilder mpb{std::move(root)};
  block::gen::Block::Record block;
  if (!tlb::unpack_cell(mpb.root(), block) || block.state_update->load_cell().is_error()) {
    return td::Status::Error("invalid block");
  }
  TRY_RESULT(proof, mpb.extract_proof());
  if (proof.is_null()) {
    return td::Status::Error("failed to create proof");
  }
  return proof;
}

td::Result<RootHash> unpack_block_state_proof(BlockIdExt block_id, td::Ref<vm::Cell> proof) {
  auto virt_root = vm::MerkleProof::virtualize(proof, 1);
  if (virt_root.is_null()) {
    return td::Status::Error("invalid Merkle proof");
  }
  if (virt_root->get_hash().as_slice() != block_id.root_hash.as_slice()) {
    return td::Status::Error("hash mismatch");
  }
  block::gen::Block::Record block;
  if (!tlb::unpack_cell(virt_root, block)) {
    return td::Status::Error("invalid block");
  }
  vm::CellSlice upd_cs{vm::NoVmSpec(), block.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4 && upd_cs.size_ext() == 0x20228)) {
    return td::Status::Error("invalid Merkle update");
  }
  return upd_cs.prefetch_ref(1)->get_hash(0).bits();
}

}  // namespace validator
}  // namespace ton
