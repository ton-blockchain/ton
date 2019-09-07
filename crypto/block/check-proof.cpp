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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "check-proof.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/mc-config.h"

#include "ton/ton-shard.h"

#include "vm/cells/MerkleProof.h"

namespace block {
td::Status check_block_header_proof(td::Ref<vm::Cell> root, ton::BlockIdExt blkid, ton::Bits256* store_shard_hash_to,
                                    bool check_state_hash) {
  ton::RootHash vhash{root->get_hash().bits()};
  if (vhash != blkid.root_hash) {
    return td::Status::Error(PSTRING() << " block header for block " << blkid.to_str() << " has incorrect root hash "
                                       << vhash.to_hex() << " instead of " << blkid.root_hash.to_hex());
  }
  std::vector<ton::BlockIdExt> prev;
  ton::BlockIdExt mc_blkid;
  bool after_split;
  TRY_STATUS(block::unpack_block_prev_blk_try(root, blkid, prev, mc_blkid, after_split));
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(root, blk) && tlb::unpack_cell(blk.info, info))) {
    return td::Status::Error(std::string{"cannot unpack header for block "} + blkid.to_str());
  }
  if (store_shard_hash_to) {
    vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
    if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
          && upd_cs.size_ext() == 0x20228)) {
      return td::Status::Error("invalid Merkle update in block header");
    }
    auto upd_hash = upd_cs.prefetch_ref(1)->get_hash(0);
    if (!check_state_hash) {
      *store_shard_hash_to = upd_hash.bits();
    } else if (store_shard_hash_to->compare(upd_hash.bits())) {
      return td::Status::Error(PSTRING() << "state hash mismatch in block header of " << blkid.to_str()
                                         << " : header declares " << upd_hash.bits().to_hex(256) << " expected "
                                         << store_shard_hash_to->to_hex());
    }
  }
  return td::Status::OK();
}

td::Result<td::Bits256> check_state_proof(ton::BlockIdExt blkid, td::Slice proof) {
  TRY_RESULT(proof_root, vm::std_boc_deserialize(proof));
  auto virt_root = vm::MerkleProof::virtualize(std::move(proof_root), 1);
  if (virt_root.is_null()) {
    return td::Status::Error("account state proof is invalid");
  }
  td::Bits256 state_hash;
  TRY_STATUS(check_block_header_proof(std::move(virt_root), blkid, &state_hash));
  return state_hash;
}

td::Result<Ref<vm::Cell>> check_extract_state_proof(ton::BlockIdExt blkid, td::Slice proof, td::Slice data) {
  try {
    TRY_RESULT(state_hash, check_state_proof(blkid, proof));
    TRY_RESULT(state_root, vm::std_boc_deserialize(data));
    auto state_virt_root = vm::MerkleProof::virtualize(std::move(state_root), 1);
    if (state_virt_root.is_null()) {
      return td::Status::Error("account state proof is invalid");
    }
    if (state_hash != state_virt_root->get_hash().bits()) {
      return td::Status::Error("root hash mismatch in the shardchain state proof");
    }
    return std::move(state_virt_root);
  } catch (vm::VmError& err) {
    return td::Status::Error(PSLICE() << "error scanning shard state proof: " << err.get_msg());
  } catch (vm::VmVirtError& err) {
    return td::Status::Error(PSLICE() << "virtualization error scanning shard state proof: " << err.get_msg());
  }
}

td::Status check_shard_proof(ton::BlockIdExt blk, ton::BlockIdExt shard_blk, td::Slice shard_proof) {
  if (blk == shard_blk) {
    if (!shard_proof.empty()) {
      LOG(WARNING) << "Unexpected non-empty shard proof";
    }
    return td::Status::OK();
  }
  if (!blk.is_masterchain() || !blk.is_valid_full()) {
    return td::Status::Error(PSLICE() << "reference block " << blk.to_str()
                                      << " for a getAccountState query must belong to the masterchain");
  }
  TRY_RESULT_PREFIX(P_roots, vm::std_boc_deserialize_multi(std::move(shard_proof)),
                    "cannot deserialize shard configuration proof");
  if (P_roots.size() != 2) {
    return td::Status::Error("shard configuration proof must have exactly two roots");
  }
  try {
    auto mc_state_root = vm::MerkleProof::virtualize(std::move(P_roots[1]), 1);
    if (mc_state_root.is_null()) {
      return td::Status::Error("shard configuration proof is invalid");
    }
    ton::Bits256 mc_state_hash = mc_state_root->get_hash().bits();
    TRY_STATUS_PREFIX(
        check_block_header_proof(vm::MerkleProof::virtualize(std::move(P_roots[0]), 1), blk, &mc_state_hash, true),
        "error in shard configuration block header proof :");
    block::gen::ShardStateUnsplit::Record sstate;
    if (!(tlb::unpack_cell(mc_state_root, sstate))) {
      return td::Status::Error("cannot unpack masterchain state header");
    }
    auto shards_dict = block::ShardConfig::extract_shard_hashes_dict(std::move(mc_state_root));
    if (!shards_dict) {
      return td::Status::Error("cannot extract shard configuration dictionary from proof");
    }
    vm::CellSlice cs;
    ton::ShardIdFull true_shard;
    if (!block::ShardConfig::get_shard_hash_raw_from(*shards_dict, cs, shard_blk.shard_full(), true_shard)) {
      return td::Status::Error(PSLICE() << "masterchain state contains no information for shard "
                                        << shard_blk.shard_full().to_str());
    }
    auto shard_info = block::McShardHash::unpack(cs, true_shard);
    if (shard_info.is_null()) {
      return td::Status::Error(PSLICE() << "cannot unpack information for shard " << shard_blk.shard_full().to_str()
                                        << " from masterchain state");
    }
    if (shard_info->top_block_id() != shard_blk) {
      return td::Status::Error(PSLICE() << "shard configuration mismatch: expected to find block " << shard_blk.to_str()
                                        << " , found " << shard_info->top_block_id().to_str());
    }
  } catch (vm::VmError err) {
    return td::Status::Error(PSLICE() << "error while traversing shard configuration proof : " << err.get_msg());
  } catch (vm::VmVirtError err) {
    return td::Status::Error(PSLICE() << "virtualization error while traversing shard configuration proof : "
                                      << err.get_msg());
  }
  return td::Status::OK();
}

td::Status check_account_proof(td::Slice proof, ton::BlockIdExt shard_blk, const block::StdAddress& addr,
                               td::Ref<vm::Cell> root, ton::LogicalTime* last_trans_lt, ton::Bits256* last_trans_hash) {
  TRY_RESULT_PREFIX(Q_roots, vm::std_boc_deserialize_multi(std::move(proof)), "cannot deserialize account proof");
  if (Q_roots.size() != 2) {
    return td::Status::Error(PSLICE() << "account state proof must have exactly two roots");
  }

  if (last_trans_lt) {
    last_trans_hash->set_zero();
  }

  try {
    auto state_root = vm::MerkleProof::virtualize(std::move(Q_roots[1]), 1);
    if (state_root.is_null()) {
      return td::Status::Error("account state proof is invalid");
    }
    ton::Bits256 state_hash = state_root->get_hash().bits();
    TRY_STATUS_PREFIX(
        check_block_header_proof(vm::MerkleProof::virtualize(std::move(Q_roots[0]), 1), shard_blk, &state_hash, true),
        "error in account shard block header proof : ");
    block::gen::ShardStateUnsplit::Record sstate;
    if (!(tlb::unpack_cell(std::move(state_root), sstate))) {
      return td::Status::Error("cannot unpack state header");
    }
    vm::AugmentedDictionary accounts_dict{vm::load_cell_slice(sstate.accounts).prefetch_ref(), 256,
                                          block::tlb::aug_ShardAccounts};
    auto acc_csr = accounts_dict.lookup(addr.addr);
    if (acc_csr.not_null()) {
      if (root.is_null()) {
        return td::Status::Error(PSLICE() << "account state proof shows that account state for " << addr
                                          << " must be non-empty, but it actually is empty");
      }
      block::gen::ShardAccount::Record acc_info;
      if (!tlb::csr_unpack(std::move(acc_csr), acc_info)) {
        return td::Status::Error("cannot unpack ShardAccount from proof");
      }
      if (acc_info.account->get_hash().bits().compare(root->get_hash().bits(), 256)) {
        return td::Status::Error(PSLICE() << "account state hash mismatch: Merkle proof expects "
                                          << acc_info.account->get_hash().bits().to_hex(256)
                                          << " but received data has " << root->get_hash().bits().to_hex(256));
      }
      if (last_trans_hash) {
        *last_trans_hash = acc_info.last_trans_hash;
      }
      if (last_trans_lt) {
        *last_trans_lt = acc_info.last_trans_lt;
      }
    } else if (root.not_null()) {
      return td::Status::Error(PSLICE() << "account state proof shows that account state for " << addr
                                        << " must be empty, but it is not");
    }
  } catch (vm::VmError err) {
    return td::Status::Error(PSLICE() << "error while traversing account proof : " << err.get_msg());
  } catch (vm::VmVirtError err) {
    return td::Status::Error(PSLICE() << "virtualization error while traversing account proof : " << err.get_msg());
  }
  return td::Status::OK();
}

td::Result<AccountState::Info> AccountState::validate(ton::BlockIdExt ref_blk, block::StdAddress addr) const {
  TRY_RESULT_PREFIX(root, vm::std_boc_deserialize(state.as_slice(), true), "cannot deserialize account state");

  LOG(INFO) << "got account state for " << addr << " with respect to blocks " << blk.to_str()
            << (shard_blk == blk ? "" : std::string{" and "} + shard_blk.to_str());
  if (blk != ref_blk && ref_blk.id.seqno != ~0U) {
    return td::Status::Error(PSLICE() << "obtained getAccountState() for a different reference block " << blk.to_str()
                                      << " instead of requested " << ref_blk.to_str());
  }

  if (!shard_blk.is_valid_full()) {
    return td::Status::Error(PSLICE() << "shard block id " << shard_blk.to_str() << " in answer is invalid");
  }

  if (!ton::shard_contains(shard_blk.shard_full(), ton::extract_addr_prefix(addr.workchain, addr.addr))) {
    return td::Status::Error(PSLICE() << "received data from shard block " << shard_blk.to_str()
                                      << " that cannot contain requested account");
  }

  TRY_STATUS(block::check_shard_proof(blk, shard_blk, shard_proof.as_slice()));

  Info res;
  TRY_STATUS(
      block::check_account_proof(proof.as_slice(), shard_blk, addr, root, &res.last_trans_lt, &res.last_trans_hash));
  res.root = std::move(root);

  return res;
}

td::Result<Transaction::Info> Transaction::validate() {
  if (root.is_null()) {
    return td::Status::Error("transactions are expected to be non-empty");
  }
  if (hash != root->get_hash().bits()) {
    return td::Status::Error(PSLICE() << "transaction hash mismatch: expected " << hash.to_hex() << ", found "
                                      << root->get_hash().bits().to_hex(256));
  }
  block::gen::Transaction::Record trans;
  if (!tlb::unpack_cell(root, trans)) {
    return td::Status::Error("cannot unpack transaction #");
  }

  if (trans.lt != lt) {
    return td::Status::Error(PSLICE() << "transaction lt mismatch: expected " << lt << ", found " << trans.lt);
  }
  Info res;
  res.blkid = blkid;
  res.prev_trans_lt = trans.prev_trans_lt;
  res.prev_trans_hash = trans.prev_trans_hash;
  res.transaction = root;
  return std::move(res);
}

td::Result<TransactionList::Info> TransactionList::validate() const {
  if (blkids.empty()) {
    return td::Status::Error("transaction list must be non-empty");
  }
  auto R = vm::std_boc_deserialize_multi(std::move(transactions_boc));
  if (R.is_error()) {
    return td::Status::Error("cannot deserialize transactions BoC");
  }
  auto list = R.move_as_ok();
  if (list.size() != blkids.size()) {
    return td::Status::Error(PSLICE() << "transaction list size " << list.size()
                                      << " must be equal to the size of block id list " << blkids.size());
  }
  size_t c = 0;
  Info res;
  auto current_lt = lt;
  auto current_hash = hash;
  for (auto& root : list) {
    const auto& blkid = blkids[c++];
    Transaction transaction;
    transaction.blkid = std::move(blkid);
    transaction.lt = current_lt;
    transaction.hash = current_hash;
    transaction.root = root;
    TRY_RESULT(info, transaction.validate());
    current_lt = info.prev_trans_lt;
    current_hash = info.prev_trans_hash;
    res.transactions.push_back(std::move(info));
  }
  return std::move(res);
}
}  // namespace block
