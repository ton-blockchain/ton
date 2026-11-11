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
*/

#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "ton/ton-io.hpp"
#include "vm/cells/MerkleProof.h"

#include "wait-block-data.hpp"

namespace ton {
namespace validator {

td::Result<td::Ref<vm::Cell>> WaitBlockData::generate_block_proof_root(BlockIdExt id, td::Ref<vm::Cell> block_root) {
  if (block_root.is_null()) {
    return td::Status::Error("block root is null");
  }

  RootHash block_root_hash{block_root->get_hash().bits()};
  if (block_root_hash != id.root_hash) {
    return td::Status::Error(PSTRING() << "block root hash mismatch: expected " << id.root_hash.to_hex() << ", found "
                                       << block_root_hash.to_hex());
  }

  auto usage_tree = std::make_shared<vm::CellUsageTree>();
  auto usage_cell = vm::UsageCell::create(block_root, usage_tree->root_ptr());
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  block::gen::ExtBlkRef::Record mcref{};  // _ ExtBlkRef = BlkMasterInfo;
  ShardIdFull shard;
  if (!(tlb::unpack_cell(usage_cell, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
        block::tlb::t_ShardIdent.unpack(info.shard.write(), shard) &&
        block::gen::BlkPrevInfo{info.after_merge}.validate_ref(info.prev_ref) &&
        tlb::unpack_cell(std::move(blk.extra), extra) && block::gen::t_ValueFlow.force_validate_ref(blk.value_flow) &&
        (!info.not_master || tlb::unpack_cell(info.master_ref, mcref)))) {
    return td::Status::Error("cannot unpack block header");
  }
  if (info.not_master != !shard.is_masterchain()) {
    return td::Status::Error("block has invalid not_master flag in its header");
  }
  BlockId block_id{shard, static_cast<unsigned>(info.seq_no)};
  if (block_id != id.id) {
    return td::Status::Error(PSTRING() << "block header corresponds to another block id: expected " << id.id
                                       << ", found " << block_id);
  }
  if (shard.is_masterchain() && (info.after_merge | info.after_split | info.before_split)) {
    return td::Status::Error(PSTRING() << "masterchain block header of " << id << " announces merge/split");
  }
  if (!shard.is_masterchain() && info.key_block) {
    return td::Status::Error(PSTRING() << "non-masterchain block header of " << id << " announces this block to be "
                                       << "a key block");
  }

  vm::CellSlice upd_cs{vm::NoVm(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4 && upd_cs.size_ext() == 0x20228)) {
    return td::Status::Error("invalid Merkle update in block");
  }

  if (info.key_block) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(extra.custom->have_refs() && tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) && mc_extra.key_block &&
          mc_extra.config.not_null())) {
      return td::Status::Error(PSTRING() << "cannot unpack extra header of key masterchain block " << block_id);
    }
    auto cfg = block::Config::unpack_config(std::move(mc_extra.config));
    if (cfg.is_error()) {
      return cfg.move_as_error_prefix(PSTRING() << "cannot extract configuration from extra header of key masterchain "
                                                << "block " << block_id << ": ");
    }
    auto res = cfg.move_as_ok()->visit_validator_params();
    if (res.is_error()) {
      return res.move_as_error_prefix(PSTRING() << "cannot extract validator set configuration from extra header of "
                                                << "key masterchain block " << block_id << ": ");
    }
  }

  TRY_RESULT(proof, vm::MerkleProof::generate(block_root, usage_tree.get()));

  if (shard.is_masterchain() && !info.key_block) {
    block::gen::McBlockExtra::Record mc_extra;
    if (!(extra.custom->have_refs() && tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) &&
          !mc_extra.key_block)) {
      return td::Status::Error(PSTRING() << "extra header of non-key masterchain block " << block_id
                                         << " is invalid or contains extra information reserved for key blocks only");
    }
  }

  return proof;
}

}  // namespace validator
}  // namespace ton
