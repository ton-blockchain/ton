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
#include "check-proof.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/mc-config.h"

#include "ton/ton-shard.h"

#include "vm/cells/MerkleProof.h"
#include "openssl/digest.hpp"
#include "Ed25519.h"

namespace block {
using namespace std::literals::string_literals;

td::Status check_block_header_proof(td::Ref<vm::Cell> root, ton::BlockIdExt blkid, ton::Bits256* store_state_hash_to,
                                    bool check_state_hash, td::uint32* save_utime, ton::LogicalTime* save_lt) {
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
  if (save_utime) {
    *save_utime = info.gen_utime;
  }
  if (save_lt) {
    *save_lt = info.end_lt;
  }
  if (store_state_hash_to) {
    vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
    if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
          && upd_cs.size_ext() == 0x20228)) {
      return td::Status::Error("invalid Merkle update in block header");
    }
    auto upd_hash = upd_cs.prefetch_ref(1)->get_hash(0);
    if (!check_state_hash) {
      *store_state_hash_to = upd_hash.bits();
    } else if (store_state_hash_to->compare(upd_hash.bits())) {
      return td::Status::Error(PSTRING() << "state hash mismatch in block header of " << blkid.to_str()
                                         << " : header declares " << upd_hash.bits().to_hex(256) << " expected "
                                         << store_state_hash_to->to_hex());
    }
  }
  return td::Status::OK();
}

td::Result<td::Bits256> check_state_proof(ton::BlockIdExt blkid, td::Slice proof) {
  TRY_RESULT(proof_root, vm::std_boc_deserialize(proof));
  auto virt_root = vm::MerkleProof::virtualize(std::move(proof_root));
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
    auto state_virt_root = vm::MerkleProof::virtualize(std::move(state_root));
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
    auto mc_state_root = vm::MerkleProof::virtualize(std::move(P_roots[1]));
    if (mc_state_root.is_null()) {
      return td::Status::Error("shard configuration proof is invalid");
    }
    ton::Bits256 mc_state_hash = mc_state_root->get_hash().bits();
    TRY_STATUS_PREFIX(
        check_block_header_proof(vm::MerkleProof::virtualize(std::move(P_roots[0])), blk, &mc_state_hash, true),
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
                               td::Ref<vm::Cell> root, ton::LogicalTime* last_trans_lt, ton::Bits256* last_trans_hash,
                               td::uint32* save_utime, ton::LogicalTime* save_lt) {
  TRY_RESULT_PREFIX(Q_roots, vm::std_boc_deserialize_multi(std::move(proof)), "cannot deserialize account proof");
  if (Q_roots.size() != 2) {
    return td::Status::Error(PSLICE() << "account state proof must have exactly two roots");
  }

  if (last_trans_lt) {
    last_trans_hash->set_zero();
  }

  try {
    auto state_root = vm::MerkleProof::virtualize(std::move(Q_roots[1]));
    if (state_root.is_null()) {
      return td::Status::Error("account state proof is invalid");
    }
    ton::Bits256 state_hash = state_root->get_hash().bits();
    TRY_STATUS_PREFIX(check_block_header_proof(vm::MerkleProof::virtualize(std::move(Q_roots[0])), shard_blk,
                                               &state_hash, true, save_utime, save_lt),
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
  TRY_RESULT_PREFIX(true_root, vm::std_boc_deserialize(state.as_slice(), true), "cannot deserialize account state");
  Ref<vm::Cell> root;

  if (is_virtualized && true_root.not_null()) {
    root = vm::MerkleProof::virtualize(true_root);
    if (root.is_null()) {
      return td::Status::Error("account state proof is invalid");
    }
  } else {
    root = true_root;
  }

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
  TRY_STATUS(block::check_account_proof(proof.as_slice(), shard_blk, addr, root, &res.last_trans_lt,
                                        &res.last_trans_hash, &res.gen_utime, &res.gen_lt));
  res.root = std::move(root);
  res.true_root = std::move(true_root);

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
  res.now = trans.now;
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
  res.lt = lt;
  res.hash = hash;
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

td::Result<BlockTransaction::Info> BlockTransaction::validate(bool check_proof) const {
  if (root.is_null()) {
    return td::Status::Error("transactions are expected to be non-empty");
  }
  if (check_proof && proof->get_hash().bits().compare(root->get_hash().bits(), 256)) {
    return td::Status::Error(PSLICE() << "transaction hash mismatch: Merkle proof expects "
                                      << proof->get_hash().bits().to_hex(256)
                                      << " but received data has " << root->get_hash().bits().to_hex(256));
  }
  block::gen::Transaction::Record trans;
  if (!tlb::unpack_cell(root, trans)) {
    return td::Status::Error("cannot unpack transaction cell");
  }
  Info res;
  res.blkid = blkid;
  res.now = trans.now;
  res.lt = trans.lt;
  res.hash = root->get_hash().bits();
  res.transaction = root;
  return std::move(res);
}

td::Result<BlockTransactionList::Info> BlockTransactionList::validate(bool check_proof) const {
  constexpr int max_answer_transactions = 256;

  TRY_RESULT_PREFIX(list, vm::std_boc_deserialize_multi(std::move(transactions_boc)), "cannot deserialize transactions boc: ");  
  std::vector<td::Ref<vm::Cell>> tx_proofs(list.size());

  if (check_proof) {
    try {
      TRY_RESULT(proof_cell, vm::std_boc_deserialize(std::move(proof_boc)));
      auto virt_root = vm::MerkleProof::virtualize(proof_cell);

      if (blkid.root_hash != virt_root->get_hash().bits()) {
        return td::Status::Error("Invalid block proof root hash");
      }
      block::gen::Block::Record blk;
      block::gen::BlockExtra::Record extra;
      if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(std::move(blk.extra), extra))) {
        return td::Status::Error("Error unpacking proof cell");
      }
      vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                  block::tlb::aug_ShardAccountBlocks};

      bool eof = false;
      ton::LogicalTime reverse = reverse_mode ? ~0ULL : 0;
      ton::LogicalTime trans_lt = static_cast<ton::LogicalTime>(start_lt);
      td::Bits256 cur_addr = start_addr;
      bool allow_same = true;
      int count = 0;
      while (!eof && count < req_count && count < max_answer_transactions) {
        auto value = acc_dict.extract_value(
              acc_dict.vm::DictionaryFixed::lookup_nearest_key(cur_addr.bits(), 256, !reverse, allow_same));
        if (value.is_null()) {
          eof = true;
          break;
        }
        allow_same = false;
        if (cur_addr != start_addr) {
          trans_lt = reverse;
        }

        block::gen::AccountBlock::Record acc_blk;
        if (!tlb::csr_unpack(std::move(value), acc_blk) || acc_blk.account_addr != cur_addr) {
          return td::Status::Error("Error unpacking proof account block");
        }
        vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                    block::tlb::aug_AccountTransactions};
        td::BitArray<64> cur_trans{(long long)trans_lt};
        while (count < req_count && count < max_answer_transactions) {
          auto tvalue = trans_dict.extract_value_ref(
                trans_dict.vm::DictionaryFixed::lookup_nearest_key(cur_trans.bits(), 64, !reverse));
          if (tvalue.is_null()) {
            trans_lt = reverse;
            break;
          }
          if (static_cast<size_t>(count) < tx_proofs.size()) {
            tx_proofs[count] = std::move(tvalue);
          }
          count++;
        }
      }
      if (static_cast<size_t>(count) != list.size()) {
        return td::Status::Error(PSLICE() << "Txs count mismatch in proof (" << count << ") and response (" << list.size() << ")");
      }
    } catch (vm::VmError& err) {
      return err.as_status("Couldn't verify proof: ");
    } catch (vm::VmVirtError& err) {
      return err.as_status("Couldn't verify proof: ");
    } catch (...) {
      return td::Status::Error("Unknown exception raised while verifying proof");
    }
  }

  Info res;
  for (int i = 0; i < static_cast<int>(list.size()); i++) {
    auto& root = list[i];
    BlockTransaction transaction;
    transaction.root = root;
    transaction.blkid = blkid;
    transaction.proof = tx_proofs[i];
    TRY_RESULT(info, transaction.validate(check_proof));
    res.transactions.push_back(std::move(info));
  }
  return std::move(res);
}

td::Status BlockProofLink::validate(td::uint32* save_utime) const {
  if (save_utime) {
    *save_utime = 0;
  }
  if (!(from.is_masterchain_ext() && to.is_masterchain_ext())) {
    return td::Status::Error("BlockProofLink must have both source and destination blocks in the masterchain");
  }
  if (from.seqno() == to.seqno()) {
    return td::Status::Error("BlockProofLink connects two masterchain blocks "s + from.to_str() + " and " +
                             to.to_str() + " of equal height");
  }
  if (is_fwd != (from.seqno() < to.seqno())) {
    return td::Status::Error("BlockProofLink from "s + from.to_str() + " to " + to.to_str() +
                             " is incorrectly declared as a " + (is_fwd ? "forward" : "backward") + " link");
  }
  if (dest_proof.is_null() && to.seqno()) {
    return td::Status::Error("BlockProofLink contains no proof for destination block "s + to.to_str());
  }
  if (proof.is_null()) {
    return td::Status::Error("BlockProofLink contains no proof for source block "s + from.to_str());
  }
  if (!is_fwd && state_proof.is_null()) {
    return td::Status::Error("a backward BlockProofLink contains no proof for the source state of "s + from.to_str());
  }
  if (is_fwd && signatures.empty()) {
    return td::Status::Error("a forward BlockProofLink from "s + from.to_str() + " to " + to.to_str() +
                             " contains no signatures");
  }
  try {
    // virtualize Merkle proof roots
    auto vs_root = vm::MerkleProof::virtualize(proof);
    if (vs_root.is_null()) {
      return td::Status::Error("BlockProofLink contains an invalid Merkle proof for source block "s + from.to_str());
    }
    ton::Bits256 state_hash;
    if (from.seqno()) {
      TRY_STATUS(check_block_header(vs_root, from, is_fwd ? nullptr : &state_hash));
    }
    auto vd_root = dest_proof.not_null() ? vm::MerkleProof::virtualize(dest_proof) : Ref<vm::Cell>{};
    if (vd_root.is_null() && to.seqno()) {
      return td::Status::Error("BlockProofLink contains an invalid Merkle proof for destination block "s + to.to_str());
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (to.seqno()) {
      TRY_STATUS(check_block_header(vd_root, to));
      if (!(tlb::unpack_cell(vd_root, blk) && tlb::unpack_cell(blk.info, info))) {
        return td::Status::Error("cannot unpack header for block "s + to.to_str());
      }
      if (info.key_block != is_key) {
        return td::Status::Error(PSTRING() << "incorrect is_key_block value " << is_key << " for destination block "
                                           << to.to_str());
      }
      if (save_utime) {
        *save_utime = info.gen_utime;
      }
    } else if (!is_key) {
      // return td::Status::Error("Zerostate destination block "s + to.to_str() + " does not have is_key_block set");
    }
    if (!is_fwd) {
      // check a backward link
      auto vstate_root = vm::MerkleProof::virtualize(state_proof);
      if (vstate_root.is_null()) {
        return td::Status::Error("backward BlockProofLink contains an invalid Merkle proof for source state "s +
                                 from.to_str());
      }
      if (state_hash != vstate_root->get_hash().bits()) {
        return td::Status::Error("BlockProofLink contains a state proof for "s + from.to_str() +
                                 " with incorrect root hash");
      }
      TRY_RESULT(config, block::ConfigInfo::extract_config(vstate_root, block::ConfigInfo::needPrevBlocks));
      if (!config->check_old_mc_block_id(to, true)) {
        return td::Status::Error("cannot check that "s + to.to_str() + " is indeed a previous masterchain block of " +
                                 from.to_str() + " using the presented Merkle proof of masterchain state");
      }
      return td::Status::OK();
    } else {
      // check a forward link
      // extract configuration from source key block or zerostate
      auto cfg_res = from.seqno() ? block::Config::extract_from_key_block(vs_root, block::ConfigInfo::needValidatorSet)
                                  : block::Config::extract_from_state(vs_root, block::ConfigInfo::needValidatorSet);
      if (cfg_res.is_error()) {
        return td::Status::Error("cannot extract configuration from source key block "s + from.to_str() +
                                 " of a forward BlockProofLink: " + cfg_res.move_as_error().to_string());
      }
      auto config = cfg_res.move_as_ok();
      // compute validator set
      ton::ShardIdFull shard{ton::masterchainId};
      auto nodes = config->compute_validator_set(shard, info.gen_utime, info.gen_catchain_seqno);
      if (nodes.empty()) {
        return td::Status::Error(PSTRING()
                                 << "while checking a forward BlockProofLink: cannot compute validator set for block "
                                 << to.to_str() << " with utime " << info.gen_utime << " and cc_seqno "
                                 << info.gen_catchain_seqno << " starting from previous key block " << from.to_str());
      }
      // check computed validator set hash
      auto vset_hash = compute_validator_set_hash(cc_seqno, shard, nodes);
      if (vset_hash != info.gen_validator_list_hash_short) {
        return td::Status::Error(
            PSTRING() << "while checking a forward BlockProofLink: computed validator set for block " << to.to_str()
                      << " with utime " << info.gen_utime << " and cc_seqno " << info.gen_catchain_seqno
                      << " starting from previous key block " << from.to_str() << " has hash " << vset_hash
                      << " different from " << info.gen_validator_list_hash_short << " stated in block header");
      }
      // check signatures
      auto err = check_block_signatures(nodes, signatures, to);
      if (err.is_error()) {
        return td::Status::Error("error checking signatures for block "s + to.to_str() +
                                 " in a forward BlockProofLink: " + err.to_string());
      }
      return td::Status::OK();
    }
  } catch (vm::VmError& err) {
    return td::Status::Error("vm error while checking BlockProofLink from "s + from.to_str() + " to " + to.to_str() +
                             " : " + err.get_msg());
  } catch (vm::VmVirtError& err) {
    return td::Status::Error("virtualization error while checking BlockProofLink from "s + from.to_str() + " to " +
                             to.to_str() + " : " + err.get_msg());
  }
}

td::Status BlockProofChain::validate(td::CancellationToken cancellation_token) {
  valid = false;
  has_key_block = false;
  has_utime = false;
  last_utime = 0;
  key_blkid.invalidate();
  if (!(from.is_masterchain_ext() && to.is_masterchain_ext())) {
    return td::Status::Error("BlockProofChain must have both source and destination blocks in the masterchain");
  }
  if (!link_count()) {
    if (from != to) {
      return td::Status::Error("BlockProofChain has no links, but its source block "s + from.to_str() +
                               " and destination block " + to.to_str() + " differ");
    }
    valid = true;
    return td::Status::OK();
  }
  ton::BlockIdExt cur = from;
  int i = 0;
  for (const auto& link : links) {
    ++i;
    if (link.from != cur) {
      return td::Status::Error(PSTRING() << "link #" << i << " in a BlockProofChain begins with block "
                                         << link.from.to_str() << " but the previous link ends at different block "
                                         << cur.to_str());
    }
    if (cancellation_token) {
      return td::Status::Error("Cancelled");
    }
    auto err = link.validate(&last_utime);
    if (err.is_error()) {
      return td::Status::Error(PSTRING() << "link #" << i << " in BlockProofChain is invalid: " << err.to_string());
    }
    if (link.is_key && (!has_key_block || key_blkid.seqno() < link.to.seqno())) {
      key_blkid = link.to;
      has_key_block = true;
    }
    cur = link.to;
  }
  if (cur != to) {
    return td::Status::Error("last link of BlockProofChain ends at block "s + cur.to_str() +
                             " different from declared chain destination block " + to.to_str());
  }
  has_utime = (last_utime > 0);
  valid = true;
  return td::Status::OK();
}

td::Bits256 compute_node_id_short(td::Bits256 ed25519_pubkey) {
  // pub.ed25519#4813b4c6 key:int256 = PublicKey;
  struct pubkey {
    int magic = 0x4813b4c6;
    unsigned char ed25519_key[32];
  } PK;
  std::memcpy(PK.ed25519_key, ed25519_pubkey.data(), 32);
  static_assert(sizeof(pubkey) == 36, "PublicKey structure is not 36 bytes long");
  td::Bits256 hash;
  digest::hash_str<digest::SHA256>(hash.data(), (void*)&PK, sizeof(pubkey));
  return hash;
}

td::Status check_block_signatures(const std::vector<ton::ValidatorDescr>& nodes,
                                  const std::vector<ton::BlockSignature>& signatures, const ton::BlockIdExt& blkid) {
  if (nodes.empty()) {
    return td::Status::Error("empty validator public keys set");
  }
  if (signatures.empty()) {
    return td::Status::Error("empty validator signature set");
  }
  // compute the string to be signed and its hash
  unsigned char to_sign[68];
  td::as<td::uint32>(to_sign) = 0xc50b6e70;  // ton.blockId root_cell_hash:int256 file_hash:int256 = ton.BlockId;
  memcpy(to_sign + 4, blkid.root_hash.data(), 32);
  memcpy(to_sign + 36, blkid.file_hash.data(), 32);
  // unsigned char hash[32];
  // digest::hash_str<digest::SHA256>(hash, (void*)to_sign, sizeof(to_sign));

  ton::ValidatorWeight total_weight = 0, signed_weight = 0;
  std::vector<std::pair<td::Bits256, unsigned>> node_map;
  for (unsigned i = 0; i < nodes.size(); i++) {
    total_weight += nodes[i].weight;
    node_map.emplace_back(compute_node_id_short(nodes[i].key), i);
  }
  std::sort(node_map.begin(), node_map.end());
  std::vector<unsigned> seen;
  for (auto& sig : signatures) {
    // lookup node in validator set
    auto& id = sig.node;
    auto it = std::lower_bound(node_map.begin(), node_map.end(), id,
                               [](const auto& p, const auto& x) { return p.first < x; });
    if (it == node_map.end() || it->first != id) {
      return td::Status::Error("signature set contains unknown NodeIdShort "s + id.to_hex());
    }
    unsigned i = it->second;
    seen.emplace_back(i);
    // check one signature
    td::Ed25519::PublicKey pub_key{td::SecureString{nodes.at(i).key.as_slice()}};
    auto res = pub_key.verify_signature(td::Slice{to_sign, 68}, sig.signature.as_slice());
    if (res.is_error()) {
      return res;
    }
    signed_weight += nodes[i].weight;
    if (signed_weight > total_weight) {
      break;
    }
  }
  std::sort(seen.begin(), seen.end());
  for (std::size_t i = 1; i < seen.size(); i++) {
    if (seen[i] == seen[i - 1]) {
      return td::Status::Error("signature set contains duplicate signature for NodeIdShort "s +
                               compute_node_id_short(nodes.at(seen[i]).key).to_hex());
    }
  }
  if (3 * signed_weight <= 2 * total_weight) {
    return td::Status::Error(PSTRING() << "insufficient total signature weight: only " << signed_weight << " out of "
                                       << total_weight);
  }
  return td::Status::OK();
}

}  // namespace block
