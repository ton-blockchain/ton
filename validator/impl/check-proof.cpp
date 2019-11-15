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
#include "check-proof.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"
#include "fabric.h"
#include "signature-set.hpp"
#include "validator-set.hpp"
#include "shard.hpp"

#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"

#include "validator/invariants.hpp"

namespace ton {

namespace validator {
using namespace std::literals::string_literals;

void CheckProof::alarm() {
  abort_query(td::Status::Error(ErrorCode::notready, "timeout"));
}

void CheckProof::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(VALIDATOR_WARNING) << "aborting check proof for " << id_ << " query: " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

bool CheckProof::fatal_error(td::Status error) {
  abort_query(std::move(error));
  return false;
}

bool CheckProof::fatal_error(std::string err_msg, int err_code) {
  abort_query(td::Status::Error(err_code, err_msg));
  return false;
}

void CheckProof::finish_query() {
  if (skip_check_signatures_) {
    // TODO: check other invariants
  } else if (is_proof()) {
    ValidatorInvariants::check_post_check_proof(handle_);
  } else {
    ValidatorInvariants::check_post_check_proof_link(handle_);
  }
  if (promise_) {
    VLOG(VALIDATOR_DEBUG) << "checked proof for " << handle_->id();
    promise_.set_result(handle_);
  }
  stop();
}

bool CheckProof::check_send_error(td::actor::ActorId<CheckProof> SelfId, td::Status error) {
  if (error.is_error()) {
    td::actor::send_closure(std::move(SelfId), &CheckProof::abort_query, std::move(error));
    return true;
  } else {
    return false;
  }
}

bool CheckProof::init_parse(bool is_aux) {
  block::gen::BlockProof::Record proof;
  BlockIdExt proof_blk_id;
  if (!(tlb::unpack_cell(is_aux ? old_proof_root_ : proof_root_, proof) &&
        block::tlb::t_BlockIdExt.unpack(proof.proof_for.write(), proof_blk_id))) {
    return false;
  }
  BlockIdExt decl_id = (is_aux ? old_proof_ : proof_)->block_id();
  if (proof_blk_id != decl_id) {
    return fatal_error("block proof is for another block: declared "s + decl_id.to_str() + ", found " +
                       proof_blk_id.to_str());
  }
  if (!is_aux) {
    if (proof_blk_id != id_) {
      return fatal_error("block proof is for another block: expected "s + id_.to_str() + ", found " +
                         proof_blk_id.to_str());
    }
    if (!is_masterchain() && is_proof()) {
      return fatal_error("have a proof for non-masterchain block "s + id_.to_str());
    }
  } else {
    key_id_ = proof_blk_id;
    if (!is_masterchain()) {
      return fatal_error("cannot verify non-masterchain block "s + id_.to_str() +
                         " using previous key masterchain block");
    }
    if (!key_id_.is_masterchain()) {
      return fatal_error("auxiliary key block "s + key_id_.to_str() + " does not belong to the masterchain");
    }
    if (key_id_.seqno() != prev_key_seqno_) {
      return fatal_error(
          PSTRING() << "cannot verify newer block " << id_.to_str() << " using key block " << key_id_.to_str()
                    << " because the newer block declares different previous key block seqno " << prev_key_seqno_);
    }
    if (key_id_.seqno() >= id_.seqno()) {
      return fatal_error("cannot verify block "s + id_.to_str() + " using key block " + key_id_.to_str() +
                         " with larger or equal seqno");
    }
  }
  auto keep_cc_seqno = catchain_seqno_;
  auto keep_utime = created_at_;
  Ref<vm::Cell> sig_root = proof.signatures->prefetch_ref();
  if (sig_root.not_null()) {
    vm::CellSlice cs{vm::NoVmOrd(), sig_root};
    bool have_sig;
    if (!(cs.fetch_ulong(8) == 0x11                 // block_signatures#11
          && cs.fetch_uint_to(32, validator_hash_)  // validator_set_hash:uint32
          && cs.fetch_uint_to(32, catchain_seqno_)  // catchain_seqno:uint32
          && cs.fetch_uint_to(32, sig_count_)       // sig_count:uint32
          && cs.fetch_uint_to(64, sig_weight_)      // sig_weight:uint64
          && cs.fetch_bool_to(have_sig) && have_sig == (sig_count_ > 0) &&
          cs.size_ext() == ((unsigned)have_sig << 16))) {
      return fatal_error("cannot parse BlockSignatures");
    }
    sig_root_ = cs.prefetch_ref();
    if (!proof_blk_id.is_masterchain()) {
      return fatal_error("invalid ProofLink for non-masterchain block "s + proof_blk_id.to_str() +
                         " with validator signatures present");
    }
  } else {
    validator_hash_ = 0;
    catchain_seqno_ = 0;
    sig_count_ = 0;
    sig_weight_ = 0;
    sig_root_.clear();
  }
  auto virt_root = vm::MerkleProof::virtualize(proof.root, 1);
  if (virt_root.is_null()) {
    return fatal_error("block proof for block "s + proof_blk_id.to_str() +
                       " does not contain a valid Merkle proof for the block header");
  }
  RootHash virt_hash{virt_root->get_hash().bits()};
  if (virt_hash != proof_blk_id.root_hash) {
    return fatal_error("block proof for block "s + proof_blk_id.to_str() +
                       " contains a Merkle proof with incorrect root hash: expected " +
                       proof_blk_id.root_hash.to_hex() + ", found " + virt_hash.to_hex());
  }
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::ExtBlkRef::Record mcref;  // _ ExtBlkRef = BlkMasterInfo;
  ShardIdFull shard;
  if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
        block::tlb::t_ShardIdent.unpack(info.shard.write(), shard) &&
        block::gen::BlkPrevInfo{info.after_merge}.validate_ref(info.prev_ref) &&
        block::gen::t_ValueFlow.force_validate_ref(blk.value_flow) &&
        (!info.not_master || tlb::unpack_cell(info.master_ref, mcref)))) {
    return fatal_error("cannot unpack block header in the Merkle proof");
  }
  BlockId blk_id{shard, (unsigned)info.seq_no};
  if (blk_id != proof_blk_id.id) {
    return fatal_error("block header in the Merkle proof corresponds to another block id: expected "s +
                       proof_blk_id.id.to_str() + ", found " + blk_id.to_str());
  }
  if (info.not_master != !shard.is_masterchain()) {
    return fatal_error("block has invalid not_master flag in its (Merkelized) header");
  }
  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return fatal_error("invalid Merkle update in block");
  }
  state_old_hash_ = upd_cs.prefetch_ref(0)->get_hash(0).bits();
  state_hash_ = upd_cs.prefetch_ref(1)->get_hash(0).bits();
  lt_ = info.end_lt;
  created_at_ = info.gen_utime;
  after_merge_ = info.after_merge;
  before_split_ = info.before_split;
  // after_split_ = info.after_split;
  want_merge_ = info.want_merge;
  want_split_ = info.want_split;
  is_key_block_ = info.key_block;
  prev_key_seqno_ = info.prev_key_block_seqno;
  {
    auto res = block::unpack_block_prev_blk_ext(virt_root, proof_blk_id, prev_, mc_blkid_, after_split_);
    if (res.is_error()) {
      return fatal_error(res.message().str());
    }
  }
  CHECK(after_split_ == info.after_split);
  if (shard.is_masterchain() && (after_merge_ | before_split_ | after_split_)) {
    return fatal_error("block header declares split/merge for a masterchain block");
  }
  if (after_merge_ && after_split_) {
    return fatal_error("a block cannot be both after merge and after split at the same time");
  }
  int shard_pfx_len = ton::shard_prefix_length(shard.shard);
  if (after_split_ && !shard_pfx_len) {
    return fatal_error("a block with empty shard prefix cannot be after split");
  }
  if (after_merge_ && shard_pfx_len >= 60) {
    return fatal_error("a block split 60 times cannot be after merge");
  }
  if (is_key_block_ && !shard.is_masterchain()) {
    return fatal_error("a non-masterchain block cannot be a key block");
  }
  block::gen::BlockExtra::Record extra;
  if (!is_aux) {
    // FIXME: remove "is_key_block_ &&" later
    if (is_key_block_ && !tlb::unpack_cell(std::move(blk.extra), extra)) {
      return fatal_error("cannot unpack extra header of block "s + blk_id.to_str());
    }
  }
  if (is_key_block_ && !is_aux) {
    // visit validator-set related fields in key blocks
    block::gen::McBlockExtra::Record mc_extra;
    if (!(tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra) && mc_extra.key_block &&
          mc_extra.config.not_null())) {
      return fatal_error("cannot unpack extra header of key masterchain block "s + blk_id.to_str());
    }
    auto cfg = block::Config::unpack_config(std::move(mc_extra.config));
    if (cfg.is_error()) {
      return fatal_error("cannot extract configuration from extra header of key masterchain block "s + blk_id.to_str() +
                         " : " + cfg.move_as_error().to_string());
    }
    auto res = cfg.move_as_ok()->visit_validator_params();
    if (res.is_error()) {
      return fatal_error("cannot extract validator set configuration from extra header of key masterchain block "s +
                         blk_id.to_str() + " : " + res.to_string());
    }
  }
  if (is_aux) {
    if (!is_key_block_) {
      return fatal_error("auxiliary proof passed for verification of the proof of block "s + id_.to_str() +
                         " belongs to non-key block " + key_id_.to_str());
    }
    auto config_r = block::Config::extract_from_key_block(virt_root, block::Config::needValidatorSet);
    if (config_r.is_error()) {
      return fatal_error(config_r.move_as_error());
    }
    auto config = config_r.move_as_ok();
    if (!config) {
      return fatal_error("cannot extract configuration from previous key block " + key_id_.to_str());
    }
    ValidatorSetCompute vs_comp;
    auto res = vs_comp.init(config.get());
    if (res.is_error()) {
      return fatal_error(std::move(res));
    }
    vset_ = vs_comp.get_validator_set(id_.shard_full(), keep_utime, keep_cc_seqno);
    if (vset_.is_null()) {
      return fatal_error("cannot extract current validator set for block "s + id_.to_str() +
                         " from previous key block " + key_id_.to_str());
    }
  }
  return true;
}

void CheckProof::start_up() {
  alarm_timestamp() = timeout_;

  auto res = vm::std_boc_deserialize(proof_->data());
  if (res.is_error()) {
    abort_query(res.move_as_error());
    return;
  }
  proof_root_ = res.move_as_ok();

  if (mode_ == m_relproof) {
    CHECK(old_proof_.not_null());
    res = vm::std_boc_deserialize(old_proof_->data());
    if (res.is_error()) {
      abort_query(res.move_as_error());
      return;
    }
    old_proof_root_ = res.move_as_ok();
  }

  try {
    if (!init_parse()) {
      fatal_error("cannot parse proof for block "s + id_.to_str());
      return;
    }
    if (mode_ == m_relproof) {
      if (!init_parse(true)) {
        fatal_error("cannot parse proof of previous key block "s + key_id_.to_str());
        return;
      }
      if (!init_parse()) {
        fatal_error("cannot parse proof for block "s + id_.to_str());
        return;
      }
    }
  } catch (vm::VmError err) {
    fatal_error("error while processing Merkle proof: "s + err.get_msg());
    return;
  } catch (vm::VmVirtError err) {
    fatal_error("error while processing Merkle proof: "s + err.get_msg());
    return;
  }

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, id_,
                          true, [SelfId = actor_id(this)](td::Result<BlockHandle> R) {
                            if (R.is_error()) {
                              td::actor::send_closure(SelfId, &CheckProof::abort_query, R.move_as_error());
                            } else {
                              td::actor::send_closure(SelfId, &CheckProof::got_block_handle, R.move_as_ok());
                            }
                          });
}

void CheckProof::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);
  CHECK(handle_);
  if (!is_proof() || skip_check_signatures_) {
    got_block_handle_2(handle_);
    return;
  }
  if (handle_->inited_proof()) {
    finish_query();
    return;
  }
  CHECK(is_proof() && prev_.size() == 1);
  if (mode_ == m_relproof) {
    CHECK(vset_.not_null());
    check_signatures(vset_);
    return;
  }
  if (mode_ == m_relstate) {
    process_masterchain_state();
    return;
  }
  td::actor::send_closure(manager_, &ValidatorManager::wait_block_state_short, prev_[0], priority(),
                          timeout_, [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
                            check_send_error(SelfId, R) ||
                                td::actor::send_closure_bool(SelfId, &CheckProof::got_masterchain_state,
                                                             td::Ref<MasterchainState>{R.move_as_ok()});
                          });
}

void CheckProof::got_masterchain_state(td::Ref<MasterchainState> state) {
  CHECK(is_proof());
  state_ = std::move(state);

  if (state_->root_hash() != state_old_hash_) {
    fatal_error(PSTRING() << "invalid previous state hash in proof: expected " << state_->root_hash().to_hex()
                          << ", found in update " << state_old_hash_.to_hex());
    return;
  }
  vset_ = state_->get_validator_set(id_.shard_full());
  check_signatures(vset_);
}

void CheckProof::process_masterchain_state() {
  CHECK(is_proof());
  CHECK(state_.not_null());

  auto id = state_->get_block_id();
  if (!id.is_masterchain()) {
    fatal_error("cannot check a masterchain block proof starting from non-masterchain state for "s + id.to_str());
    return;
  }
  if (!is_masterchain()) {
    fatal_error("cannot check a non-masterchain block proof starting from masterchain state");
    return;
  }
  if (id.seqno() < prev_key_seqno_) {
    fatal_error(PSTRING() << "cannot check masterchain block proof for " << id_.to_str()
                          << " starting from masterchain state for " << id.to_str()
                          << " older than the previous key block with seqno " << prev_key_seqno_);
    return;
  }
  if (id.seqno() >= id_.seqno()) {
    fatal_error("cannot check masterchain block proof for "s + id_.to_str() +
                " starting from newer masterchain state for " + id.to_str());
    return;
  }
  auto state_q = Ref<MasterchainStateQ>(state_);
  CHECK(state_q.not_null());
  vset_ = state_q->get_validator_set(id_.shard_full(), created_at_, catchain_seqno_);
  check_signatures(vset_);
}

void CheckProof::check_signatures(Ref<ValidatorSet> s) {
  if (s->get_catchain_seqno() != catchain_seqno_) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, PSTRING() << "bad validator catchain seqno: expected "
                                                                       << s->get_catchain_seqno() << ", found "
                                                                       << catchain_seqno_));
    return;
  }
  if (s->get_validator_set_hash() != validator_hash_) {
    abort_query(td::Status::Error(ErrorCode::protoviolation, PSTRING() << "bad validator set hash: expected "
                                                                       << s->get_validator_set_hash() << ", found "
                                                                       << validator_hash_));
    return;
  }

  if (sig_root_.is_null()) {
    fatal_error("no block signatures present in proof to check");
    return;
  }

  auto sigs = BlockSignatureSetQ::fetch(sig_root_);
  if (sigs.is_null()) {
    fatal_error("cannot deserialize signature set");
    return;
  }
  if (sigs->signatures().size() != sig_count_) {
    fatal_error(PSTRING() << "signature count mismatch: present " << sigs->signatures().size() << ", declared "
                          << sig_count_);
    return;
  }

  auto S = s->check_signatures(id_.root_hash, id_.file_hash, sigs);
  if (S.is_error()) {
    abort_query(S.move_as_error());
    return;
  }
  auto s_weight = S.move_as_ok();
  if (s_weight != sig_weight_) {
    fatal_error(PSTRING() << "total signature weight mismatch: declared " << sig_weight_ << ", actual " << s_weight);
    return;
  }
  sig_ok_ = true;

  if (handle_) {
    got_block_handle_2(handle_);
  } else {
    td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, id_,
                            true, [SelfId = actor_id(this)](td::Result<BlockHandle> R) {
                              check_send_error(SelfId, R) ||
                                  td::actor::send_closure_bool(SelfId, &CheckProof::got_block_handle_2, R.move_as_ok());
                            });
  }
}

void CheckProof::got_block_handle_2(BlockHandle handle) {
  handle_ = std::move(handle);

  handle_->set_split(before_split_);
  handle_->set_merge(after_merge_);
  handle_->set_is_key_block(is_key_block_);
  handle_->set_state_root_hash(state_hash_);
  handle_->set_logical_time(lt_);
  handle_->set_unix_time(created_at_);
  for (auto &prev : prev_) {
    handle_->set_prev(prev);
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    check_send_error(SelfId, R) || td::actor::send_closure_bool(SelfId, &CheckProof::finish_query);
  });
  if (skip_check_signatures_) {
    // do not save proof if we skipped signatures
    handle_->flush(manager_, handle_, std::move(P));
  } else if (is_proof()) {
    auto proof = Ref<Proof>(proof_);
    CHECK(proof.not_null());
    CHECK(sig_ok_);
    td::actor::send_closure_later(manager_, &ValidatorManager::set_block_proof, handle_, std::move(proof),
                                  std::move(P));
  } else {
    CHECK(proof_.not_null());
    td::actor::send_closure_later(manager_, &ValidatorManager::set_block_proof_link, handle_, proof_, std::move(P));
  }
}

}  // namespace validator

}  // namespace ton
