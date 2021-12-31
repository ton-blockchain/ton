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
#include "validate-query.hpp"
#include "top-shard-descr.hpp"
#include "validator-set.hpp"
#include "adnl/utils.hpp"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "vm/boc.h"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/output-queue-merger.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cells/MerkleUpdate.h"
#include "common/errorlog.h"
#include <ctime>

namespace ton {

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

std::string ErrorCtx::as_string() const {
  std::string a;
  for (const auto& s : entries_) {
    a += s;
    a += " : ";
  }
  return a;
}

ValidateQuery::ValidateQuery(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                             std::vector<BlockIdExt> prev, BlockCandidate candidate, Ref<ValidatorSet> validator_set,
                             td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                             td::Promise<ValidateCandidateResult> promise, bool is_fake)
    : shard_(shard)
    , id_(candidate.id)
    , min_ts(min_ts)
    , min_mc_block_id(min_masterchain_block_id)
    , prev_blocks(std::move(prev))
    , block_candidate(std::move(candidate))
    , validator_set_(std::move(validator_set))
    , manager(std::move(manager))
    , timeout(timeout)
    , main_promise(std::move(promise))
    , is_fake_(is_fake)
    , shard_pfx_(shard_.shard)
    , shard_pfx_len_(ton::shard_prefix_length(shard_)) {
  proc_hash_.zero();
}

void ValidateQuery::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void ValidateQuery::abort_query(td::Status error) {
  (void)fatal_error(std::move(error));
}

bool ValidateQuery::reject_query(std::string error, td::BufferSlice reason) {
  error = error_ctx() + error;
  LOG(ERROR) << "REJECT: aborting validation of block candidate for " << shard_.to_str() << " : " << error;
  if (main_promise) {
    errorlog::ErrorLog::log(PSTRING() << "REJECT: aborting validation of block candidate for " << shard_.to_str()
                                      << " : " << error << ": data=" << block_candidate.id.file_hash.to_hex()
                                      << " collated_data=" << block_candidate.collated_file_hash.to_hex());
    errorlog::ErrorLog::log_file(block_candidate.data.clone());
    errorlog::ErrorLog::log_file(block_candidate.collated_data.clone());
    main_promise.set_result(CandidateReject{std::move(error), std::move(reason)});
  }
  stop();
  return false;
}

bool ValidateQuery::reject_query(std::string err_msg, td::Status error, td::BufferSlice reason) {
  error.ensure_error();
  return reject_query(err_msg + " : " + error.to_string(), std::move(reason));
}

bool ValidateQuery::soft_reject_query(std::string error, td::BufferSlice reason) {
  error = error_ctx() + error;
  LOG(ERROR) << "SOFT REJECT: aborting validation of block candidate for " << shard_.to_str() << " : " << error;
  if (main_promise) {
    errorlog::ErrorLog::log(PSTRING() << "SOFT REJECT: aborting validation of block candidate for " << shard_.to_str()
                                      << " : " << error << ": data=" << block_candidate.id.file_hash.to_hex()
                                      << " collated_data=" << block_candidate.collated_file_hash.to_hex());
    errorlog::ErrorLog::log_file(block_candidate.data.clone());
    errorlog::ErrorLog::log_file(block_candidate.collated_data.clone());
    main_promise.set_result(CandidateReject{std::move(error), std::move(reason)});
  }
  stop();
  return false;
}

bool ValidateQuery::fatal_error(td::Status error) {
  error.ensure_error();
  LOG(ERROR) << "aborting validation of block candidate for " << shard_.to_str() << " : " << error.to_string();
  if (main_promise) {
    auto c = error.code();
    if (c <= -667 && c >= -670) {
      errorlog::ErrorLog::log(PSTRING() << "FATAL ERROR: aborting validation of block candidate for " << shard_.to_str()
                                        << " : " << error << ": data=" << block_candidate.id.file_hash.to_hex()
                                        << " collated_data=" << block_candidate.collated_file_hash.to_hex());
      errorlog::ErrorLog::log_file(block_candidate.data.clone());
      errorlog::ErrorLog::log_file(block_candidate.collated_data.clone());
    }
    main_promise(std::move(error));
  }
  stop();
  return false;
}

bool ValidateQuery::fatal_error(int err_code, std::string err_msg) {
  return fatal_error(td::Status::Error(err_code, error_ctx() + err_msg));
}

bool ValidateQuery::fatal_error(int err_code, std::string err_msg, td::Status error) {
  error.ensure_error();
  return fatal_error(err_code, err_msg + " : " + error.to_string());
}

bool ValidateQuery::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, error_ctx() + err_msg));
}

void ValidateQuery::finish_query() {
  if (main_promise) {
    main_promise.set_result(now_);
  }
  stop();
}

/*
 * 
 *   INITIAL PARSE & LOAD REQUIRED DATA
 * 
 */

void ValidateQuery::start_up() {
  LOG(INFO) << "validate query for " << block_candidate.id.to_str() << " started";
  alarm_timestamp() = timeout;
  rand_seed_.set_zero();
  created_by_ = block_candidate.pubkey;

  CHECK(id_ == block_candidate.id);
  if (ShardIdFull(id_) != shard_) {
    soft_reject_query(PSTRING() << "block candidate belongs to shard " << ShardIdFull(id_).to_str()
                                << " different from current shard " << shard_.to_str());
    return;
  }
  if (workchain() != ton::masterchainId && workchain() != ton::basechainId) {
    soft_reject_query("can validate block candidates only for masterchain (-1) and base workchain (0)");
    return;
  }
  if (!shard_.is_valid_ext()) {
    reject_query("requested to validate a block for an invalid shard");
    return;
  }
  td::uint64 x = td::lower_bit64(shard_.shard);
  if (x < 8) {
    reject_query("a shard cannot be split more than 60 times");
    return;
  }
  if (is_masterchain() && !shard_.is_masterchain_ext()) {
    reject_query("sub-shards cannot exist in the masterchain");
    return;
  }
  if (is_masterchain() && !prev_blocks.size()) {
    min_mc_block_id = BlockIdExt{BlockId{masterchainId, shardIdAll, 0}};
  }
  if (!ShardIdFull(min_mc_block_id).is_masterchain_ext()) {
    soft_reject_query("requested minimal masterchain block id does not belong to masterchain");
    return;
  }
  if (prev_blocks.size() > 2) {
    soft_reject_query("cannot have more than two previous blocks");
    return;
  }
  if (!prev_blocks.size()) {
    soft_reject_query("must have one or two previous blocks to generate a next block");
    return;
  }
  if (prev_blocks.size() == 2) {
    if (is_masterchain()) {
      soft_reject_query("cannot merge shards in masterchain");
      return;
    }
    if (!(shard_is_parent(shard_, ShardIdFull(prev_blocks[0])) &&
          shard_is_parent(shard_, ShardIdFull(prev_blocks[1])) && prev_blocks[0].id.shard < prev_blocks[1].id.shard)) {
      soft_reject_query(
          "the two previous blocks for a merge operation are not siblings or are not children of current shard");
      return;
    }
    for (const auto& blk : prev_blocks) {
      if (!blk.id.seqno) {
        soft_reject_query("previous blocks for a block merge operation must have non-zero seqno");
        return;
      }
    }
    after_merge_ = true;
    // soft_reject_query("merging shards is not implemented yet");
    // return;
  } else {
    CHECK(prev_blocks.size() == 1);
    // creating next block
    if (!ShardIdFull(prev_blocks[0]).is_valid_ext()) {
      soft_reject_query("previous block does not have a valid id");
      return;
    }
    if (ShardIdFull(prev_blocks[0]) != shard_) {
      after_split_ = true;
      if (!shard_is_parent(ShardIdFull(prev_blocks[0]), shard_)) {
        soft_reject_query("previous block does not belong to the shard we are generating a new block for");
        return;
      }
      if (is_masterchain()) {
        soft_reject_query("cannot split shards in masterchain");
        return;
      }
    }
    if (is_masterchain() && min_mc_block_id.id.seqno > prev_blocks[0].id.seqno) {
      soft_reject_query(
          "cannot refer to specified masterchain block because it is later than the immediately preceding "
          "masterchain block");
      return;
    }
    if (after_split_) {
      // soft_reject_query("splitting shards not implemented yet");
      // return;
    }
  }
  // 2. learn latest masterchain state and block id
  LOG(DEBUG) << "sending get_top_masterchain_state_block() to Manager";
  ++pending;
  td::actor::send_closure_later(manager, &ValidatorManager::get_top_masterchain_state_block,
                                [self = get_self()](td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
                                  LOG(DEBUG) << "got answer to get_top_masterchain_state_block";
                                  td::actor::send_closure_later(
                                      std::move(self), &ValidateQuery::after_get_latest_mc_state, std::move(res));
                                });
  // 3. load state(s) corresponding to previous block(s)
  prev_states.resize(prev_blocks.size());
  for (int i = 0; (unsigned)i < prev_blocks.size(); i++) {
    // 3.1. load state
    LOG(DEBUG) << "sending wait_block_state() query #" << i << " for " << prev_blocks[i].to_str() << " to Manager";
    ++pending;
    td::actor::send_closure_later(manager, &ValidatorManager::wait_block_state_short, prev_blocks[i], priority(),
                                  timeout, [self = get_self(), i](td::Result<Ref<ShardState>> res) -> void {
                                    LOG(DEBUG) << "got answer to wait_block_state_short query #" << i;
                                    td::actor::send_closure_later(
                                        std::move(self), &ValidateQuery::after_get_shard_state, i, std::move(res));
                                  });
  }
  // 4. unpack block candidate (while necessary data is being loaded)
  if (!unpack_block_candidate()) {
    reject_query("error unpacking block candidate");
    return;
  }
  // 5. request masterchain state referred to in the block
  if (!is_masterchain()) {
    ++pending;
    td::actor::send_closure_later(manager, &ValidatorManager::wait_block_state_short, mc_blkid_, priority(), timeout,
                                  [self = get_self()](td::Result<Ref<ShardState>> res) {
                                    LOG(DEBUG) << "got answer to wait_block_state() query for masterchain block";
                                    td::actor::send_closure_later(std::move(self), &ValidateQuery::after_get_mc_state,
                                                                  std::move(res));
                                  });
    // 5.1. request corresponding block handle
    ++pending;
    td::actor::send_closure_later(manager, &ValidatorManager::get_block_handle, mc_blkid_, true,
                                  [self = get_self()](td::Result<BlockHandle> res) {
                                    LOG(DEBUG) << "got answer to get_block_handle() query for masterchain block";
                                    td::actor::send_closure_later(std::move(self), &ValidateQuery::got_mc_handle,
                                                                  std::move(res));
                                  });
  } else {
    if (prev_blocks[0] != mc_blkid_) {
      soft_reject_query("cannot validate masterchain block "s + id_.to_str() +
                        " because it refers to masterchain block " + mc_blkid_.to_str() +
                        " but its (expected) previous block is " + prev_blocks[0].to_str());
      return;
    }
  }
  // ...
  CHECK(pending);
}

// unpack block candidate, and check root hash and file hash
bool ValidateQuery::unpack_block_candidate() {
  vm::BagOfCells boc1, boc2;
  // 1. deserialize block itself
  FileHash fhash = block::compute_file_hash(block_candidate.data);
  if (fhash != id_.file_hash) {
    return reject_query(PSTRING() << "block candidate has invalid file hash: declared " << id_.file_hash.to_hex()
                                  << ", actual " << fhash.to_hex());
  }
  auto res1 = boc1.deserialize(block_candidate.data);
  if (res1.is_error()) {
    return reject_query("cannot deserialize block", res1.move_as_error());
  }
  if (boc1.get_root_count() != 1) {
    return reject_query("block BoC must contain exactly one root");
  }
  block_root_ = boc1.get_root_cell();
  CHECK(block_root_.not_null());
  // 2. check that root_hash equals the announced one
  RootHash rhash{block_root_->get_hash().bits()};
  if (rhash != id_.root_hash) {
    return reject_query(PSTRING() << "block candidate has invalid root hash: declared " << id_.root_hash.to_hex()
                                  << ", actual " << rhash.to_hex());
  }
  // 3. initial block parse
  {
    auto guard = error_ctx_add_guard("parsing block header");
    try {
      if (!init_parse()) {
        return reject_query("invalid block header");
      }
    } catch (vm::VmError& err) {
      return reject_query(err.get_msg());
    } catch (vm::VmVirtError& err) {
      return reject_query(err.get_msg());
    }
  }
  // ...
  // 8. deserialize collated data
  auto res2 = boc2.deserialize(block_candidate.collated_data);
  if (res2.is_error()) {
    return reject_query("cannot deserialize collated data", res2.move_as_error());
  }
  int n = boc2.get_root_count();
  CHECK(n >= 0);
  for (int i = 0; i < n; i++) {
    collated_roots_.emplace_back(boc2.get_root_cell(i));
  }
  // 9. extract/classify collated data
  return extract_collated_data();
}

bool ValidateQuery::init_parse() {
  CHECK(block_root_.not_null());
  std::vector<BlockIdExt> prev_blks;
  bool after_split;
  auto res = block::unpack_block_prev_blk_try(block_root_, id_, prev_blks, mc_blkid_, after_split);
  if (res.is_error()) {
    return reject_query("cannot unpack block header : "s + res.to_string());
  }
  CHECK(mc_blkid_.id.is_masterchain_ext());
  mc_seqno_ = mc_blkid_.seqno();
  if (prev_blks.size() != prev_blocks.size()) {
    return soft_reject_query(PSTRING() << "block header declares " << prev_blks.size()
                                       << " previous blocks, but we are given " << prev_blocks.size());
  }
  for (std::size_t i = 0; i < prev_blks.size(); i++) {
    if (prev_blks[i] != prev_blocks[i]) {
      return soft_reject_query(PSTRING() << "previous block #" << i + 1 << " mismatch: expected "
                                         << prev_blocks[i].to_str() << ", found in header " << prev_blks[i]);
    }
  }
  if (after_split != after_split_) {
    // ??? impossible
    return fatal_error("after_split mismatch in block header");
  }
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  block::gen::ExtBlkRef::Record mcref;  // _ ExtBlkRef = BlkMasterInfo;
  ShardIdFull shard;
  if (!(tlb::unpack_cell(block_root_, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
        block::tlb::t_ShardIdent.unpack(info.shard.write(), shard) &&
        block::gen::BlkPrevInfo{info.after_merge}.validate_ref(info.prev_ref) &&
        (!info.not_master || tlb::unpack_cell(info.master_ref, mcref)) && tlb::unpack_cell(blk.extra, extra))) {
    return reject_query("cannot unpack block header");
  }
  if (shard != shard_) {
    return reject_query("shard mismatch in the block header");
  }
  state_update_ = blk.state_update;
  vm::CellSlice upd_cs{vm::NoVmSpec(), blk.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return fatal_error("invalid Merkle update in block");
  }
  global_id_ = blk.global_id;
  vert_seqno_ = info.vert_seq_no;
  prev_state_hash_ = upd_cs.prefetch_ref(0)->get_hash(0).bits();
  state_hash_ = upd_cs.prefetch_ref(1)->get_hash(0).bits();
  start_lt_ = info.start_lt;
  end_lt_ = info.end_lt;
  now_ = info.gen_utime;
  // after_merge_ = info.after_merge;
  before_split_ = info.before_split;
  // after_split_ = info.after_split;
  want_merge_ = info.want_merge;
  want_split_ = info.want_split;
  is_key_block_ = info.key_block;
  prev_key_seqno_ = info.prev_key_block_seqno;
  CHECK(after_split_ == info.after_split);
  if (is_key_block_) {
    LOG(INFO) << "validating key block " << id_.to_str();
  }
  if (start_lt_ >= end_lt_) {
    return reject_query("block has start_lt greater than or equal to end_lt");
  }
  if (shard.is_masterchain() && (info.after_merge | info.before_split | info.after_split)) {
    return reject_query("block header declares split/merge for a masterchain block");
  }
  if (info.after_merge && info.after_split) {
    return reject_query("a block cannot be both after merge and after split at the same time");
  }
  int shard_pfx_len = ton::shard_prefix_length(shard);
  if (info.after_split && !shard_pfx_len) {
    return reject_query("a block with empty shard prefix cannot be after split");
  }
  if (info.after_merge && shard_pfx_len >= 60) {
    return reject_query("a block split 60 times cannot be after merge");
  }
  if (is_key_block_ && !shard.is_masterchain()) {
    return reject_query("a non-masterchain block cannot be a key block");
  }
  if (info.vert_seqno_incr) {
    // what about non-masterchain blocks?
    return reject_query("new blocks cannot have vert_seqno_incr set");
  }
  if (info.after_merge != after_merge_) {
    return reject_query("after_merge value mismatch in block header");
  }
  rand_seed_ = extra.rand_seed;
  if (created_by_ != extra.created_by) {
    return reject_query("block candidate "s + id_.to_str() + " has creator " + created_by_.to_hex() +
                        " but the block header contains different value " + extra.created_by.to_hex());
  }
  if (is_masterchain()) {
    if (!extra.custom->size_refs()) {
      return reject_query("masterchain block candidate without McBlockExtra");
    }
    block::gen::McBlockExtra::Record mc_extra;
    if (!tlb::unpack_cell(extra.custom->prefetch_ref(), mc_extra)) {
      return reject_query("cannot unpack McBlockExtra");
    }
    if (mc_extra.key_block != is_key_block_) {
      return reject_query("key_block flag mismatch in BlockInfo and McBlockExtra");
    }
    shard_hashes_ = mc_extra.shard_hashes;
    if (is_key_block_) {
      blk_config_params_ = mc_extra.config;
    }
    fees_import_dict_ = std::make_unique<vm::AugmentedDictionary>(mc_extra.shard_fees, 96, block::tlb::aug_ShardFees);
    // prev_blk_signatures:(HashmapE 16 CryptoSignaturePair)
    if (mc_extra.r1.prev_blk_signatures->have_refs()) {
      prev_signatures_ = BlockSignatureSetQ::fetch(mc_extra.r1.prev_blk_signatures->prefetch_ref());
      if (prev_signatures_.is_null() || !prev_signatures_->size()) {
        return reject_query("cannot deserialize signature set for the previous masterchain block in prev_signatures");
      }
    }
    recover_create_msg_ = mc_extra.r1.recover_create_msg->prefetch_ref();
    mint_msg_ = mc_extra.r1.mint_msg->prefetch_ref();
    new_shard_conf_ = std::make_unique<block::ShardConfig>(shard_hashes_->prefetch_ref());
    // NB: new_shard_conf_->mc_shard_hash_ is unset at this point
  } else if (extra.custom->size_refs()) {
    return reject_query("non-masterchain block cannot have McBlockExtra");
  }
  // ...
  return true;
}

bool ValidateQuery::extract_collated_data_from(Ref<vm::Cell> croot, int idx) {
  bool is_special = false;
  auto cs = vm::load_cell_slice_special(croot, is_special);
  if (!cs.is_valid()) {
    return reject_query("cannot load root cell");
  }
  if (is_special) {
    if (cs.special_type() != vm::Cell::SpecialType::MerkleProof) {
      return reject_query("it is a special cell, but not a Merkle proof root");
    }
    auto virt_root = vm::MerkleProof::virtualize(croot, 1);
    if (virt_root.is_null()) {
      return reject_query("invalid Merkle proof");
    }
    RootHash virt_hash{virt_root->get_hash().bits()};
    LOG(DEBUG) << "collated datum # " << idx << " is a Merkle proof with root hash " << virt_hash.to_hex();
    auto ins = virt_roots_.emplace(virt_hash, std::move(virt_root));
    if (!ins.second) {
      return reject_query("Merkle proof with duplicate virtual root hash "s + virt_hash.to_hex());
    }
    return true;
  }
  if (block::gen::t_TopBlockDescrSet.has_valid_tag(cs)) {
    LOG(DEBUG) << "collated datum # " << idx << " is a TopBlockDescrSet";
    if (!block::gen::t_TopBlockDescrSet.validate_upto(10000, cs)) {
      return reject_query("invalid TopBlockDescrSet");
    }
    if (top_shard_descr_dict_) {
      return reject_query("duplicate TopBlockDescrSet in collated data");
    }
    top_shard_descr_dict_ = std::make_unique<vm::Dictionary>(cs.prefetch_ref(), 96);
    return true;
  }
  LOG(WARNING) << "collated datum # " << idx << " has unknown type (magic " << cs.prefetch_ulong(32) << "), ignoring";
  return true;
}

// processes further and sorts data in collated_roots_
bool ValidateQuery::extract_collated_data() {
  int i = -1;
  for (auto croot : collated_roots_) {
    ++i;
    auto guard = error_ctx_add_guard(PSTRING() << "collated datum #" << i);
    try {
      if (!extract_collated_data_from(croot, i)) {
        return reject_query("cannot unpack collated datum");
      }
    } catch (vm::VmError& err) {
      return reject_query(PSTRING() << "vm error " << err.get_msg());
    } catch (vm::VmVirtError& err) {
      return reject_query(PSTRING() << "virtualization error " << err.get_msg());
    }
  }
  return true;
}

void ValidateQuery::after_get_latest_mc_state(td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
  LOG(DEBUG) << "in ValidateQuery::after_get_latest_mc_state()";
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto state_blk = res.move_as_ok();
  latest_mc_state_ = Ref<MasterchainStateQ>(std::move(state_blk.first));
  latest_mc_blkid_ = state_blk.second;
  latest_mc_seqno_ = latest_mc_blkid_.seqno();
  if (latest_mc_state_.is_null()) {
    fatal_error(-666, "unable to load latest masterchain state");
    return;
  }
  if (!ShardIdFull(latest_mc_blkid_).is_masterchain_ext()) {
    fatal_error(-666, "invalid last masterchain block id "s + latest_mc_blkid_.to_str());
    return;
  }
  if (latest_mc_blkid_.seqno() < min_mc_block_id.seqno()) {
    fatal_error(-666, "requested to validate a block referring to an unknown future masterchain block");
    return;
  }
  if (latest_mc_blkid_ != latest_mc_state_->get_block_id()) {
    if (ShardIdFull(latest_mc_blkid_) != ShardIdFull(latest_mc_state_->get_block_id()) || latest_mc_seqno_) {
      fatal_error(-666, "latest masterchain state does not match latest masterchain block");
      return;
    }
  }
  if (!pending) {
    if (!try_validate()) {
      fatal_error("cannot validate new block");
    }
  }
}

void ValidateQuery::after_get_mc_state(td::Result<Ref<ShardState>> res) {
  LOG(DEBUG) << "in ValidateQuery::after_get_mc_state() for " << mc_blkid_.to_str();
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  if (!process_mc_state(Ref<MasterchainState>(res.move_as_ok()))) {
    fatal_error("cannot process masterchain state for "s + mc_blkid_.to_str());
    return;
  }
  if (!pending) {
    if (!try_validate()) {
      fatal_error("cannot validate new block");
    }
  }
}

void ValidateQuery::got_mc_handle(td::Result<BlockHandle> res) {
  LOG(DEBUG) << "in ValidateQuery::got_mc_handle() for " << mc_blkid_.to_str();
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto handle = res.move_as_ok();
  if (!handle->inited_proof() && mc_blkid_.seqno()) {
    fatal_error(-666, "reference masterchain block "s + mc_blkid_.to_str() + " for block " + id_.to_str() +
                          " does not have a valid proof");
    return;
  }
}

void ValidateQuery::after_get_shard_state(int idx, td::Result<Ref<ShardState>> res) {
  LOG(DEBUG) << "in ValidateQuery::after_get_shard_state(" << idx << ")";
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  // got state of previous block #i
  CHECK((unsigned)idx < prev_blocks.size());
  prev_states.at(idx) = res.move_as_ok();
  CHECK(prev_states[idx].not_null());
  CHECK(prev_states[idx]->get_shard() == ShardIdFull(prev_blocks[idx]));
  CHECK(prev_states[idx]->root_cell().not_null());
  if (is_masterchain()) {
    CHECK(!idx);
    if (prev_blocks[0] != mc_blkid_) {
      fatal_error("impossible situation: previous block "s + prev_blocks[0].to_str() + " is not the block " +
                  mc_blkid_.to_str() + " referred to by the current block");
      return;
    }
    if (!process_mc_state(static_cast<Ref<MasterchainState>>(prev_states[0]))) {
      fatal_error("cannot process masterchain state for "s + mc_blkid_.to_str());
      return;
    }
  }
  if (!pending) {
    if (!try_validate()) {
      fatal_error("cannot validate new block");
    }
  }
}

bool ValidateQuery::process_mc_state(Ref<MasterchainState> mc_state) {
  if (mc_state.is_null()) {
    return fatal_error("could not obtain reference masterchain state "s + mc_blkid_.to_str());
  }
  if (mc_state->get_block_id() != mc_blkid_) {
    if (ShardIdFull(mc_blkid_) != ShardIdFull(mc_state->get_block_id()) || mc_blkid_.seqno()) {
      return fatal_error("reference masterchain state for "s + mc_blkid_.to_str() + " is in fact for different block " +
                         mc_state->get_block_id().to_str());
    }
  }
  mc_state_ = Ref<MasterchainStateQ>(std::move(mc_state));
  mc_state_root_ = mc_state_->root_cell();
  if (mc_state_root_.is_null()) {
    return fatal_error(-666, "unable to load reference masterchain state "s + mc_blkid_.to_str());
  }
  if (!try_unpack_mc_state()) {
    return fatal_error(-666, "cannot unpack reference masterchain state "s + mc_blkid_.to_str());
  }
  return register_mc_state(mc_state_);
}

bool ValidateQuery::try_unpack_mc_state() {
  LOG(DEBUG) << "unpacking reference masterchain state";
  auto guard = error_ctx_add_guard("unpack last mc state");
  try {
    if (mc_state_.is_null()) {
      return fatal_error(-666, "no previous masterchain state present");
    }
    mc_state_root_ = mc_state_->root_cell();
    if (mc_state_root_.is_null()) {
      return fatal_error(-666, "latest masterchain state does not have a root cell");
    }
    auto res = block::ConfigInfo::extract_config(
        mc_state_root_,
        block::ConfigInfo::needShardHashes | block::ConfigInfo::needLibraries | block::ConfigInfo::needValidatorSet |
            block::ConfigInfo::needWorkchainInfo | block::ConfigInfo::needStateExtraRoot |
            block::ConfigInfo::needCapabilities |
            (is_masterchain() ? block::ConfigInfo::needAccountsRoot | block::ConfigInfo::needSpecialSmc : 0));
    if (res.is_error()) {
      return fatal_error(-666, "cannot extract configuration from reference masterchain state "s + mc_blkid_.to_str() +
                                   " : " + res.move_as_error().to_string());
    }
    config_ = res.move_as_ok();
    CHECK(config_);
    config_->set_block_id_ext(mc_blkid_);
    ihr_enabled_ = config_->ihr_enabled();
    create_stats_enabled_ = config_->create_stats_enabled();
    if (config_->has_capabilities() && (config_->get_capabilities() & ~supported_capabilities())) {
      LOG(ERROR) << "block generation capabilities " << config_->get_capabilities()
                 << " have been enabled in global configuration, but we support only " << supported_capabilities()
                 << " (upgrade validator software?)";
    }
    if (config_->get_global_version() > supported_version()) {
      LOG(ERROR) << "block version " << config_->get_global_version()
                 << " have been enabled in global configuration, but we support only " << supported_version()
                 << " (upgrade validator software?)";
    }

    old_shard_conf_ = std::make_unique<block::ShardConfig>(*config_);
    if (!is_masterchain()) {
      new_shard_conf_ = std::make_unique<block::ShardConfig>(*config_);
    } else {
      CHECK(new_shard_conf_);
      new_shard_conf_->set_mc_hash(old_shard_conf_->get_mc_hash());
      CHECK(!mc_seqno_ || new_shard_conf_->get_mc_hash().not_null());
    }
    if (global_id_ != config_->get_global_blockchain_id()) {
      return reject_query(PSTRING() << "blockchain global id mismatch: new block has " << global_id_
                                    << " while the masterchain configuration expects "
                                    << config_->get_global_blockchain_id());
    }
    if (vert_seqno_ != config_->get_vert_seqno()) {
      return reject_query(PSTRING() << "vertical seqno mismatch: new block has " << vert_seqno_
                                    << " while the masterchain configuration expects " << config_->get_vert_seqno());
    }
    prev_key_block_exists_ = config_->get_last_key_block(prev_key_block_, prev_key_block_lt_);
    if (prev_key_block_exists_) {
      prev_key_block_seqno_ = prev_key_block_.seqno();
    } else {
      prev_key_block_seqno_ = 0;
    }
    if (prev_key_seqno_ != prev_key_block_seqno_) {
      return reject_query(PSTRING() << "previous key block seqno value in candidate block header is " << prev_key_seqno_
                                    << " while the correct value corresponding to reference masterchain state "
                                    << mc_blkid_.to_str() << " is " << prev_key_block_seqno_);
    }
    auto limits = config_->get_block_limits(is_masterchain());
    if (limits.is_error()) {
      return fatal_error(limits.move_as_error());
    }
    block_limits_ = limits.move_as_ok();
    if (!fetch_config_params()) {
      return false;
    }
    if (!is_masterchain() && !check_this_shard_mc_info()) {
      return fatal_error("masterchain configuration does not admit creating block "s + id_.to_str());
    }
  } catch (vm::VmError& err) {
    return fatal_error(-666, err.get_msg());
  } catch (vm::VmVirtError& err) {
    return fatal_error(-666, err.get_msg());
  }
  return true;
}

// almost the same as in Collator
bool ValidateQuery::fetch_config_params() {
  old_mparams_ = config_->get_config_param(9);
  {
    auto res = config_->get_storage_prices();
    if (res.is_error()) {
      return fatal_error(res.move_as_error());
    }
    storage_prices_ = res.move_as_ok();
  }
  {
    // recover (not generate) rand seed from block header
    CHECK(!rand_seed_.is_zero());
  }
  {
    // compute compute_phase_cfg / storage_phase_cfg
    auto cell = config_->get_config_param(is_masterchain() ? 20 : 21);
    if (cell.is_null()) {
      return fatal_error("cannot fetch current gas prices and limits from masterchain configuration");
    }
    if (!compute_phase_cfg_.parse_GasLimitsPrices(std::move(cell), storage_phase_cfg_.freeze_due_limit,
                                                  storage_phase_cfg_.delete_due_limit)) {
      return fatal_error("cannot unpack current gas prices and limits from masterchain configuration");
    }
    compute_phase_cfg_.block_rand_seed = rand_seed_;
    compute_phase_cfg_.libraries = std::make_unique<vm::Dictionary>(config_->get_libraries_root(), 256);
    compute_phase_cfg_.global_config = config_->get_root_cell();
  }
  {
    // compute action_phase_cfg
    block::gen::MsgForwardPrices::Record rec;
    auto cell = config_->get_config_param(24);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return fatal_error("cannot fetch masterchain message transfer prices from masterchain configuration");
    }
    action_phase_cfg_.fwd_mc =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    cell = config_->get_config_param(25);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return fatal_error("cannot fetch standard message transfer prices from masterchain configuration");
    }
    action_phase_cfg_.fwd_std =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    action_phase_cfg_.workchains = &config_->get_workchain_list();
    action_phase_cfg_.bounce_msg_body = (config_->has_capability(ton::capBounceMsgBody) ? 256 : 0);
  }
  {
    // fetch block_grams_created
    auto cell = config_->get_config_param(14);
    if (cell.is_null()) {
      basechain_create_fee_ = masterchain_create_fee_ = td::zero_refint();
    } else {
      block::gen::BlockCreateFees::Record create_fees;
      if (!(tlb::unpack_cell(cell, create_fees) &&
            block::tlb::t_Grams.as_integer_to(create_fees.masterchain_block_fee, masterchain_create_fee_) &&
            block::tlb::t_Grams.as_integer_to(create_fees.basechain_block_fee, basechain_create_fee_))) {
        return fatal_error("cannot unpack BlockCreateFees from configuration parameter #14");
      }
    }
  }
  return true;
}

// almost the same as in Collator
bool ValidateQuery::check_prev_block(const BlockIdExt& listed, const BlockIdExt& prev, bool chk_chain_len) {
  if (listed.seqno() > prev.seqno()) {
    return reject_query(PSTRING() << "cannot generate a shardchain block after previous block " << prev.to_str()
                                  << " because masterchain configuration already contains a newer block "
                                  << listed.to_str());
  }
  if (listed.seqno() == prev.seqno() && listed != prev) {
    return reject_query(PSTRING() << "cannot generate a shardchain block after previous block " << prev.to_str()
                                  << " because masterchain configuration lists another block " << listed.to_str()
                                  << " of the same height");
  }
  if (chk_chain_len && prev.seqno() >= listed.seqno() + 8) {
    return reject_query(PSTRING() << "cannot generate next block after " << prev.to_str()
                                  << " because this would lead to an unregistered chain of length > 8 (only "
                                  << listed.to_str() << " is registered in the masterchain)");
  }
  return true;
}

// almost the same as in Collator
bool ValidateQuery::check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev) {
  if (listed != prev) {
    return reject_query(PSTRING() << "cannot generate shardchain block for shard " << shard_.to_str()
                                  << " after previous block " << prev.to_str()
                                  << " because masterchain configuration expects another previous block "
                                  << listed.to_str() << " and we are immediately after a split/merge event");
  }
  return true;
}

// almost the same as in Collator
// (main change: fatal_error -> reject_query)
bool ValidateQuery::check_this_shard_mc_info() {
  wc_info_ = config_->get_workchain_info(workchain());
  if (wc_info_.is_null()) {
    return reject_query(PSTRING() << "cannot create new block for workchain " << workchain()
                                  << " absent from workchain configuration");
  }
  if (!wc_info_->active) {
    return reject_query(PSTRING() << "cannot create new block for disabled workchain " << workchain());
  }
  if (!wc_info_->basic) {
    return reject_query(PSTRING() << "cannot create new block for non-basic workchain " << workchain());
  }
  if (wc_info_->enabled_since && wc_info_->enabled_since > config_->utime) {
    return reject_query(PSTRING() << "cannot create new block for workchain " << workchain()
                                  << " which is not enabled yet");
  }
  if (wc_info_->min_addr_len != 0x100 || wc_info_->max_addr_len != 0x100) {
    return false;
  }
  accept_msgs_ = wc_info_->accept_msgs;
  bool split_allowed = false;
  if (!config_->has_workchain(workchain())) {
    // creating first block for a new workchain
    LOG(INFO) << "creating first block for workchain " << workchain();
    return reject_query(PSTRING() << "cannot create first block for workchain " << workchain()
                                  << " after previous block "
                                  << (prev_blocks.size() ? prev_blocks[0].to_str() : "(null)")
                                  << " because no shard for this workchain is declared yet");
  }
  auto left = config_->get_shard_hash(shard_ - 1, false);
  if (left.is_null()) {
    return reject_query(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                  << " because there is no similar shard in existing masterchain configuration");
  }
  if (left->shard() == shard_) {
    // no split/merge
    if (after_merge_ || after_split_) {
      return reject_query(
          PSTRING() << "cannot generate new shardchain block for " << shard_.to_str()
                    << " after a supposed split or merge event because this event is not reflected in the masterchain");
    }
    if (!check_prev_block(left->blk_, prev_blocks[0])) {
      return false;
    }
    if (left->before_split_) {
      return reject_query(PSTRING() << "cannot generate new unsplit shardchain block for " << shard_.to_str()
                                    << " after previous block " << left->blk_.to_str() << " with before_split set");
    }
    auto sib = config_->get_shard_hash(shard_sibling(shard_));
    if (left->before_merge_ && sib->before_merge_) {
      return reject_query(PSTRING() << "cannot generate new unmerged shardchain block for " << shard_.to_str()
                                    << " after both " << left->blk_.to_str() << " and " << sib->blk_.to_str()
                                    << " set before_merge flags");
    }
    if (left->is_fsm_split()) {
      if (now_ >= left->fsm_utime() && now_ < left->fsm_utime_end()) {
        split_allowed = true;
      }
    }
  } else if (shard_is_parent(shard_, left->shard())) {
    // after merge
    if (!left->before_merge_) {
      return reject_query(PSTRING() << "cannot create new merged block for shard " << shard_.to_str()
                                    << " because its left ancestor " << left->blk_.to_str()
                                    << " has no before_merge flag");
    }
    auto right = config_->get_shard_hash(shard_ + 1, false);
    if (right.is_null()) {
      return reject_query(
          PSTRING()
          << "cannot create new block for shard " << shard_.to_str()
          << " after a preceding merge because there is no right ancestor shard in existing masterchain configuration");
    }
    if (!shard_is_parent(shard_, right->shard())) {
      return reject_query(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                    << " after a preceding merge because its right ancestor appears to be "
                                    << right->blk_.to_str());
    }
    if (!right->before_merge_) {
      return reject_query(PSTRING() << "cannot create new merged block for shard " << shard_.to_str()
                                    << " because its right ancestor " << right->blk_.to_str()
                                    << " has no before_merge flag");
    }
    if (after_split_) {
      return reject_query(
          PSTRING() << "cannot create new block for shard " << shard_.to_str()
                    << " after a purported split because existing shard configuration suggests a merge");
    } else if (after_merge_) {
      if (!(check_prev_block_exact(left->blk_, prev_blocks[0]) &&
            check_prev_block_exact(right->blk_, prev_blocks[1]))) {
        return false;
      }
    } else {
      auto cseqno = std::max(left->seqno(), right->seqno());
      if (prev_blocks[0].seqno() <= cseqno) {
        return reject_query(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                      << " after previous block " << prev_blocks[0].to_str()
                                      << " because masterchain contains newer possible ancestors "
                                      << left->blk_.to_str() << " and " << right->blk_.to_str());
      }
      if (prev_blocks[0].seqno() >= cseqno + 8) {
        return reject_query(
            PSTRING() << "cannot create new block for shard " << shard_.to_str() << " after previous block "
                      << prev_blocks[0].to_str()
                      << " because this would lead to an unregistered chain of length > 8 (masterchain contains only "
                      << left->blk_.to_str() << " and " << right->blk_.to_str() << ")");
      }
    }
  } else if (shard_is_parent(left->shard(), shard_)) {
    // after split
    if (!left->before_split_) {
      return reject_query(PSTRING() << "cannot generate new split shardchain block for " << shard_.to_str()
                                    << " after previous block " << left->blk_.to_str() << " without before_split");
    }
    if (after_merge_) {
      return reject_query(
          PSTRING() << "cannot create new block for shard " << shard_.to_str()
                    << " after a purported merge because existing shard configuration suggests a split");
    } else if (after_split_) {
      if (!(check_prev_block_exact(left->blk_, prev_blocks[0]))) {
        return false;
      }
    } else {
      if (!(check_prev_block(left->blk_, prev_blocks[0]))) {
        return false;
      }
    }
  } else {
    return reject_query(PSTRING() << "masterchain configuration contains only block " << left->blk_.to_str()
                                  << " which belongs to a different shard from ours " << shard_.to_str());
  }
  if (before_split_ && !split_allowed) {
    return reject_query(PSTRING() << "new block " << id_.to_str()
                                  << " has before_split set, but this is forbidden by masterchain configuration");
  }
  return true;
}

/*
 * 
 *  METHODS CALLED FROM try_validate() stage 0
 * 
 */

bool ValidateQuery::compute_prev_state() {
  CHECK(prev_states.size() == 1u + after_merge_);
  prev_state_root_ = prev_states[0]->root_cell();
  CHECK(prev_state_root_.not_null());
  if (after_merge_) {
    Ref<vm::Cell> aux_root = prev_states[1]->root_cell();
    if (!block::gen::t_ShardState.cell_pack_split_state(prev_state_root_, prev_states[0]->root_cell(),
                                                        prev_states[1]->root_cell())) {
      return fatal_error(-667, "cannot construct mechanically merged previously state");
    }
  }
  Bits256 state_hash{prev_state_root_->get_hash().bits()};
  if (state_hash != prev_state_hash_) {
    return reject_query("previous state hash mismatch for block "s + id_.to_str() + " : block header declares " +
                        prev_state_hash_.to_hex() + " , actual " + state_hash.to_hex());
  }
  return true;
}

bool ValidateQuery::compute_next_state() {
  LOG(DEBUG) << "computing next state";
  auto res = vm::MerkleUpdate::validate(state_update_);
  if (res.is_error()) {
    return reject_query("state update is invalid: "s + res.move_as_error().to_string());
  }
  res = vm::MerkleUpdate::may_apply(prev_state_root_, state_update_);
  if (res.is_error()) {
    return reject_query("state update cannot be applied: "s + res.move_as_error().to_string());
  }
  state_root_ = vm::MerkleUpdate::apply(prev_state_root_, state_update_);
  if (state_root_.is_null()) {
    return reject_query("cannot apply Merkle update from block to compute new state");
  }
  Bits256 state_hash{state_root_->get_hash().bits()};
  if (state_hash != state_hash_) {
    return reject_query("next state hash mismatch for block "s + id_.to_str() + " : block header declares " +
                        state_hash_.to_hex() + " , actual " + state_hash.to_hex());
  }
  block::gen::ShardStateUnsplit::Record info;
  if (!tlb::unpack_cell(state_root_, info)) {
    return reject_query("next state does not have a valid header");
  }
  if (end_lt_ != info.gen_lt) {
    return reject_query(PSTRING() << "new state contains generation lt " << info.gen_lt << " distinct from end_lt "
                                  << end_lt_ << " in block header");
  }
  if (now_ != info.gen_utime) {
    return reject_query(PSTRING() << "new state contains generation time " << info.gen_utime
                                  << " distinct from the value " << now_ << " in block header");
  }
  if (before_split_ != info.before_split) {
    return reject_query("before_split value mismatch in new state and in block header");
  }
  block::ShardId id{info.shard_id};
  ton::BlockId hdr_id{ton::ShardIdFull(id), info.seq_no};
  if (hdr_id != id_.id) {
    return reject_query("header of new state claims it belongs to block "s + hdr_id.to_str() + " instead of " +
                        id_.id.to_str());
  }
  CHECK(info.custom->size_refs() == 0 || info.custom->size_refs() == 1);
  if (info.custom->size_refs() != static_cast<unsigned>(is_masterchain())) {
    return reject_query("McStateExtra in the new state of a non-masterchain block, or conversely");
  }
  if (is_masterchain()) {
    block::gen::McStateExtra::Record extra;
    if (!tlb::unpack_cell(info.custom->prefetch_ref(), extra)) {
      return reject_query("cannot unpack McStateExtra in the new state");
    }
    CHECK(shard_hashes_.not_null());
    if (!extra.shard_hashes->contents_equal(*shard_hashes_)) {
      return reject_query("ShardHashes in the new state and in the block differ");
    }
    if (is_key_block_) {
      CHECK(blk_config_params_.not_null());
      if (!extra.config->contents_equal(*blk_config_params_)) {
        return reject_query("ConfigParams in the header of the new key block and in the new state differ");
      }
    }
    auto r_config_info = block::ConfigInfo::extract_config(
        state_root_, block::ConfigInfo::needShardHashes | block::ConfigInfo::needLibraries |
                         block::ConfigInfo::needValidatorSet | block::ConfigInfo::needWorkchainInfo |
                         block::ConfigInfo::needStateExtraRoot | block::ConfigInfo::needAccountsRoot |
                         block::ConfigInfo::needSpecialSmc | block::ConfigInfo::needCapabilities);
    if (r_config_info.is_error()) {
      return reject_query("cannot extract configuration from new masterchain state "s + mc_blkid_.to_str() + " : " +
                          r_config_info.error().to_string());
    }
    new_config_ = r_config_info.move_as_ok();
    CHECK(new_config_);
    new_config_->set_block_id_ext(id_);
  }
  return true;
}

// similar to Collator::unpack_merge_last_state()
bool ValidateQuery::unpack_merge_prev_state() {
  LOG(DEBUG) << "unpack/merge previous states";
  CHECK(prev_states.size() == 2);
  // 2. extract the two previous states
  Ref<vm::Cell> root0, root1;
  if (!block::gen::t_ShardState.cell_unpack_split_state(prev_state_root_, root0, root1)) {
    return fatal_error(-667, "cannot unsplit a virtual split_state after a merge");
  }
  // 3. unpack previous states
  // 3.1. unpack left ancestor
  if (!unpack_one_prev_state(ps_, prev_blocks.at(0), std::move(root0))) {
    return fatal_error("cannot unpack the state of left ancestor "s + prev_blocks.at(0).to_str());
  }
  // 3.2. unpack right ancestor
  block::ShardState ss1;
  if (!unpack_one_prev_state(ss1, prev_blocks.at(1), std::move(root1))) {
    return fatal_error("cannot unpack the state of right ancestor "s + prev_blocks.at(1).to_str());
  }
  // 4. merge the two ancestors of the current state
  LOG(INFO) << "merging the two previous states";
  auto res = ps_.merge_with(ss1);
  if (res.is_error()) {
    return fatal_error(std::move(res)) || fatal_error("cannot merge the two previous states");
  }
  return true;
}

// similar to Collator::unpack_last_state()
bool ValidateQuery::unpack_prev_state() {
  LOG(DEBUG) << "unpacking previous state(s)";
  CHECK(prev_state_root_.not_null());
  if (after_merge_) {
    if (!unpack_merge_prev_state()) {
      return fatal_error("unable to unpack/merge previous states immediately after a merge");
    }
    return true;
  }
  CHECK(prev_states.size() == 1);
  // unpack previous state
  return unpack_one_prev_state(ps_, prev_blocks.at(0), prev_state_root_) && (!after_split_ || split_prev_state(ps_));
}

// similar to Collator::unpack_one_last_state()
bool ValidateQuery::unpack_one_prev_state(block::ShardState& ss, BlockIdExt blkid, Ref<vm::Cell> prev_state_root) {
  auto res = ss.unpack_state_ext(blkid, std::move(prev_state_root), global_id_, mc_seqno_, after_split_,
                                 after_split_ | after_merge_, [this](ton::BlockSeqno mc_seqno) {
                                   Ref<MasterchainStateQ> state;
                                   return request_aux_mc_state(mc_seqno, state);
                                 });
  if (res.is_error()) {
    return fatal_error(std::move(res));
  }
  if (ss.vert_seqno_ > vert_seqno_) {
    return reject_query(PSTRING() << "one of previous states " << ss.id_.to_str() << " has vertical seqno "
                                  << ss.vert_seqno_ << " larger than that of the new block " << vert_seqno_);
  }
  return true;
}

// similar to Collator::split_last_state()
bool ValidateQuery::split_prev_state(block::ShardState& ss) {
  LOG(INFO) << "Splitting previous state " << ss.id_.to_str() << " to subshard " << shard_.to_str();
  CHECK(after_split_);
  auto sib_shard = ton::shard_sibling(shard_);
  auto res1 = ss.compute_split_out_msg_queue(sib_shard);
  if (res1.is_error()) {
    return fatal_error(res1.move_as_error());
  }
  sibling_out_msg_queue_ = res1.move_as_ok();
  auto res2 = ss.compute_split_processed_upto(sib_shard);
  if (res2.is_error()) {
    return fatal_error(res2.move_as_error());
  }
  sibling_processed_upto_ = res2.move_as_ok();
  auto res3 = ss.split(shard_);
  if (res3.is_error()) {
    return fatal_error(std::move(res3));
  }
  return true;
}

bool ValidateQuery::unpack_next_state() {
  LOG(DEBUG) << "unpacking new state";
  CHECK(state_root_.not_null());
  auto res = ns_.unpack_state_ext(id_, state_root_, global_id_, mc_seqno_, before_split_, false,
                                  [](ton::BlockSeqno mc_seqno) { return true; });
  if (res.is_error()) {
    return reject_query("cannot unpack new state", std::move(res));
  }
  if (ns_.utime_ != now_) {
    return reject_query(PSTRING() << "new state of " << id_.to_str() << " claims to have been generated at unixtime "
                                  << ns_.utime_ << ", but the block header contains " << now_);
  }
  if (ns_.lt_ != end_lt_) {
    return reject_query(PSTRING() << "new state of " << id_.to_str()
                                  << " claims to have been generated at logical time " << ns_.lt_
                                  << ", but the block header contains end lt " << end_lt_);
  }
  if (!is_masterchain() && ns_.mc_blk_ref_ != mc_blkid_) {
    return reject_query("new state refers to masterchain block "s + ns_.mc_blk_ref_.to_str() + " different from " +
                        mc_blkid_.to_str() + " indicated in block header");
  }
  if (ns_.vert_seqno_ != vert_seqno_) {
    return reject_query(PSTRING() << "new state has vertical seqno " << ns_.vert_seqno_ << " different from "
                                  << vert_seqno_ << " declared in the new block header");
  }
  // ...
  return true;
}

// almost the same as in Collator
bool ValidateQuery::request_neighbor_queues() {
  CHECK(new_shard_conf_);
  auto neighbor_list = new_shard_conf_->get_neighbor_shard_hash_ids(shard_);
  LOG(DEBUG) << "got a preliminary list of " << neighbor_list.size() << " neighbors for " << shard_.to_str();
  for (ton::BlockId blk_id : neighbor_list) {
    auto shard_ptr = new_shard_conf_->get_shard_hash(ton::ShardIdFull(blk_id));
    if (shard_ptr.is_null()) {
      return reject_query("cannot obtain shard hash for neighbor "s + blk_id.to_str());
    }
    if (shard_ptr->blk_.id != blk_id) {
      return reject_query("invalid block id "s + shard_ptr->blk_.to_str() + " returned in information for neighbor " +
                          blk_id.to_str());
    }
    neighbors_.emplace_back(*shard_ptr);
  }
  int i = 0;
  for (block::McShardDescr& descr : neighbors_) {
    LOG(DEBUG) << "requesting outbound queue of neighbor #" << i << " : " << descr.blk_.to_str();
    ++pending;
    send_closure_later(manager, &ValidatorManager::wait_block_message_queue_short, descr.blk_, priority(), timeout,
                       [self = get_self(), i](td::Result<Ref<MessageQueue>> res) {
                         td::actor::send_closure(std::move(self), &ValidateQuery::got_neighbor_out_queue, i,
                                                 std::move(res));
                       });
    ++i;
  }
  return true;
}

// almost the same as in Collator
void ValidateQuery::got_neighbor_out_queue(int i, td::Result<Ref<MessageQueue>> res) {
  LOG(DEBUG) << "obtained outbound queue for neighbor #" << i;
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  Ref<MessageQueue> outq_descr = res.move_as_ok();
  block::McShardDescr& descr = neighbors_.at(i);
  if (outq_descr->get_block_id() != descr.blk_) {
    LOG(DEBUG) << "outq_descr->id = " << outq_descr->get_block_id().to_str() << " ; descr.id = " << descr.blk_.to_str();
    fatal_error(
        -667, "invalid outbound queue information returned for "s + descr.shard().to_str() + " : id or hash mismatch");
    return;
  }
  if (outq_descr->root_cell().is_null()) {
    fatal_error("no OutMsgQueueInfo in queue info in a neighbor state");
    return;
  }
  block::gen::OutMsgQueueInfo::Record qinfo;
  if (!tlb::unpack_cell(outq_descr->root_cell(), qinfo)) {
    fatal_error("cannot unpack neighbor output queue info");
    return;
  }
  descr.set_queue_root(qinfo.out_queue->prefetch_ref(0));
  // TODO: comment the next two lines in the future when the output queues become huge
  // (do this carefully)
  if (debug_checks_) {
    CHECK(block::gen::t_OutMsgQueueInfo.validate_ref(1000000, outq_descr->root_cell()));
    CHECK(block::tlb::t_OutMsgQueueInfo.validate_ref(1000000, outq_descr->root_cell()));
  }
  // unpack ProcessedUpto
  LOG(DEBUG) << "unpacking ProcessedUpto of neighbor " << descr.blk_.to_str();
  if (verbosity >= 2) {
    block::gen::t_ProcessedInfo.print(std::cerr, qinfo.proc_info);
    qinfo.proc_info->print_rec(std::cerr);
  }
  descr.processed_upto = block::MsgProcessedUptoCollection::unpack(descr.shard(), qinfo.proc_info);
  if (!descr.processed_upto) {
    fatal_error("cannot unpack ProcessedUpto in neighbor output queue info for neighbor "s + descr.blk_.to_str());
    return;
  }
  outq_descr.clear();
  do {
    // require masterchain blocks referred to in ProcessedUpto
    // TODO: perform this only if there are messages for this shard in our output queue
    // .. (have to check the above condition and perform a `break` here) ..
    // ..
    for (const auto& entry : descr.processed_upto->list) {
      Ref<MasterchainStateQ> state;
      if (!request_aux_mc_state(entry.mc_seqno, state)) {
        return;
      }
    }
  } while (false);
  if (!pending) {
    LOG(INFO) << "all neighbor output queues fetched";
    try_validate();
  }
}

// almost the same as in Collator
bool ValidateQuery::register_mc_state(Ref<MasterchainStateQ> other_mc_state) {
  if (other_mc_state.is_null() || mc_state_.is_null()) {
    return false;
  }
  if (!mc_state_->check_old_mc_block_id(other_mc_state->get_block_id())) {
    return fatal_error(
        "attempting to register masterchain state for block "s + other_mc_state->get_block_id().to_str() +
        " which is not an ancestor of most recent masterchain block " + mc_state_->get_block_id().to_str());
  }
  auto seqno = other_mc_state->get_seqno();
  auto res = aux_mc_states_.insert(std::make_pair(seqno, other_mc_state));
  if (res.second) {
    return true;  // inserted
  }
  auto& found = res.first->second;
  if (found.is_null()) {
    found = std::move(other_mc_state);
    return true;
  } else if (found->get_block_id() != other_mc_state->get_block_id()) {
    return fatal_error("got two masterchain states of same height corresponding to different blocks "s +
                       found->get_block_id().to_str() + " and " + other_mc_state->get_block_id().to_str());
  }
  return true;
}

// almost the same as in Collator
bool ValidateQuery::request_aux_mc_state(BlockSeqno seqno, Ref<MasterchainStateQ>& state) {
  if (mc_state_.is_null()) {
    return fatal_error(PSTRING() << "cannot find masterchain block with seqno " << seqno
                                 << " to load corresponding state because no masterchain state is known yet");
  }
  if (seqno > mc_state_->get_seqno()) {
    state = mc_state_;
    return true;
  }
  auto res = aux_mc_states_.insert(std::make_pair(seqno, Ref<MasterchainStateQ>{}));
  if (!res.second) {
    state = res.first->second;
    return true;
  }
  BlockIdExt blkid;
  if (!mc_state_->get_old_mc_block_id(seqno, blkid)) {
    return fatal_error(PSTRING() << "cannot find masterchain block with seqno " << seqno
                                 << " to load corresponding state as required");
  }
  CHECK(blkid.is_valid_ext() && blkid.is_masterchain());
  LOG(DEBUG) << "sending auxiliary wait_block_state() query for " << blkid.to_str() << " to Manager";
  ++pending;
  td::actor::send_closure_later(manager, &ValidatorManager::wait_block_state_short, blkid, priority(), timeout,
                                [self = get_self(), blkid](td::Result<Ref<ShardState>> res) {
                                  LOG(DEBUG) << "got answer to wait_block_state query for " << blkid.to_str();
                                  td::actor::send_closure_later(std::move(self),
                                                                &ValidateQuery::after_get_aux_shard_state, blkid,
                                                                std::move(res));
                                });
  state.clear();
  return true;
}

// almost the same as in Collator
Ref<MasterchainStateQ> ValidateQuery::get_aux_mc_state(BlockSeqno seqno) const {
  auto it = aux_mc_states_.find(seqno);
  if (it != aux_mc_states_.end()) {
    return it->second;
  } else {
    return {};
  }
}

// almost the same as in Collator
void ValidateQuery::after_get_aux_shard_state(ton::BlockIdExt blkid, td::Result<Ref<ShardState>> res) {
  LOG(DEBUG) << "in ValidateQuery::after_get_aux_shard_state(" << blkid.to_str() << ")";
  --pending;
  if (res.is_error()) {
    fatal_error("cannot load auxiliary masterchain state for "s + blkid.to_str() + " : " +
                res.move_as_error().to_string());
    return;
  }
  auto state = Ref<MasterchainStateQ>(res.move_as_ok());
  if (state.is_null()) {
    fatal_error("auxiliary masterchain state for "s + blkid.to_str() + " turned out to be null");
    return;
  }
  if (state->get_block_id() != blkid) {
    fatal_error("auxiliary masterchain state for "s + blkid.to_str() +
                " turned out to correspond to a different block " + state->get_block_id().to_str());
    return;
  }
  if (!register_mc_state(std::move(state))) {
    fatal_error("cannot register auxiliary masterchain state for "s + blkid.to_str());
    return;
  }
  try_validate();
}

// similar to Collator::update_one_shard()
bool ValidateQuery::check_one_shard(const block::McShardHash& info, const block::McShardHash* sibling,
                                    const block::WorkchainInfo* wc_info, const block::CatchainValidatorsConfig& ccvc) {
  auto shard = info.shard();
  LOG(DEBUG) << "checking shard " << shard.to_str() << " in new shard configuration";
  if (info.next_validator_shard_ != shard.shard) {
    return reject_query("new shard configuration for shard "s + shard.to_str() +
                        " contains different next_validator_shard_ " +
                        ton::ShardIdFull{shard.workchain, info.next_validator_shard_}.to_str());
  }
  auto old = old_shard_conf_->get_shard_hash(shard - 1, false);
  Ref<block::McShardHash> prev;
  CatchainSeqno cc_seqno;
  bool old_before_merge = false, fsm_inherited = false, workchain_created = false;
  if (old.is_null()) {
    if (shard.is_split()) {
      return reject_query("new shard configuration contains split shard "s + shard.to_str() + " unknown before");
    }
    if (!wc_info) {
      return reject_query("new shard configuration contains newly-created shard "s + shard.to_str() +
                          " for an unknown workchain");
    }
    if (!wc_info->active) {
      return reject_query("new shard configuration contains newly-created shard "s + shard.to_str() +
                          " for an inactive workchain");
    }
    if (info.seqno()) {
      return reject_query(PSTRING() << "newly-created shard " << shard.to_str() << " starts with non-zero seqno "
                                    << info.seqno());
    }
    if (info.blk_.root_hash != wc_info->zerostate_root_hash || info.blk_.file_hash != wc_info->zerostate_file_hash) {
      return reject_query("new shard configuration contains newly-created shard "s + shard.to_str() +
                          " with incorrect zerostate hashes");
    }
    if (info.end_lt_ >= start_lt_) {
      return reject_query(PSTRING() << "newly-created shard " << shard.to_str() << " has incorrect logical time "
                                    << info.end_lt_ << " for a new block with start_lt=" << start_lt_);
    }
    if (info.gen_utime_ > now_) {
      return reject_query(PSTRING() << "newly-created shard " << shard.to_str() << " has incorrect creation time "
                                    << info.gen_utime_ << " for a new block created only at " << now_);
    }
    if (info.before_split_ || info.before_merge_ || info.want_split_ || info.want_merge_) {
      return reject_query("newly-created shard "s + shard.to_str() + " has merge/split flags (incorrectly) set");
    }
    if (info.min_ref_mc_seqno_ != ~0U) {
      return reject_query("newly-created shard "s + shard.to_str() + " has finite min_ref_mc_seqno");
    }
    if (info.reg_mc_seqno_ != id_.seqno()) {
      return reject_query(PSTRING() << "newly-created shard " << shard.to_str() << " has registration mc seqno "
                                    << info.reg_mc_seqno_ << " different from seqno of current block " << id_.seqno());
    }
    if (!info.fees_collected_.is_zero()) {
      return reject_query("newly-created shard "s + shard.to_str() + " has non-zero fees_collected");
    }
    cc_seqno = 0;
  } else if (old->top_block_id() == info.top_block_id()) {
    // shard unchanged ?
    LOG(DEBUG) << "shard " << shard.to_str() << " unchanged";
    if (!old->basic_info_equal(info, true, true)) {
      return reject_query("shard information for block "s + info.top_block_id().to_str() +
                          " listed in new shard configuration differs from that present in the old shard configuration "
                          "for the same block");
    }
    cc_seqno = old->next_catchain_seqno_;
    prev = old;
    // ...
  } else {
    // shard changed, extract and check TopShardBlockDescr from collated data
    LOG(DEBUG) << "shard " << shard.to_str() << " changed from " << old->top_block_id().to_str() << " to "
               << info.top_block_id().to_str();
    if (info.reg_mc_seqno_ != id_.seqno()) {
      return reject_query(PSTRING() << "shard information for block "s << info.top_block_id().to_str()
                                    << " has been updated in the new shard configuration, but it has reg_mc_seqno="
                                    << info.reg_mc_seqno_ << " different from that of the current block "
                                    << id_.seqno());
    }
    td::BitArray<96> key;
    key.bits().store_int(shard.workchain, 32);
    (key.bits() + 32).store_uint(shard.shard, 64);
    try {
      auto tbd_ref = top_shard_descr_dict_ ? top_shard_descr_dict_->lookup_ref(key) : Ref<vm::Cell>{};
      if (tbd_ref.is_null()) {
        return reject_query("no ShardTopBlockDescr for newly-registered shard "s + info.top_block_id().to_str() +
                            " is present in collated data");
      }
      auto res = ShardTopBlockDescrQ::fetch(std::move(tbd_ref), is_fake_);
      if (res.is_error()) {
        return reject_query("cannot unpack ShardTopBlockDescr for "s + shard.to_str() +
                            " contained in collated data : "s + res.move_as_error().to_string());
      }
      auto sh_bd = res.move_as_ok();
      CHECK(sh_bd.not_null());
      if (sh_bd->block_id() != info.top_block_id()) {
        return reject_query("ShardTopBlockDescr for shard "s + shard.to_str() + " is for new block " +
                            sh_bd->block_id().to_str() + " instead of " + info.top_block_id().to_str() +
                            " declared in new shardchain configuration");
      }
      // following checks are similar to those of Collator::import_new_shard_top_blocks()
      int res_flags = 0;
      auto chk_res = sh_bd->prevalidate(mc_blkid_, mc_state_,
                                        ShardTopBlockDescrQ::fail_new | ShardTopBlockDescrQ::fail_too_new, res_flags);
      if (chk_res.is_error()) {
        return reject_query(PSTRING() << "ShardTopBlockDescr for " << sh_bd->block_id().to_str()
                                      << " is invalid: res_flags=" << res_flags << " "
                                      << chk_res.move_as_error().to_string());
      }
      int chain_len = chk_res.move_as_ok();
      if (chain_len <= 0 || chain_len > 8) {
        return reject_query(PSTRING() << "ShardTopBlockDescr for " << sh_bd->block_id().to_str()
                                      << " is invalid: its chain length is " << chain_len << " (not in range 1..8)");
      }
      if (sh_bd->generated_at() > now_) {
        return reject_query(PSTRING() << "ShardTopBlockDescr for " << sh_bd->block_id().to_str()
                                      << " is invalid: it claims to be generated at " << sh_bd->generated_at()
                                      << " while it is still " << now_);
      }
      Ref<block::McShardHash> descr = sh_bd->get_top_descr(chain_len);
      CHECK(descr.not_null());
      CHECK(descr->top_block_id() == sh_bd->block_id());
      auto start_blks = sh_bd->get_prev_at(chain_len);
      auto res2 = old_shard_conf_->may_update_shard_block_info(descr, start_blks, start_lt_);
      if (res2.is_error()) {
        return reject_query("new top shard block "s + sh_bd->block_id().to_str() +
                            " cannot be added to shard configuration: " + res2.move_as_error().to_string());
      }
      if (!descr->basic_info_equal(info, true, false)) {
        return reject_query(
            "shard information for block "s + info.top_block_id().to_str() +
            " listed in new shard configuration differs from that present in ShardTopBlockDescr (and block header)");
      }
      // all fields in info and descr are equal
      // except fsm*, before_merge_, next_catchain_seqno_
      // of these, only next_catchain_seqno_ makes sense in descr
      cc_seqno = descr->next_catchain_seqno_;
      // check that there is a corresponding record in ShardFees
      auto import = fees_import_dict_->lookup(key);
      if (import.is_null()) {
        if (!descr->fees_collected_.is_zero()) {
          return reject_query("new shard top block "s + sh_bd->block_id().to_str() +
                              " has been registered and has non-zero collected fees " +
                              descr->fees_collected_.to_str() + ", but there is no corresponding entry in ShardFees");
        }
      } else {
        block::gen::ShardFeeCreated::Record fc;
        block::CurrencyCollection import_fees, funds_created;
        if (!(tlb::csr_unpack(import, fc) && import_fees.validate_unpack(std::move(fc.fees)) &&
              funds_created.validate_unpack(std::move(fc.create)))) {
          return reject_query("ShardFees record with key "s + key.to_hex() +
                              " does not contain a valid CurrencyCollection");
        }
        if (import_fees != descr->fees_collected_) {
          return reject_query("ShardFees record for new shard top block "s + sh_bd->block_id().to_str() +
                              " declares fees_collected=" + import_fees.to_str() +
                              ", but the shard configuration contains a different value " +
                              descr->fees_collected_.to_str());
        }
        if (funds_created != descr->funds_created_) {
          return reject_query("ShardFees record for new shard top block "s + sh_bd->block_id().to_str() +
                              " declares funds_created=" + funds_created.to_str() +
                              ", but the shard configuration contains a different value " +
                              descr->funds_created_.to_str());
        }
      }
      // register shard block creators
      register_shard_block_creators(sh_bd->get_creator_list(chain_len));
      // ...
    } catch (vm::VmError& err) {
      return reject_query("incorrect ShardTopBlockDescr for "s + shard.to_str() +
                          " in collated data : " + err.get_msg());
    }
    if (ton::shard_is_parent(old->shard(), shard)) {
      // shard has been split
      LOG(INFO) << "detected shard split " << old->shard().to_str() << " -> " << shard.to_str();
      // ...
    } else if (ton::shard_is_parent(shard, old->shard())) {
      // shard has been merged
      auto old2 = old_shard_conf_->get_shard_hash(shard + 1, false);
      CHECK(old2.not_null());
      if (!ton::shard_is_sibling(old->shard(), old2->shard())) {
        return reject_query("shard "s + shard.to_str() + " has been impossibly merged from more than two shards " +
                            old->shard().to_str() + ", " + old2->shard().to_str() + " and others");
      }
      LOG(INFO) << "detected shard merge " << old->shard().to_str() << " + " << old2->shard().to_str() << " -> "
                << shard.to_str();
      // ...
    } else if (shard == old->shard()) {
      // shard updated without split/merge
      prev = old;
      // ...
    } else {
      return reject_query("new configuration contains shard "s + shard.to_str() +
                          " that could not be obtained from previously existing shard " + old->shard().to_str());
      // ...
    }
  }
  if (prev.not_null()) {
    // shard was not created, split or merged; it is a successor of `prev`
    old_before_merge = prev->before_merge_;
    if (!prev->is_fsm_none() && !prev->fsm_equal(info) && now_ < prev->fsm_utime_end() && !info.before_split_) {
      return reject_query("future split/merge information for shard "s + shard.to_str() +
                          " has been arbitrarily changed without a good reason");
    }
    fsm_inherited = !prev->is_fsm_none() && prev->fsm_equal(info);
    if (fsm_inherited && (now_ > prev->fsm_utime_end() || info.before_split_)) {
      return reject_query(
          PSTRING() << "future split/merge information for shard " << shard.to_str()
                    << "has been carried on to the new shard configuration, but it is either expired (expire time "
                    << prev->fsm_utime_end() << ", now " << now_ << "), or before_split bit has been set ("
                    << (int)info.before_split_ << ")");
    }
  } else {
    // shard was created, split or merged
    if (info.before_split_) {
      return reject_query("a newly-created, split or merged shard "s + shard.to_str() +
                          " cannot have before_split set immediately after");
    }
  }
  unsigned depth = ton::shard_prefix_length(shard);
  bool split_cond = ((info.want_split_ || depth < wc_info->min_split) && depth < wc_info->max_split && depth < 60);
  bool merge_cond = !info.before_split_ && depth > wc_info->min_split &&
                    (info.want_merge_ || depth > wc_info->max_split) && sibling && !sibling->before_split_ &&
                    (sibling->want_merge_ || depth > wc_info->max_split);
  if (!fsm_inherited && !info.is_fsm_none()) {
    if (info.fsm_utime() < now_ || info.fsm_utime_end() <= info.fsm_utime() ||
        info.fsm_utime_end() < info.fsm_utime() + ton::min_split_merge_interval ||
        info.fsm_utime_end() > now_ + ton::max_split_merge_delay) {
      return reject_query(PSTRING() << "incorrect future split/merge interval " << info.fsm_utime() << " .. "
                                    << info.fsm_utime_end() << " set for shard " << shard.to_str()
                                    << " in new shard configuration (it is " << now_ << " now)");
    }
    if (info.is_fsm_split() && !split_cond) {
      return reject_query("announcing future split for shard "s + shard.to_str() +
                          " in new shard configuration, but split conditions are not met");
    }
    if (info.is_fsm_merge() && !merge_cond) {
      return reject_query("announcing future merge for shard "s + shard.to_str() +
                          " in new shard configuration, but merge conditions are not met");
    }
  }
  if (info.is_fsm_merge() && (!sibling || sibling->before_split_)) {
    return reject_query(
        "future merge for shard "s + shard.to_str() +
        " is still set in the new shard configuration, but its sibling is absent or has before_split set");
  }
  if (info.before_merge_) {
    if (!sibling || !sibling->before_merge_) {
      return reject_query("before_merge set for shard "s + shard.to_str() +
                          " in shard configuration, but not for its sibling");
    }
    if (!info.is_fsm_merge()) {
      return reject_query(
          "before_merge set for shard "s + shard.to_str() +
          " in shard configuration, but it has not been announced in future split/merge for this shard");
    }
    if (!merge_cond) {
      return reject_query("before_merge set for shard "s + shard.to_str() +
                          " in shard configuration, but merge conditions are not met");
    }
  }
  bool cc_updated = (info.next_catchain_seqno_ != cc_seqno);
  if (info.next_catchain_seqno_ != cc_seqno + (unsigned)cc_updated) {
    return reject_query(PSTRING() << "new shard configuration for shard " << shard.to_str()
                                  << " changed catchain seqno from " << cc_seqno << " to " << info.next_catchain_seqno_
                                  << " (only updates by at most one are allowed)");
  }
  if (!cc_updated && update_shard_cc_) {
    return reject_query(PSTRING() << "new shard configuration for shard " << shard.to_str()
                                  << " has unchanged catchain seqno " << cc_seqno
                                  << ", but it must have been updated for all shards");
  }
  bool bm_cleared = !info.before_merge_ && old_before_merge;
  if (!cc_updated && bm_cleared && !workchain_created) {
    return reject_query(PSTRING() << "new shard configuration for shard " << shard.to_str()
                                  << " has unchanged catchain seqno " << cc_seqno
                                  << " while the before_merge bit has been cleared");
  }
  if (cc_updated && !(update_shard_cc_ || bm_cleared)) {
    return reject_query(PSTRING() << "new shard configuration for shard " << shard.to_str()
                                  << " has increased catchain seqno " << cc_seqno << " without a good reason");
  }
  min_shard_ref_mc_seqno_ = std::min(min_shard_ref_mc_seqno_, info.min_ref_mc_seqno_);
  max_shard_utime_ = std::max(max_shard_utime_, info.gen_utime_);
  max_shard_lt_ = std::max(max_shard_lt_, info.end_lt_);
  return true;
}

// checks old_shard_conf_ -> new_shard_conf_ transition using top_shard_descr_dict_ from collated data
// similar to Collator::update_shard_config()
bool ValidateQuery::check_shard_layout() {
  prev_now_ = config_->utime;
  if (prev_now_ > now_) {
    return reject_query(PSTRING() << "creation time is not monotonic: " << now_ << " after " << prev_now_);
  }
  auto ccvc = new_config_->get_catchain_validators_config();
  const auto& wc_set = new_config_->get_workchain_list();
  update_shard_cc_ = is_key_block_ || (now_ / ccvc.shard_cc_lifetime > prev_now_ / ccvc.shard_cc_lifetime);
  if (update_shard_cc_) {
    LOG(INFO) << "catchain_seqno of all shards must be updated";
  }

  WorkchainId wc_id{ton::workchainInvalid};
  Ref<block::WorkchainInfo> wc_info;

  if (!new_shard_conf_->process_sibling_shard_hashes([self = this, &wc_set, &wc_id, &wc_info, &ccvc](
                                                         block::McShardHash& cur, const block::McShardHash* sibling) {
        if (!cur.is_valid()) {
          return -2;
        }
        if (wc_id != cur.workchain()) {
          wc_id = cur.workchain();
          if (wc_id == ton::workchainInvalid || wc_id == ton::masterchainId) {
            self->reject_query(PSTRING() << "new shard configuration contains shards of invalid workchain " << wc_id);
            return -2;
          }
          auto it = wc_set.find(wc_id);
          if (it == wc_set.end()) {
            wc_info.clear();
          } else {
            wc_info = it->second;
          }
        }
        return self->check_one_shard(cur, sibling, wc_info.get(), ccvc) ? 0 : -1;
      })) {
    return reject_query("new shard configuration is invalid");
  }
  auto wc_list = old_shard_conf_->get_workchains();
  for (auto x : wc_list) {
    if (!new_shard_conf_->has_workchain(x)) {
      return reject_query(PSTRING() << "shards of workchain " << x
                                    << " existed in previous shardchain configuration, but are absent from new");
    }
  }
  for (const auto& pair : wc_set) {
    if (pair.second->active && !new_shard_conf_->has_workchain(pair.first)) {
      return reject_query(PSTRING() << "workchain " << pair.first
                                    << " is active, but is absent from new shard configuration");
    }
  }
  return check_mc_validator_info(is_key_block_ || (now_ / ccvc.mc_cc_lifetime > prev_now_ / ccvc.mc_cc_lifetime));
}

// similar to Collator::register_shard_block_creators
bool ValidateQuery::register_shard_block_creators(std::vector<td::Bits256> creator_list) {
  for (const auto& x : creator_list) {
    LOG(DEBUG) << "registering block creator " << x.to_hex();
    if (!x.is_zero()) {
      auto res = block_create_count_.emplace(x, 1);
      if (!res.second) {
        (res.first->second)++;
      }
      block_create_total_++;
    }
  }
  return true;
}

// similar to Collator::check_cur_validator_set()
bool ValidateQuery::check_cur_validator_set() {
  CatchainSeqno cc_seqno = 0;
  auto nodes = config_->compute_validator_set_cc(shard_, now_, &cc_seqno);
  if (nodes.empty()) {
    return reject_query("cannot compute validator set for shard "s + shard_.to_str() + " from old masterchain state");
  }
  std::vector<ValidatorDescr> export_nodes;
  if (validator_set_.not_null()) {
    if (validator_set_->get_catchain_seqno() != cc_seqno) {
      return reject_query(PSTRING() << "current validator set catchain seqno mismatch: this validator set has cc_seqno="
                                    << validator_set_->get_catchain_seqno() << ", only validator set with cc_seqno="
                                    << cc_seqno << " is entitled to create block " << id_.to_str());
    }
    export_nodes = validator_set_->export_vector();
  }
  if (export_nodes != nodes /* && !is_fake_ */) {
    return reject_query("current validator set mismatch: this validator set is not entitled to create block "s +
                        id_.to_str());
  }
  return true;
}

// parallel to 4. of Collator::create_mc_state_extra()
// checks validator_info in mc_state_extra
bool ValidateQuery::check_mc_validator_info(bool update_mc_cc) {
  block::gen::McStateExtra::Record old_state_extra;
  block::gen::ValidatorInfo::Record old_val_info;
  if (!(tlb::unpack_cell(config_->get_state_extra_root(), old_state_extra) &&
        tlb::csr_unpack(old_state_extra.r1.validator_info, old_val_info))) {
    return soft_reject_query("cannot unpack ValidatorInfo from McStateExtra of old masterchain state");
  }
  block::gen::McStateExtra::Record state_extra;
  block::gen::ValidatorInfo::Record val_info;
  if (!(tlb::unpack_cell(new_config_->get_state_extra_root(), state_extra) &&
        tlb::csr_unpack(state_extra.r1.validator_info, val_info))) {
    return reject_query("cannot unpack ValidatorInfo from McStateExtra of new masterchain state");
  }
  bool cc_updated = (val_info.catchain_seqno != old_val_info.catchain_seqno);
  if (val_info.catchain_seqno != old_val_info.catchain_seqno + (unsigned)cc_updated) {
    return reject_query(PSTRING() << "new masterchain state increased masterchain catchain seqno from "
                                  << old_val_info.catchain_seqno << " to " << val_info.catchain_seqno
                                  << " (only updates by at most one are allowed)");
  }
  if (cc_updated != update_mc_cc) {
    return reject_query(cc_updated ? "masterchain catchain seqno increased without any reason"
                                   : "masterchain catchain seqno unchanged while it had to");
  }
  auto nodes = new_config_->compute_validator_set(shard_, now_, val_info.catchain_seqno);
  if (nodes.empty()) {
    return reject_query("cannot compute next masterchain validator set from new masterchain state");
  }

  auto vlist_hash = block::compute_validator_set_hash(/* val_info.catchain_seqno */ 0, shard_, std::move(nodes));
  if (val_info.validator_list_hash_short != vlist_hash) {
    return reject_query(PSTRING() << "new masterchain validator list hash incorrect hash: expected " << vlist_hash
                                  << ", found val_info.validator_list_hash_short");
  }
  LOG(INFO) << "masterchain validator set hash changed from " << old_val_info.validator_list_hash_short << " to "
            << vlist_hash;
  if (val_info.nx_cc_updated != (cc_updated & update_shard_cc_)) {
    return reject_query(PSTRING() << "val_info.nx_cc_updated has incorrect value " << val_info.nx_cc_updated);
  }
  return true;
}

bool ValidateQuery::check_utime_lt() {
  if (start_lt_ <= ps_.lt_) {
    return reject_query(PSTRING() << "block has start_lt " << start_lt_ << " less than or equal to lt " << ps_.lt_
                                  << " of the previous state");
  }
  if (now_ <= ps_.utime_) {
    return reject_query(PSTRING() << "block has creation time " << now_
                                  << " less than or equal to that of the previous state (" << ps_.utime_ << ")");
  }
  if (now_ <= config_->utime) {
    return reject_query(PSTRING() << "block has creation time " << now_
                                  << " less than or equal to that of the reference masterchain state ("
                                  << config_->utime << ")");
  }
  /*
  if (now_ > (unsigned)std::time(nullptr) + 15) {
    return reject_query(PSTRING() << "block has creation time " << now_ << " too much in the future (it is only " << (unsigned)std::time(nullptr) << " now)");
  }
  */
  if (start_lt_ <= config_->lt) {
    return reject_query(PSTRING() << "block has start_lt " << start_lt_ << " less than or equal to lt " << config_->lt
                                  << " of the reference masterchain state");
  }
  auto lt_bound = std::max(ps_.lt_, std::max(config_->lt, max_shard_lt_));
  if (start_lt_ > lt_bound + config_->get_lt_align() * 4) {
    return reject_query(PSTRING() << "block has start_lt " << start_lt_
                                  << " which is too large without a good reason (lower bound is " << lt_bound + 1
                                  << ")");
  }
  if (is_masterchain() && start_lt_ - ps_.lt_ > config_->get_max_lt_growth()) {
    return reject_query(PSTRING() << "block increases logical time from previous state by " << start_lt_ - ps_.lt_
                                  << " which exceeds the limit (" << config_->get_max_lt_growth() << ")");
  }
  if (end_lt_ - start_lt_ > block_limits_->lt_delta.hard()) {
    return reject_query(PSTRING() << "block increased logical time by " << end_lt_ - start_lt_
                                  << " which is larger than the hard limit " << block_limits_->lt_delta.hard());
  }
  return true;
}

/*
 * 
 *  METHODS CALLED FROM try_validate() stage 1
 * 
 */

// almost the same as in Collator
// (but it can take into account the new state of the masterchain)
bool ValidateQuery::fix_one_processed_upto(block::MsgProcessedUpto& proc, ton::ShardIdFull owner, bool allow_cur) {
  if (proc.compute_shard_end_lt) {
    return true;
  }
  auto seqno = std::min(proc.mc_seqno, mc_seqno_);
  if (allow_cur && is_masterchain() && proc.mc_seqno == id_.seqno() && proc.mc_seqno == mc_seqno_ + 1) {
    seqno = id_.seqno();
    CHECK(new_config_);
    proc.compute_shard_end_lt = new_config_->get_compute_shard_end_lt_func();
  } else {
    auto state = get_aux_mc_state(seqno);
    if (state.is_null()) {
      return fatal_error(
          -666, PSTRING() << "cannot obtain masterchain state with seqno " << seqno << " (originally required "
                          << proc.mc_seqno << ") in a MsgProcessedUpto record for "
                          << ton::ShardIdFull{owner.workchain, proc.shard}.to_str() << " owned by " << owner.to_str());
    }
    proc.compute_shard_end_lt = state->get_config()->get_compute_shard_end_lt_func();
  }
  return (bool)proc.compute_shard_end_lt;
}

// almost the same as in Collator
bool ValidateQuery::fix_processed_upto(block::MsgProcessedUptoCollection& upto, bool allow_cur) {
  for (auto& entry : upto.list) {
    if (!fix_one_processed_upto(entry, upto.owner, allow_cur)) {
      return false;
    }
  }
  return true;
}

bool ValidateQuery::fix_all_processed_upto() {
  CHECK(ps_.processed_upto_);
  if (!fix_processed_upto(*ps_.processed_upto_)) {
    return fatal_error("Cannot adjust old ProcessedUpto of our shard state");
  }
  if (sibling_processed_upto_ && !fix_processed_upto(*sibling_processed_upto_)) {
    return fatal_error("Cannot adjust old ProcessedUpto of the shard state of our virtual sibling");
  }
  if (!fix_processed_upto(*ns_.processed_upto_, true)) {
    return fatal_error("Cannot adjust new ProcessedUpto of our shard state");
  }
  for (auto& descr : neighbors_) {
    CHECK(descr.processed_upto);
    if (!fix_processed_upto(*descr.processed_upto)) {
      return fatal_error("Cannot adjust ProcessedUpto of neighbor "s + descr.blk_.to_str());
    }
  }
  return true;
}

// almost the same as in Collator
bool ValidateQuery::add_trivial_neighbor_after_merge() {
  LOG(DEBUG) << "in add_trivial_neighbor_after_merge()";
  CHECK(prev_blocks.size() == 2);
  int found = 0;
  std::size_t n = neighbors_.size();
  for (std::size_t i = 0; i < n; i++) {
    auto& nb = neighbors_.at(i);
    if (ton::shard_intersects(nb.shard(), shard_)) {
      ++found;
      LOG(DEBUG) << "neighbor #" << i << " : " << nb.blk_.to_str() << " intersects our shard " << shard_.to_str();
      if (!ton::shard_is_parent(shard_, nb.shard()) || found > 2) {
        return fatal_error("impossible shard configuration in add_trivial_neighbor_after_merge()");
      }
      auto prev_shard = prev_blocks.at(found - 1).shard_full();
      if (nb.shard() != prev_shard) {
        return fatal_error("neighbor shard "s + nb.shard().to_str() + " does not match that of our ancestor " +
                           prev_shard.to_str());
      }
      if (found == 1) {
        nb.set_queue_root(ps_.out_msg_queue_->get_root_cell());
        nb.processed_upto = ps_.processed_upto_;
        nb.blk_.id.shard = shard_.shard;
        LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb.blk_.to_str()
                   << " with shard expansion (immediate after-merge adjustment)";
      } else {
        LOG(DEBUG) << "disabling neighbor #" << i << " : " << nb.blk_.to_str() << " (immediate after-merge adjustment)";
        nb.disable();
      }
    }
  }
  CHECK(found == 2);
  return true;
}

// almost the same as in Collator
bool ValidateQuery::add_trivial_neighbor() {
  LOG(DEBUG) << "in add_trivial_neighbor()";
  if (after_merge_) {
    return add_trivial_neighbor_after_merge();
  }
  CHECK(prev_blocks.size() == 1);
  if (!prev_blocks[0].seqno()) {
    // skipping
    LOG(DEBUG) << "no trivial neighbor because previous block has zero seqno";
    return true;
  }
  CHECK(prev_state_root_.not_null());
  auto descr_ref = block::McShardDescr::from_state(prev_blocks[0], prev_state_root_);
  if (descr_ref.is_null()) {
    return reject_query("cannot deserialize header of previous state");
  }
  CHECK(descr_ref.not_null());
  CHECK(descr_ref->blk_ == prev_blocks[0]);
  CHECK(ps_.out_msg_queue_);
  ton::ShardIdFull prev_shard = descr_ref->shard();
  // Possible cases are:
  // 1. prev_shard = shard = one of neighbors
  //    => replace neighbor by (more recent) prev_shard info
  // 2. shard is child of prev_shard = one of neighbors
  //    => after_split must be set;
  //       replace neighbor by new split data (and shrink its shard);
  //       insert new virtual neighbor (our future sibling).
  // 3. prev_shard = shard = child of one of neighbors
  //    => after_split must be clear (we are continuing an after-split chain);
  //       make our virtual sibling from the neighbor (split its queue);
  //       insert ourselves from prev_shard data
  // In all of the above cases, our shard intersects exactly one neighbor, which has the same shard or its parent.
  // 4. there are two neighbors intersecting shard = prev_shard, which are its children.
  // 5. there are two prev_shards, the two children of shard, and two neighbors coinciding with prev_shards
  int found = 0, cs = 0;
  std::size_t n = neighbors_.size();
  for (std::size_t i = 0; i < n; i++) {
    auto& nb = neighbors_.at(i);
    if (ton::shard_intersects(nb.shard(), shard_)) {
      ++found;
      LOG(DEBUG) << "neighbor #" << i << " : " << nb.blk_.to_str() << " intersects our shard " << shard_.to_str();
      if (nb.shard() == prev_shard) {
        if (prev_shard == shard_) {
          // case 1. Normal.
          CHECK(found == 1);
          nb = *descr_ref;
          nb.set_queue_root(ps_.out_msg_queue_->get_root_cell());
          nb.processed_upto = ps_.processed_upto_;
          LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb.blk_.to_str() << " (simple replacement)";
          cs = 1;
        } else if (ton::shard_is_parent(nb.shard(), shard_)) {
          // case 2. Immediate after-split.
          CHECK(found == 1);
          CHECK(after_split_);
          CHECK(sibling_out_msg_queue_);
          CHECK(sibling_processed_upto_);
          neighbors_.emplace_back(*descr_ref);
          auto& nb2 = neighbors_.at(i);
          nb2.set_queue_root(sibling_out_msg_queue_->get_root_cell());
          nb2.processed_upto = sibling_processed_upto_;
          nb2.blk_.id.shard = ton::shard_sibling(shard_.shard);
          LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb2.blk_.to_str()
                     << " with shard shrinking to our sibling (immediate after-split adjustment)";
          auto& nb1 = neighbors_.at(n);
          nb1.set_queue_root(ps_.out_msg_queue_->get_root_cell());
          nb1.processed_upto = ps_.processed_upto_;
          nb1.blk_.id.shard = shard_.shard;
          LOG(DEBUG) << "created neighbor #" << n << " : " << nb1.blk_.to_str()
                     << " with shard shrinking to our (immediate after-split adjustment)";
          cs = 2;
        } else {
          return fatal_error("impossible shard configuration in add_trivial_neighbor()");
        }
      } else if (ton::shard_is_parent(nb.shard(), shard_) && shard_ == prev_shard) {
        // case 3. Continued after-split
        CHECK(found == 1);
        CHECK(!after_split_);
        CHECK(!sibling_out_msg_queue_);
        CHECK(!sibling_processed_upto_);
        neighbors_.emplace_back(*descr_ref);
        auto& nb2 = neighbors_.at(i);
        auto sib_shard = ton::shard_sibling(shard_);
        // compute the part of virtual sibling's OutMsgQueue with destinations in our shard
        sibling_out_msg_queue_ =
            std::make_unique<vm::AugmentedDictionary>(nb2.outmsg_root, 352, block::tlb::aug_OutMsgQueue);
        td::BitArray<96> pfx;
        pfx.bits().store_int(shard_.workchain, 32);
        (pfx.bits() + 32).store_uint(shard_.shard, 64);
        int l = ton::shard_prefix_length(shard_);
        CHECK(sibling_out_msg_queue_->cut_prefix_subdict(pfx.bits(), 32 + l));
        int res2 = block::filter_out_msg_queue(*sibling_out_msg_queue_, nb2.shard(), sib_shard);
        if (res2 < 0) {
          return fatal_error("cannot filter virtual sibling's OutMsgQueue from that of the last common ancestor");
        }
        nb2.set_queue_root(sibling_out_msg_queue_->get_root_cell());
        if (!nb2.processed_upto->split(sib_shard)) {
          return fatal_error("error splitting ProcessedUpto for our virtual sibling");
        }
        nb2.blk_.id.shard = ton::shard_sibling(shard_.shard);
        LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb2.blk_.to_str()
                   << " with shard shrinking to our sibling (continued after-split adjustment)";
        auto& nb1 = neighbors_.at(n);
        nb1.set_queue_root(ps_.out_msg_queue_->get_root_cell());
        nb1.processed_upto = ps_.processed_upto_;
        LOG(DEBUG) << "created neighbor #" << n << " : " << nb1.blk_.to_str()
                   << " from our preceding state (continued after-split adjustment)";
        cs = 3;
      } else if (ton::shard_is_parent(shard_, nb.shard()) && shard_ == prev_shard) {
        // case 4. Continued after-merge.
        if (found == 1) {
          cs = 4;
        }
        CHECK(cs == 4);
        CHECK(found <= 2);
        if (found == 1) {
          nb = *descr_ref;
          nb.set_queue_root(ps_.out_msg_queue_->get_root_cell());
          nb.processed_upto = ps_.processed_upto_;
          LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb.blk_.to_str()
                     << " with shard expansion (continued after-merge adjustment)";
        } else {
          LOG(DEBUG) << "disabling neighbor #" << i << " : " << nb.blk_.to_str()
                     << " (continued after-merge adjustment)";
          nb.disable();
        }
      } else {
        return fatal_error("impossible shard configuration in add_trivial_neighbor()");
      }
    }
  }
  CHECK(found && cs);
  CHECK(found == (1 + (cs == 4)));
  return true;
}

bool ValidateQuery::unpack_block_data() {
  LOG(DEBUG) << "unpacking block structures";
  block::gen::Block::Record blk;
  block::gen::BlockExtra::Record extra;
  if (!(tlb::unpack_cell(block_root_, blk) && tlb::unpack_cell(blk.extra, extra))) {
    return reject_query("cannot unpack Block header");
  }
  auto inmsg_cs = vm::load_cell_slice_ref(std::move(extra.in_msg_descr));
  auto outmsg_cs = vm::load_cell_slice_ref(std::move(extra.out_msg_descr));
  // run some hand-written checks from block::tlb::
  // (automatic tests from block::gen:: have been already run for the entire block)
  if (!block::tlb::t_InMsgDescr.validate_upto(1000000, *inmsg_cs)) {
    return reject_query("InMsgDescr of the new block failed to pass handwritten validity tests");
  }
  if (!block::tlb::t_OutMsgDescr.validate_upto(1000000, *outmsg_cs)) {
    return reject_query("OutMsgDescr of the new block failed to pass handwritten validity tests");
  }
  if (!block::tlb::t_ShardAccountBlocks.validate_ref(1000000, extra.account_blocks)) {
    return reject_query("ShardAccountBlocks of the new block failed to pass handwritten validity tests");
  }
  in_msg_dict_ = std::make_unique<vm::AugmentedDictionary>(std::move(inmsg_cs), 256, block::tlb::aug_InMsgDescr);
  out_msg_dict_ = std::make_unique<vm::AugmentedDictionary>(std::move(outmsg_cs), 256, block::tlb::aug_OutMsgDescr);
  account_blocks_dict_ = std::make_unique<vm::AugmentedDictionary>(
      vm::load_cell_slice_ref(std::move(extra.account_blocks)), 256, block::tlb::aug_ShardAccountBlocks);
  LOG(DEBUG) << "validating InMsgDescr";
  if (!in_msg_dict_->validate_all()) {
    return reject_query("InMsgDescr dictionary is invalid");
  }
  LOG(DEBUG) << "validating OutMsgDescr";
  if (!out_msg_dict_->validate_all()) {
    return reject_query("OutMsgDescr dictionary is invalid");
  }
  LOG(DEBUG) << "validating ShardAccountBlocks";
  if (!account_blocks_dict_->validate_all()) {
    return reject_query("ShardAccountBlocks dictionary is invalid");
  }
  return unpack_precheck_value_flow(std::move(blk.value_flow));
}

bool ValidateQuery::unpack_precheck_value_flow(Ref<vm::Cell> value_flow_root) {
  vm::CellSlice cs{vm::NoVmOrd(), value_flow_root};
  if (!(cs.is_valid() && value_flow_.fetch(cs) && cs.empty_ext())) {
    return reject_query("cannot unpack ValueFlow of the new block "s + id_.to_str());
  }
  std::ostringstream os;
  value_flow_.show(os);
  LOG(DEBUG) << "value flow: " << os.str();
  if (!value_flow_.validate()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() + " is invalid (in-balance is not equal to out-balance)");
  }
  if (!is_masterchain() && !value_flow_.minted.is_zero()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " is invalid (non-zero minted value in a non-masterchain block)");
  }
  if (!is_masterchain() && !value_flow_.recovered.is_zero()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " is invalid (non-zero recovered value in a non-masterchain block)");
  }
  if (!value_flow_.recovered.is_zero() && recover_create_msg_.is_null()) {
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " has a non-zero recovered fees value, but there is no recovery InMsg");
  }
  if (value_flow_.recovered.is_zero() && recover_create_msg_.not_null()) {
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " has a zero recovered fees value, but there is a recovery InMsg");
  }
  if (!value_flow_.minted.is_zero() && mint_msg_.is_null()) {
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " has a non-zero minted value, but there is no mint InMsg");
  }
  if (value_flow_.minted.is_zero() && mint_msg_.not_null()) {
    return reject_query("ValueFlow of block "s + id_.to_str() + " has a zero minted value, but there is a mint InMsg");
  }
  if (!value_flow_.minted.is_zero()) {
    block::CurrencyCollection to_mint;
    if (!compute_minted_amount(to_mint) || !to_mint.is_valid()) {
      return reject_query("cannot compute the correct amount of extra currencies to be minted");
    }
    if (value_flow_.minted != to_mint) {
      return reject_query("invalid extra currencies amount to be minted: declared "s + value_flow_.minted.to_str() +
                          ", expected " + to_mint.to_str());
    }
  }
  td::RefInt256 create_fee;
  if (is_masterchain()) {
    create_fee = masterchain_create_fee_;
  } else if (workchain() == basechainId) {
    create_fee = (basechain_create_fee_ >> ton::shard_prefix_length(shard_));
  } else {
    create_fee = td::make_refint(0);
  }
  if (value_flow_.created != block::CurrencyCollection{create_fee}) {
    return reject_query("ValueFlow of block "s + id_.to_str() + " declares block creation fee " +
                        value_flow_.created.to_str() + ", but the current configuration expects it to be " +
                        td::dec_string(create_fee));
  }
  if (!value_flow_.fees_imported.is_zero() && !is_masterchain()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " is invalid (non-zero fees_imported in a non-masterchain block)");
  }
  auto accounts_extra = ps_.account_dict_->get_root_extra();
  block::CurrencyCollection cc;
  if (!(accounts_extra.write().advance(5) && cc.unpack(std::move(accounts_extra)))) {
    return reject_query("cannot unpack CurrencyCollection from the root of old accounts dictionary");
  }
  if (cc != value_flow_.from_prev_blk) {
    return reject_query("ValueFlow for "s + id_.to_str() +
                        " declares from_prev_blk=" + value_flow_.from_prev_blk.to_str() +
                        " but the sum over all accounts present in the previous state is " + cc.to_str());
  }
  accounts_extra = ns_.account_dict_->get_root_extra();
  if (!(accounts_extra.write().advance(5) && cc.unpack(std::move(accounts_extra)))) {
    return reject_query("cannot unpack CurrencyCollection from the root of new accounts dictionary");
  }
  if (cc != value_flow_.to_next_blk) {
    return reject_query("ValueFlow for "s + id_.to_str() + " declares to_next_blk=" + value_flow_.to_next_blk.to_str() +
                        " but the sum over all accounts present in the new state is " + cc.to_str());
  }
  auto msg_extra = in_msg_dict_->get_root_extra();
  // block::gen::t_ImportFees.print(std::cerr, msg_extra);
  if (!(block::tlb::t_Grams.as_integer_skip_to(msg_extra.write(), import_fees_) && cc.unpack(std::move(msg_extra)))) {
    return reject_query("cannot unpack ImportFees from the augmentation of the InMsgDescr dictionary");
  }
  if (cc != value_flow_.imported) {
    return reject_query("ValueFlow for "s + id_.to_str() + " declares imported=" + value_flow_.imported.to_str() +
                        " but the sum over all inbound messages listed in InMsgDescr is " + cc.to_str());
  }
  if (!cc.unpack(out_msg_dict_->get_root_extra())) {
    return reject_query("cannot unpack CurrencyCollection from the augmentation of the InMsgDescr dictionary");
  }
  if (cc != value_flow_.exported) {
    return reject_query("ValueFlow for "s + id_.to_str() + " declares exported=" + value_flow_.exported.to_str() +
                        " but the sum over all outbound messages listed in OutMsgDescr is " + cc.to_str());
  }
  if (!transaction_fees_.validate_unpack(account_blocks_dict_->get_root_extra())) {
    return reject_query(
        "cannot unpack CurrencyCollection with total transaction fees from the augmentation of the ShardAccountBlocks "
        "dictionary");
  }
  auto expected_fees = value_flow_.fees_imported + value_flow_.created + transaction_fees_ + import_fees_;
  if (value_flow_.fees_collected != expected_fees) {
    return reject_query(PSTRING() << "ValueFlow for " << id_.to_str() << " declares fees_collected="
                                  << value_flow_.fees_collected.to_str() << " but the total message import fees are "
                                  << import_fees_ << ", the total transaction fees are " << transaction_fees_.to_str()
                                  << ", creation fee for this block is " << value_flow_.created.to_str()
                                  << " and the total imported fees from shards are "
                                  << value_flow_.fees_imported.to_str() << " with a total of "
                                  << expected_fees.to_str());
  }
  return true;
}

// similar to Collator::compute_minted_amount()
bool ValidateQuery::compute_minted_amount(block::CurrencyCollection& to_mint) {
  if (!is_masterchain()) {
    return to_mint.set_zero();
  }
  to_mint.set_zero();
  auto cell = config_->get_config_param(7);
  if (cell.is_null()) {
    return true;
  }
  if (!block::tlb::t_ExtraCurrencyCollection.validate_ref(cell)) {
    LOG(WARNING) << "configuration parameter #7 does not contain a valid ExtraCurrencyCollection, minting disabled";
    return true;
  }
  vm::Dictionary dict{vm::load_cell_slice(cell).prefetch_ref(), 32}, dict2{ps_.global_balance_.extra, 32}, dict3{32};
  if (!dict.check_for_each([this, &dict2, &dict3](Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 32);
        int curr_id = (int)key.get_int(32);
        auto amount = block::tlb::t_VarUInteger_32.as_integer(value);
        if (amount.is_null() || !amount->is_valid()) {
          return reject_query(PSTRING() << "cannot parse amount of currency #" << curr_id
                                        << " to be minted from configuration parameter #7");
        }
        auto value2 = dict2.lookup(key, 32);
        auto amount2 = value2.not_null() ? block::tlb::t_VarUInteger_32.as_integer(value2) : td::make_refint(0);
        if (amount2.is_null() || !amount2->is_valid()) {
          return reject_query(PSTRING() << "cannot parse amount of currency #" << curr_id
                                        << " from old global balance");
        }
        auto delta = amount - amount2;
        int s = td::sgn(delta);
        if (s) {
          LOG(INFO) << "currency #" << curr_id << ": existing " << amount2 << ", required " << amount
                    << ", to be minted " << delta;
          if (s == 1 && curr_id) {
            vm::CellBuilder cb;
            return (block::tlb::t_VarUInteger_32.store_integer_ref(cb, delta) &&
                    dict3.set_builder(key, 32, cb, vm::Dictionary::SetMode::Add)) ||
                   reject_query(PSTRING() << "cannot add " << delta << " of currency #" << curr_id << " to be minted");
          }
        }
        return true;
      })) {
    return reject_query("error scanning extra currencies to be minted");
  }
  to_mint.extra = std::move(dict3).extract_root_cell();
  if (!to_mint.is_zero()) {
    LOG(INFO) << "new currencies to be minted: " << to_mint.to_str();
  }
  return true;
}

bool ValidateQuery::precheck_one_account_update(td::ConstBitPtr acc_id, Ref<vm::CellSlice> old_value,
                                                Ref<vm::CellSlice> new_value) {
  LOG(DEBUG) << "checking update of account " << acc_id.to_hex(256);
  old_value = ps_.account_dict_->extract_value(std::move(old_value));
  new_value = ns_.account_dict_->extract_value(std::move(new_value));
  auto acc_blk_root = account_blocks_dict_->lookup(acc_id, 256);
  if (acc_blk_root.is_null()) {
    if (verbosity >= 3 * 0) {
      std::cerr << "state of account " << workchain() << ":" << acc_id.to_hex(256)
                << " in the old shardchain state:" << std::endl;
      if (old_value.not_null()) {
        block::gen::t_ShardAccount.print(std::cerr, *old_value);
      } else {
        std::cerr << "<absent>" << std::endl;
      }
      std::cerr << "state of account " << workchain() << ":" << acc_id.to_hex(256)
                << " in the new shardchain state:" << std::endl;
      if (new_value.not_null()) {
        block::gen::t_ShardAccount.print(std::cerr, *new_value);
      } else {
        std::cerr << "<absent>" << std::endl;
      }
    }
    return reject_query("the state of account "s + acc_id.to_hex(256) +
                        " changed in the new state with respect to the old state, but the block contains no "
                        "AccountBlock for this account");
  }
  if (new_value.not_null()) {
    if (!block::gen::t_ShardAccount.validate_csr(10000, new_value)) {
      return reject_query("new state of account "s + acc_id.to_hex(256) +
                          " failed to pass automated validity checks for ShardAccount");
    }
    if (!block::tlb::t_ShardAccount.validate_csr(10000, new_value)) {
      return reject_query("new state of account "s + acc_id.to_hex(256) +
                          " failed to pass hand-written validity checks for ShardAccount");
    }
  }
  block::gen::AccountBlock::Record acc_blk;
  block::gen::HASH_UPDATE::Record hash_upd;
  if (!(tlb::csr_unpack(std::move(acc_blk_root), acc_blk) &&
        tlb::type_unpack_cell(std::move(acc_blk.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd))) {
    return reject_query("cannot extract (HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256));
  }
  if (acc_blk.account_addr != acc_id) {
    return reject_query("AccountBlock of account "s + acc_id.to_hex(256) + " appears to belong to another account " +
                        acc_blk.account_addr.to_hex());
  }
  Ref<vm::Cell> old_state, new_state;
  if (!(block::tlb::t_ShardAccount.extract_account_state(old_value, old_state) &&
        block::tlb::t_ShardAccount.extract_account_state(new_value, new_state))) {
    return reject_query("cannot extract Account from the ShardAccount of "s + acc_id.to_hex(256));
  }
  if (hash_upd.old_hash != old_state->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256) +
                        " has incorrect old hash");
  }
  if (hash_upd.new_hash != new_state->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256) +
                        " has incorrect new hash");
  }
  return true;
}

bool ValidateQuery::precheck_account_updates() {
  LOG(INFO) << "pre-checking all Account updates between the old and the new state";
  try {
    CHECK(ps_.account_dict_ && ns_.account_dict_);
    if (!ps_.account_dict_->scan_diff(
            *ns_.account_dict_,
            [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val_extra,
                   Ref<vm::CellSlice> new_val_extra) {
              CHECK(key_len == 256);
              return precheck_one_account_update(key, std::move(old_val_extra), std::move(new_val_extra));
            },
            3 /* check augmentation of changed nodes */)) {
      return reject_query("invalid ShardAccounts dictionary in the new state");
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid ShardAccount dictionary difference between the old and the new state: "s +
                        err.get_msg());
  }
  return true;
}

bool ValidateQuery::precheck_one_transaction(td::ConstBitPtr acc_id, ton::LogicalTime trans_lt,
                                             Ref<vm::CellSlice> trans_csr, ton::Bits256& prev_trans_hash,
                                             ton::LogicalTime& prev_trans_lt, unsigned& prev_trans_lt_len,
                                             ton::Bits256& acc_state_hash) {
  LOG(DEBUG) << "checking Transaction " << trans_lt;
  if (trans_csr.is_null() || trans_csr->size_ext() != 0x10000) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256) << " is invalid");
  }
  auto trans_root = trans_csr->prefetch_ref();
  block::gen::Transaction::Record trans;
  block::gen::HASH_UPDATE::Record hash_upd;
  if (!(tlb::unpack_cell(trans_root, trans) &&
        tlb::type_unpack_cell(std::move(trans.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd))) {
    return reject_query(PSTRING() << "cannot unpack transaction " << trans_lt << " of " << acc_id.to_hex(256));
  }
  if (trans.account_addr != acc_id || trans.lt != trans_lt) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256)
                                  << " claims to be transaction " << trans.lt << " of " << trans.account_addr.to_hex());
  }
  if (trans.now != now_) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256)
                                  << " claims that current time is " << trans.now
                                  << " while the block header indicates " << now_);
  }
  if (trans.prev_trans_hash != prev_trans_hash || trans.prev_trans_lt != prev_trans_lt) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256)
                                  << " claims that the previous transaction was " << trans.prev_trans_lt << ":"
                                  << trans.prev_trans_hash.to_hex() << " while the correct value is " << prev_trans_lt
                                  << ":" << prev_trans_hash.to_hex());
  }
  if (trans_lt < prev_trans_lt + prev_trans_lt_len) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256)
                                  << " starts at logical time " << trans_lt
                                  << ", earlier than the previous transaction " << prev_trans_lt << " .. "
                                  << prev_trans_lt + prev_trans_lt_len << " ends");
  }
  unsigned lt_len = trans.outmsg_cnt + 1;
  if (trans_lt <= start_lt_ || trans_lt + lt_len > end_lt_) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " .. " << trans_lt + lt_len << " of "
                                  << acc_id.to_hex(256) << " is not inside the logical time interval " << start_lt_
                                  << " .. " << end_lt_ << " of the encompassing new block");
  }
  if (hash_upd.old_hash != acc_state_hash) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256)
                                  << " claims to start from account state with hash " << hash_upd.old_hash.to_hex()
                                  << " while the actual value is " << acc_state_hash.to_hex());
  }
  prev_trans_lt = trans_lt;
  prev_trans_lt_len = lt_len;
  prev_trans_hash = trans_root->get_hash().bits();
  acc_state_hash = hash_upd.new_hash;
  unsigned c = 0;
  vm::Dictionary out_msgs{trans.r1.out_msgs, 15};
  if (!out_msgs.check_for_each([&c](Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 15);
        return key.get_uint(15) == c++;
      }) ||
      c != (unsigned)trans.outmsg_cnt) {
    return reject_query(PSTRING() << "transaction " << trans_lt << " of " << acc_id.to_hex(256)
                                  << " has invalid indices in the out_msg dictionary (keys 0 .. "
                                  << trans.outmsg_cnt - 1 << " expected)");
  }
  return true;
}

// NB: could be run in parallel for different accounts
bool ValidateQuery::precheck_one_account_block(td::ConstBitPtr acc_id, Ref<vm::CellSlice> acc_blk_root) {
  LOG(DEBUG) << "checking AccountBlock for " << acc_id.to_hex(256);
  if (!acc_id.equals(shard_pfx_.bits(), shard_pfx_len_)) {
    return reject_query("new block "s + id_.to_str() + " contains AccountBlock for account " + acc_id.to_hex(256) +
                        " not belonging to the block's shard " + shard_.to_str());
  }
  CHECK(acc_blk_root.not_null());
  // acc_blk_root->print_rec(std::cerr);
  // block::gen::t_AccountBlock.print(std::cerr, acc_blk_root);
  block::gen::AccountBlock::Record acc_blk;
  block::gen::HASH_UPDATE::Record hash_upd;
  if (!(tlb::csr_unpack(acc_blk_root, acc_blk) &&
        tlb::type_unpack_cell(std::move(acc_blk.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd))) {
    return reject_query("cannot extract (HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256));
  }
  if (acc_blk.account_addr != acc_id) {
    return reject_query("AccountBlock of account "s + acc_id.to_hex(256) + " appears to belong to another account " +
                        acc_blk.account_addr.to_hex());
  }
  block::tlb::ShardAccount::Record old_state, new_state;
  if (!(old_state.unpack(ps_.account_dict_->lookup(acc_id, 256)) &&
        new_state.unpack(ns_.account_dict_->lookup(acc_id, 256)))) {
    return reject_query("cannot extract Account from the ShardAccount of "s + acc_id.to_hex(256));
  }
  if (hash_upd.old_hash != old_state.account->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256) +
                        " has incorrect old hash");
  }
  if (hash_upd.new_hash != new_state.account->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256) +
                        " has incorrect new hash");
  }
  if (!block::gen::t_AccountBlock.validate_upto(1000000, *acc_blk_root)) {
    return reject_query("AccountBlock of "s + acc_id.to_hex(256) + " failed to pass automated validity checks");
  }
  if (!block::tlb::t_AccountBlock.validate_upto(1000000, *acc_blk_root)) {
    return reject_query("AccountBlock of "s + acc_id.to_hex(256) + " failed to pass hand-written validity checks");
  }
  unsigned last_trans_lt_len = 1;
  ton::Bits256 acc_state_hash = hash_upd.old_hash;
  try {
    vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                       block::tlb::aug_AccountTransactions};
    td::BitArray<64> min_trans, max_trans;
    if (trans_dict.get_minmax_key(min_trans).is_null() || trans_dict.get_minmax_key(max_trans, true).is_null()) {
      return reject_query("cannot extract minimal and maximal keys from the transaction dictionary of account "s +
                          acc_id.to_hex(256));
    }
    if (min_trans.to_ulong() <= start_lt_ || max_trans.to_ulong() >= end_lt_) {
      return reject_query(PSTRING() << "new block contains transactions " << min_trans.to_ulong() << " .. "
                                    << max_trans.to_ulong() << " outside of the block's lt range " << start_lt_
                                    << " .. " << end_lt_);
    }
    if (!trans_dict.validate_check_extra(
            [this, acc_id, &old_state, &last_trans_lt_len, &acc_state_hash](
                Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
              CHECK(key_len == 64);
              return precheck_one_transaction(acc_id, key.get_uint(64), std::move(value), old_state.last_trans_hash,
                                              old_state.last_trans_lt, last_trans_lt_len, acc_state_hash) ||
                     reject_query(PSTRING() << "transaction " << key.get_uint(64) << " of account "
                                            << acc_id.to_hex(256) << " is invalid");
            })) {
      return reject_query("invalid transaction dictionary in AccountBlock of "s + acc_id.to_hex(256));
    }
    if (!new_state.is_zero && (old_state.last_trans_lt != new_state.last_trans_lt ||
                               old_state.last_trans_hash != new_state.last_trans_hash)) {
      return reject_query(PSTRING() << "last transaction mismatch for account " << acc_id.to_hex(256)
                                    << " : block lists " << old_state.last_trans_lt << ":"
                                    << old_state.last_trans_hash.to_hex() << " but the new state claims that it is "
                                    << new_state.last_trans_lt << ":" << new_state.last_trans_hash.to_hex());
    }
    if (acc_state_hash != hash_upd.new_hash) {
      return reject_query("final state hash mismatch in (HASH_UPDATE Account) for account "s + acc_id.to_hex(256));
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid transaction dictionary in AccountBlock of "s + acc_id.to_hex(256) + " : " +
                        err.get_msg());
  }
  return true;
}

bool ValidateQuery::precheck_account_transactions() {
  LOG(INFO) << "pre-checking all AccountBlocks, and all transactions of all accounts";
  try {
    CHECK(account_blocks_dict_);
    if (!account_blocks_dict_->validate_check_extra(
            [this](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
              CHECK(key_len == 256);
              return precheck_one_account_block(key, std::move(value)) ||
                     reject_query("invalid AccountBlock for account "s + key.to_hex(256) + " in the new block "s +
                                  id_.to_str());
            })) {
      return reject_query("invalid ShardAccountBlock dictionary in the new block "s + id_.to_str());
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid ShardAccountBlocks dictionary: "s + err.get_msg());
  }
  return true;
}

Ref<vm::Cell> ValidateQuery::lookup_transaction(const ton::StdSmcAddress& addr, ton::LogicalTime lt) const {
  CHECK(account_blocks_dict_);
  block::gen::AccountBlock::Record ab_rec;
  if (!tlb::csr_unpack_safe(account_blocks_dict_->lookup(addr), ab_rec)) {
    return {};
  }
  vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(ab_rec.transactions), 64,
                                     block::tlb::aug_AccountTransactions};
  return trans_dict.lookup_ref(td::BitArray<64>{(long long)lt});
}

// checks that a ^Transaction refers to a transaction present in the ShardAccountBlocks
bool ValidateQuery::is_valid_transaction_ref(Ref<vm::Cell> trans_ref) const {
  ton::StdSmcAddress addr;
  ton::LogicalTime lt;
  if (!block::get_transaction_id(trans_ref, addr, lt)) {
    LOG(DEBUG) << "cannot parse transaction header";
    return false;
  }
  auto trans = lookup_transaction(addr, lt);
  if (trans.is_null()) {
    LOG(DEBUG) << "transaction " << lt << " of " << addr.to_hex() << " not found";
    return false;
  }
  if (trans->get_hash() != trans_ref->get_hash()) {
    LOG(DEBUG) << "transaction " << lt << " of " << addr.to_hex() << " has a different hash";
    return false;
  }
  return true;
}

// checks that any change in OutMsgQueue in the state is accompanied by an OutMsgDescr record in the block
// also checks that the keys are correct
bool ValidateQuery::precheck_one_message_queue_update(td::ConstBitPtr out_msg_id, Ref<vm::CellSlice> old_value,
                                                      Ref<vm::CellSlice> new_value) {
  LOG(DEBUG) << "checking update of enqueued outbound message " << out_msg_id.get_int(32) << ":"
             << (out_msg_id + 32).to_hex(64) << "... with hash " << (out_msg_id + 96).to_hex(256);
  old_value = ps_.out_msg_queue_->extract_value(std::move(old_value));
  new_value = ns_.out_msg_queue_->extract_value(std::move(new_value));
  CHECK(old_value.not_null() || new_value.not_null());
  if (old_value.not_null() && old_value->size_ext() != 0x10040) {
    return reject_query("old EnqueuedMsg with key "s + out_msg_id.to_hex(352) + " is invalid");
  }
  if (new_value.not_null() && new_value->size_ext() != 0x10040) {
    return reject_query("new EnqueuedMsg with key "s + out_msg_id.to_hex(352) + " is invalid");
  }
  if (new_value.not_null()) {
    if (!block::gen::t_EnqueuedMsg.validate_csr(new_value)) {
      return reject_query("new EnqueuedMsg with key "s + out_msg_id.to_hex(352) +
                          " failed to pass automated validity checks");
    }
    if (!block::tlb::t_EnqueuedMsg.validate_csr(new_value)) {
      return reject_query("new EnqueuedMsg with key "s + out_msg_id.to_hex(352) +
                          " failed to pass hand-written validity checks");
    }
    ton::LogicalTime enqueued_lt = new_value->prefetch_ulong(64);
    if (enqueued_lt < start_lt_ || enqueued_lt >= end_lt_) {
      return reject_query(PSTRING() << "new EnqueuedMsg with key "s + out_msg_id.to_hex(352) + " has enqueued_lt="
                                    << enqueued_lt << " outside of this block's range " << start_lt_ << " .. "
                                    << end_lt_);
    }
  }
  if (old_value.not_null()) {
    if (!block::gen::t_EnqueuedMsg.validate_csr(old_value)) {
      return reject_query("old EnqueuedMsg with key "s + out_msg_id.to_hex(352) +
                          " failed to pass automated validity checks");
    }
    if (!block::tlb::t_EnqueuedMsg.validate_csr(old_value)) {
      return reject_query("old EnqueuedMsg with key "s + out_msg_id.to_hex(352) +
                          " failed to pass hand-written validity checks");
    }
    ton::LogicalTime enqueued_lt = old_value->prefetch_ulong(64);
    if (enqueued_lt >= start_lt_) {
      return reject_query(PSTRING() << "new EnqueuedMsg with key "s + out_msg_id.to_hex(352) + " has enqueued_lt="
                                    << enqueued_lt << " greater than or equal to this block's start_lt=" << start_lt_);
    }
  }
  int mode = old_value.not_null() + new_value.not_null() * 2;
  static const char* m_str[] = {"", "de", "en", "re"};
  auto out_msg_cs = out_msg_dict_->lookup(out_msg_id + 96, 256);
  if (out_msg_cs.is_null()) {
    return reject_query("no OutMsgDescr corresponding to "s + m_str[mode] + "queued message with key " +
                        out_msg_id.to_hex(352));
  }
  if (mode == 3) {
    return reject_query("EnqueuedMsg with key "s + out_msg_id.to_hex(352) +
                        " has been changed in the OutMsgQueue, but the key did not change");
  }
  auto q_msg_env = (old_value.not_null() ? old_value : new_value)->prefetch_ref();
  int tag = (int)out_msg_cs->prefetch_ulong(3);
  // mode for msg_export_{ext,new,imm,tr,deq_imm,???,deq/deq_short,tr_req}
  static const int tag_mode[8] = {0, 2, 0, 2, 1, 0, 1, 3};
  static const char* tag_str[8] = {"ext", "new", "imm", "tr", "deq_imm", "???", "deq", "tr_req"};
  if (tag < 0 || tag >= 8 || !(tag_mode[tag] & mode)) {
    return reject_query(PSTRING() << "OutMsgDescr corresponding to " << m_str[mode] << "queued message with key "
                                  << out_msg_id.to_hex(352) << " has invalid tag " << tag << "(" << tag_str[tag & 7]
                                  << ")");
  }
  bool is_short = (tag == 6 && (out_msg_cs->prefetch_ulong(4) &
                                1));  // msg_export_deq_short does not contain true MsgEnvelope / Message
  Ref<vm::Cell> msg_env, msg;
  td::Bits256 msg_env_hash;
  block::gen::OutMsg::Record_msg_export_deq_short deq_short;
  if (!is_short) {
    msg_env = out_msg_cs->prefetch_ref();
    if (msg_env.is_null()) {
      return reject_query("OutMsgDescr for "s + out_msg_id.to_hex(352) + " is invalid (contains no MsgEnvelope)");
    }
    msg_env_hash = msg_env->get_hash().bits();
    msg = vm::load_cell_slice(msg_env).prefetch_ref();
    if (msg.is_null()) {
      return reject_query("OutMsgDescr for "s + out_msg_id.to_hex(352) + " is invalid (contains no message)");
    }
    if (msg->get_hash().as_bitslice() != out_msg_id + 96) {
      return reject_query("OutMsgDescr for "s + (out_msg_id + 96).to_hex(256) +
                          " contains a message with different hash "s + msg->get_hash().bits().to_hex(256));
    }
  } else {
    if (!tlb::csr_unpack(out_msg_cs, deq_short)) {  // parsing msg_export_deq_short$1101 ...
      return reject_query("OutMsgDescr for "s + out_msg_id.to_hex(352) +
                          " is invalid (cannot unpack msg_export_deq_short)");
    }
    msg_env_hash = deq_short.msg_env_hash;
  }
  //
  if (mode == 1) {
    // dequeued message
    if (tag == 7) {
      // this is a msg_export_tr_req$111, a re-queued transit message (after merge)
      // check that q_msg_env still contains msg
      auto q_msg = vm::load_cell_slice(q_msg_env).prefetch_ref();
      if (q_msg.is_null()) {
        return reject_query("MsgEnvelope in the old outbound queue with key "s + out_msg_id.to_hex(352) +
                            " is invalid");
      }
      if (q_msg->get_hash().as_bitslice() != msg->get_hash().bits()) {
        return reject_query("MsgEnvelope in the old outbound queue with key "s + out_msg_id.to_hex(352) +
                            " contains a Message with incorrect hash " + q_msg->get_hash().bits().to_hex(256));
      }
      auto import = out_msg_cs->prefetch_ref(1);
      if (import.is_null()) {
        return reject_query("OutMsgDescr for "s + out_msg_id.to_hex(352) + " is not a valid msg_export_tr_req");
      }
      auto import_cs = vm::load_cell_slice(std::move(import));
      int import_tag = (int)import_cs.prefetch_ulong(3);
      if (import_tag != 4) {
        // must be msg_import_tr$100
        return reject_query(PSTRING() << "OutMsgDescr for " << out_msg_id.to_hex(352)
                                      << " refers to a reimport InMsgDescr with invalid tag " << import_tag
                                      << " instead of msg_import_tr$100");
      }
      auto in_msg_env = import_cs.prefetch_ref();
      if (in_msg_env.is_null()) {
        return reject_query("OutMsgDescr for "s + out_msg_id.to_hex(352) +
                            " is a msg_export_tr_req referring to an invalid reimport InMsgDescr");
      }
      if (in_msg_env->get_hash().as_bitslice() != q_msg_env->get_hash().bits()) {
        return reject_query("OutMsgDescr corresponding to dequeued message with key "s + out_msg_id.to_hex(352) +
                            " is a msg_export_tr_req referring to a reimport InMsgDescr that contains a MsgEnvelope "
                            "distinct from that originally kept in the old queue");
      }
    } else if (msg_env_hash != q_msg_env->get_hash().bits()) {
      return reject_query("OutMsgDescr corresponding to dequeued message with key "s + out_msg_id.to_hex(352) +
                          " contains a MsgEnvelope distinct from that originally kept in the old queue");
    }
  } else {
    // enqueued message
    if (msg_env_hash != q_msg_env->get_hash().bits()) {
      return reject_query("OutMsgDescr corresponding to "s + m_str[mode] + "queued message with key "s +
                          out_msg_id.to_hex(352) +
                          " contains a MsgEnvelope distinct from that stored in the new queue");
    }
  }
  // in all cases above, we have to check that all 352-bit key is correct (including first 96 bits)
  // otherwise we might not be able to correctly recover OutMsgQueue entries starting from OutMsgDescr later
  // or we might have several OutMsgQueue entries with different 352-bit keys all having the same last 256 bits (with the message hash)
  if (is_short) {
    // check out_msg_id using fields next_workchain:int32 next_addr_pfx:uint64 of msg_export_deq_short$1101
    if (out_msg_id.get_int(32) != deq_short.next_workchain ||
        (out_msg_id + 32).get_uint(64) != deq_short.next_addr_pfx) {
      return reject_query(
          PSTRING() << "OutMsgQueue entry with key " << out_msg_id.to_hex(352)
                    << " corresponds to msg_export_deq_short OutMsg entry with incorrect next hop parameters "
                    << deq_short.next_workchain << "," << deq_short.next_addr_pfx);
    }
  }
  td::BitArray<352> key;
  if (!block::compute_out_msg_queue_key(q_msg_env, key)) {
    return reject_query("OutMsgQueue entry with key "s + out_msg_id.to_hex(352) +
                        " refers to a MsgEnvelope that cannot be unpacked");
  }
  if (key != out_msg_id) {
    return reject_query("OutMsgQueue entry with key "s + out_msg_id.to_hex(352) +
                        " contains a MsgEnvelope that should have been stored under different key " + key.to_hex());
  }
  return true;
}

bool ValidateQuery::precheck_message_queue_update() {
  LOG(INFO) << "pre-checking the difference between the old and the new outbound message queues";
  try {
    CHECK(ps_.out_msg_queue_ && ns_.out_msg_queue_);
    CHECK(out_msg_dict_);
    if (!ps_.out_msg_queue_->scan_diff(
            *ns_.out_msg_queue_,
            [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val_extra,
                   Ref<vm::CellSlice> new_val_extra) {
              CHECK(key_len == 352);
              return precheck_one_message_queue_update(key, std::move(old_val_extra), std::move(new_val_extra));
            },
            3 /* check augmentation of changed nodes */)) {
      return reject_query("invalid OutMsgQueue dictionary in the new state");
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid OutMsgQueue dictionary difference between the old and the new state: "s +
                        err.get_msg());
  }
  return true;
}

bool ValidateQuery::update_max_processed_lt_hash(ton::LogicalTime lt, const ton::Bits256& hash) {
  if (proc_lt_ < lt || (proc_lt_ == lt && proc_hash_ < hash)) {
    proc_lt_ = lt;
    proc_hash_ = hash;
  }
  return true;
}

bool ValidateQuery::update_min_enqueued_lt_hash(ton::LogicalTime lt, const ton::Bits256& hash) {
  if (lt < min_enq_lt_ || (lt == min_enq_lt_ && hash < min_enq_hash_)) {
    min_enq_lt_ = lt;
    min_enq_hash_ = hash;
  }
  return true;
}

// check that the enveloped message (MsgEnvelope) was present in the output queue of a neighbor, and that it has not been processed before
bool ValidateQuery::check_imported_message(Ref<vm::Cell> msg_env) {
  block::tlb::MsgEnvelope::Record_std env;
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  ton::AccountIdPrefixFull src_prefix, dest_prefix, cur_prefix, next_prefix;
  if (!(msg_env.not_null() && tlb::unpack_cell(msg_env, env) && tlb::unpack_cell_inexact(env.msg, info) &&
        block::tlb::t_MsgAddressInt.get_prefix_to(std::move(info.src), src_prefix) &&
        block::tlb::t_MsgAddressInt.get_prefix_to(std::move(info.dest), dest_prefix) &&
        block::interpolate_addr_to(src_prefix, dest_prefix, env.cur_addr, cur_prefix) &&
        block::interpolate_addr_to(src_prefix, dest_prefix, env.next_addr, next_prefix))) {
    return reject_query("cannot unpack MsgEnvelope of an imported internal message with hash "s +
                        (env.msg.not_null() ? env.msg->get_hash().to_hex() : "(unknown)"));
  }
  if (!ton::shard_contains(shard_, next_prefix)) {
    return reject_query("imported message with hash "s + env.msg->get_hash().to_hex() + " has next hop address " +
                        next_prefix.to_str() + "... not in this shard");
  }
  td::BitArray<32 + 64 + 256> key;
  key.bits().store_int(next_prefix.workchain, 32);
  (key.bits() + 32).store_int(next_prefix.account_id_prefix, 64);
  (key.bits() + 96).copy_from(env.msg->get_hash().bits(), 256);
  for (const auto& nb : neighbors_) {
    if (!nb.is_disabled() && nb.contains(cur_prefix)) {
      CHECK(nb.out_msg_queue);
      auto nqv = nb.out_msg_queue->lookup_with_extra(key.bits(), key.size());
      if (nqv.is_null()) {
        return reject_query("imported internal message with hash "s + env.msg->get_hash().to_hex() +
                            " and previous address " + cur_prefix.to_str() + "..., next hop address " +
                            next_prefix.to_str() + " could not be found in the outbound message queue of neighbor " +
                            nb.blk_.to_str() + " under key " + key.to_hex());
      }
      block::EnqueuedMsgDescr enq_msg_descr;
      unsigned long long created_lt;
      if (!(nqv.write().fetch_ulong_bool(64, created_lt)  // augmentation
            && enq_msg_descr.unpack(nqv.write())          // unpack EnqueuedMsg
            && enq_msg_descr.check_key(key.bits())        // check key
            && enq_msg_descr.lt_ == created_lt)) {
        return reject_query("imported internal message from the outbound message queue of neighbor " +
                            nb.blk_.to_str() + " under key " + key.to_hex() +
                            " has an invalid EnqueuedMsg record in that queue");
      }
      if (enq_msg_descr.msg_env_->get_hash() != msg_env->get_hash()) {
        return reject_query("imported internal message from the outbound message queue of neighbor " +
                            nb.blk_.to_str() + " under key " + key.to_hex() +
                            " had a different MsgEnvelope in that outbound message queue");
      }
      if (ps_.processed_upto_->already_processed(enq_msg_descr)) {
        return reject_query(PSTRING() << "imported internal message with hash " << env.msg->get_hash().bits()
                                      << " and lt=" << created_lt
                                      << " has been already imported by a previous block of this shardchain");
      }
      update_max_processed_lt_hash(enq_msg_descr.lt_, enq_msg_descr.hash_);
      return true;
    }
  }
  return reject_query("imported internal message with hash "s + env.msg->get_hash().to_hex() +
                      " and previous address " + cur_prefix.to_str() + "..., next hop address " + next_prefix.to_str() +
                      " has previous address not belonging to any neighbor");
}

bool ValidateQuery::is_special_in_msg(const vm::CellSlice& in_msg) const {
  return (recover_create_msg_.not_null() && vm::load_cell_slice(recover_create_msg_).contents_equal(in_msg)) ||
         (mint_msg_.not_null() && vm::load_cell_slice(mint_msg_).contents_equal(in_msg));
}

bool ValidateQuery::check_in_msg(td::ConstBitPtr key, Ref<vm::CellSlice> in_msg) {
  LOG(DEBUG) << "checking InMsg with key " << key.to_hex(256);
  CHECK(in_msg.not_null());
  int tag = block::gen::t_InMsg.get_tag(*in_msg);
  CHECK(tag >= 0);  // NB: the block has been already checked to be valid TL-B in try_validate()
  ton::StdSmcAddress addr;
  ton::WorkchainId wc;
  Ref<vm::CellSlice> src, dest;
  Ref<vm::Cell> transaction;
  Ref<vm::Cell> msg, msg_env, tr_msg_env;
  // msg_envelope#4 cur_addr:IntermediateAddress next_addr:IntermediateAddress fwd_fee_remaining:Grams msg:^(Message Any) = MsgEnvelope;
  block::tlb::MsgEnvelope::Record_std env;
  // int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
  //   src:MsgAddressInt dest:MsgAddressInt
  //   value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams
  //   created_lt:uint64 created_at:uint32 = CommonMsgInfo;
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  ton::AccountIdPrefixFull src_prefix, dest_prefix, cur_prefix, next_prefix;
  td::RefInt256 fwd_fee, orig_fwd_fee;
  // initial checks and unpack
  switch (tag) {
    case block::gen::InMsg::msg_import_ext: {
      // msg_import_ext$000 msg:^(Message Any) transaction:^Transaction
      // importing an inbound external message
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info_ext;
      vm::CellSlice cs{*in_msg};
      CHECK(block::gen::t_InMsg.unpack_msg_import_ext(cs, msg, transaction));
      if (msg->get_hash().as_bitslice() != key) {
        return reject_query("InMsg with key "s + key.to_hex(256) + " refers to a message with different hash " +
                            msg->get_hash().to_hex());
      }
      if (!tlb::unpack_cell_inexact(msg, info_ext)) {
        return reject_query("InMsg with key "s + key.to_hex(256) +
                            " is a msg_import_ext$000, but it does not refer to an inbound external message");
      }
      dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info_ext.dest);
      if (!dest_prefix.is_valid()) {
        return reject_query("destination of inbound external message with hash "s + key.to_hex(256) +
                            " is an invalid blockchain address");
      }
      if (!ton::shard_contains(shard_, dest_prefix)) {
        return reject_query("inbound external message with hash "s + key.to_hex(256) + " has destination address " +
                            dest_prefix.to_str() + "... not in this shard");
      }
      dest = std::move(info_ext.dest);
      if (!block::tlb::t_MsgAddressInt.extract_std_address(dest, wc, addr)) {
        return reject_query("cannot unpack destination address of inbound external message with hash "s +
                            key.to_hex(256));
      }
      break;
    }
    case block::gen::InMsg::msg_import_imm: {
      // msg_import_imm$011 in_msg:^MsgEnvelope transaction:^Transaction fwd_fee:Grams
      // importing and processing an internal message generated in this very block
      block::gen::InMsg::Record_msg_import_imm inp;
      unsigned long long created_lt = 0;
      CHECK(tlb::csr_unpack(in_msg, inp) && tlb::unpack_cell(inp.in_msg, env) &&
            block::tlb::t_MsgEnvelope.get_created_lt(vm::load_cell_slice(inp.in_msg), created_lt) &&
            (fwd_fee = block::tlb::t_Grams.as_integer(std::move(inp.fwd_fee))).not_null());
      transaction = std::move(inp.transaction);
      msg_env = std::move(inp.in_msg);
      msg = env.msg;
      if (!is_special_in_msg(*in_msg)) {
        update_max_processed_lt_hash(created_lt, msg->get_hash().bits());
      }
      // ...
      break;
    }
    case block::gen::InMsg::msg_import_fin: {
      // msg_import_fin$100 in_msg:^MsgEnvelope transaction:^Transaction fwd_fee:Grams
      // importing and processing an internal message with destination in this shard
      block::gen::InMsg::Record_msg_import_fin inp;
      CHECK(tlb::csr_unpack(in_msg, inp) && tlb::unpack_cell(inp.in_msg, env) &&
            (fwd_fee = block::tlb::t_Grams.as_integer(std::move(inp.fwd_fee))).not_null());
      transaction = std::move(inp.transaction);
      msg_env = std::move(inp.in_msg);
      msg = env.msg;
      // ...
      break;
    }
    case block::gen::InMsg::msg_import_tr: {
      // msg_import_tr$101 in_msg:^MsgEnvelope out_msg:^MsgEnvelope transit_fee:Grams
      // importing and relaying a (transit) internal message with destination outside this shard
      block::gen::InMsg::Record_msg_import_tr inp;
      CHECK(tlb::csr_unpack(in_msg, inp) && tlb::unpack_cell(inp.in_msg, env) &&
            (fwd_fee = block::tlb::t_Grams.as_integer(std::move(inp.transit_fee))).not_null());
      msg_env = std::move(inp.in_msg);
      msg = env.msg;
      tr_msg_env = std::move(inp.out_msg);
      // ...
      break;
    }
    case block::gen::InMsg::msg_import_ihr:
      // msg_import_ihr$010 msg:^(Message Any) transaction:^Transaction ihr_fee:Grams proof_created:^Cell
      return reject_query("InMsg with key "s + key.to_hex(256) +
                          " is a msg_import_ihr, but IHR messages are not enabled in this version");
    case block::gen::InMsg::msg_discard_tr:
      // msg_discard_tr$111 in_msg:^MsgEnvelope transaction_id:uint64 fwd_fee:Grams proof_delivered:^Cell
      return reject_query("InMsg with key "s + key.to_hex(256) +
                          " is a msg_discard_tr, but IHR messages are not enabled in this version");
    case block::gen::InMsg::msg_discard_fin:
      // msg_discard_fin$110 in_msg:^MsgEnvelope transaction_id:uint64 fwd_fee:Grams
      return reject_query("InMsg with key "s + key.to_hex(256) +
                          " is a msg_discard_fin, but IHR messages are not enabled in this version");
    default:
      return reject_query(PSTRING() << "InMsg with key " << key.to_hex(256) << " has impossible tag " << tag);
  }
  // common checks for all (non-external) inbound messages
  CHECK(msg.not_null());
  if (msg->get_hash().as_bitslice() != key) {
    return reject_query("InMsg with key "s + key.to_hex(256) + " refers to a message with different hash " +
                        msg->get_hash().to_hex());
  }
  if (tag != block::gen::InMsg::msg_import_ext) {
    // unpack int_msg_info$0 ... = CommonMsgInfo, especially message addresses
    if (!tlb::unpack_cell_inexact(msg, info)) {
      return reject_query("InMsg with key "s + key.to_hex(256) +
                          " is not a msg_import_ext$000, but it does not refer to an inbound internal message");
    }
    // extract source, current, next hop and destination address prefixes
    dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
    if (!dest_prefix.is_valid()) {
      return reject_query("destination of inbound internal message with hash "s + key.to_hex(256) +
                          " is an invalid blockchain address");
    }
    src_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.src);
    if (!src_prefix.is_valid()) {
      return reject_query("source of inbound internal message with hash "s + key.to_hex(256) +
                          " is an invalid blockchain address");
    }
    cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
    next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
    if (!(cur_prefix.is_valid() && next_prefix.is_valid())) {
      return reject_query("cannot compute current and next hop addresses of inbound internal message with hash "s +
                          key.to_hex(256));
    }
    // check that next hop is nearer to the destination than the current address
    if (count_matching_bits(dest_prefix, next_prefix) < count_matching_bits(dest_prefix, cur_prefix)) {
      return reject_query("next hop address "s + next_prefix.to_str() + "... of inbound internal message with hash " +
                          key.to_hex(256) + " is further from its destination " + dest_prefix.to_str() +
                          "... than its current address " + cur_prefix.to_str() + "...");
    }
    // next hop address must belong to this shard (otherwise we should never had imported this message)
    if (!ton::shard_contains(shard_, next_prefix)) {
      return reject_query("next hop address "s + next_prefix.to_str() + "... of inbound internal message with hash " +
                          key.to_hex(256) + " does not belong to the current block's shard " + shard_.to_str());
    }
    // next hop may coincide with current address only if destination is already reached
    if (next_prefix == cur_prefix && cur_prefix != dest_prefix) {
      return reject_query(
          "next hop address "s + next_prefix.to_str() + "... of inbound internal message with hash " + key.to_hex(256) +
          " coincides with its current address, but this message has not reached its final destination " +
          dest_prefix.to_str() + "... yet");
    }
    // if a message is processed by a transaction, it must have destination inside the current shard
    if (transaction.not_null() && !ton::shard_contains(shard_, dest_prefix)) {
      return reject_query("inbound internal message with hash "s + key.to_hex(256) + " has destination address " +
                          dest_prefix.to_str() + "... not in this shard, but it is processed nonetheless");
    }
    // if a message is not processed by a transaction, its final destination must be outside this shard
    if (transaction.is_null() && ton::shard_contains(shard_, dest_prefix)) {
      return reject_query("inbound internal message with hash "s + key.to_hex(256) + " has destination address " +
                          dest_prefix.to_str() + "... in this shard, but it is not processed by a transaction");
    }
    src = std::move(info.src);
    dest = std::move(info.dest);
    // unpack complete destination address if it is inside this shard
    if (transaction.not_null() && !block::tlb::t_MsgAddressInt.extract_std_address(dest, wc, addr)) {
      return reject_query("cannot unpack destination address of inbound internal message with hash "s +
                          key.to_hex(256));
    }
    // unpack original forwarding fee
    orig_fwd_fee = block::tlb::t_Grams.as_integer(info.fwd_fee);
    CHECK(orig_fwd_fee.not_null());
    if (env.fwd_fee_remaining > orig_fwd_fee) {
      return reject_query("inbound internal message with hash "s + key.to_hex(256) + " has remaining forwarding fee " +
                          td::dec_string(env.fwd_fee_remaining) + " larger than the original (total) forwarding fee " +
                          td::dec_string(orig_fwd_fee));
    }
  }

  if (transaction.not_null()) {
    // check that the transaction reference is valid, and that it points to a Transaction which indeed processes this input message
    if (!is_valid_transaction_ref(transaction)) {
      return reject_query(
          "InMsg corresponding to inbound message with key "s + key.to_hex(256) +
          " contains an invalid Transaction reference (transaction not in the block's transaction list)");
    }
    if (!block::is_transaction_in_msg(transaction, msg)) {
      return reject_query("InMsg corresponding to inbound message with key "s + key.to_hex(256) +
                          " refers to transaction that does not process this inbound message");
    }
    ton::StdSmcAddress trans_addr;
    ton::LogicalTime trans_lt;
    CHECK(block::get_transaction_id(transaction, trans_addr, trans_lt));
    if (addr != trans_addr) {
      block::gen::t_InMsg.print(std::cerr, *in_msg);
      return reject_query(PSTRING() << "InMsg corresponding to inbound message with hash " << key.to_hex(256)
                                    << " and destination address " << addr.to_hex()
                                    << " claims that the message is processed by transaction " << trans_lt
                                    << " of another account " << trans_addr.to_hex());
    }
  }

  if (tag == block::gen::InMsg::msg_import_ext) {
    return true;  // nothing to check more for external messages
  }

  Ref<vm::Cell> out_msg_env;
  Ref<vm::Cell> reimport;
  bool tr_req = false;

  // continue checking inbound message
  switch (tag) {
    case block::gen::InMsg::msg_import_imm: {
      // msg_import_imm$011 in_msg:^MsgEnvelope transaction:^Transaction fwd_fee:Grams
      // importing and processing an internal message generated in this very block
      if (cur_prefix != dest_prefix) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_imm$011, but its current address " + cur_prefix.to_str() +
                            " is somehow distinct from its final destination " + dest_prefix.to_str());
      }
      CHECK(transaction.not_null());
      // check that the message has been created in this very block
      if (!shard_contains(shard_, src_prefix)) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_imm$011, but its source address " + src_prefix.to_str() +
                            " does not belong to this shard");
      }
      block::gen::OutMsg::Record_msg_export_imm out_msg;
      if (tlb::csr_unpack_safe(out_msg_dict_->lookup(key, 256), out_msg)) {
        out_msg_env = std::move(out_msg.out_msg);
        reimport = std::move(out_msg.reimport);
      } else if (!is_special_in_msg(*in_msg)) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_imm$011, but the corresponding OutMsg does not exist, or is not a valid "
                            "msg_export_imm$010");
      }
      // fwd_fee must be equal to the fwd_fee_remaining of this MsgEnvelope
      if (*fwd_fee != *env.fwd_fee_remaining) {
        return reject_query("msg_import_imm$011 InMsg with hash "s + key.to_hex(256) +
                            " is invalid because its collected fwd_fee=" + td::dec_string(fwd_fee) +
                            " is not equal to fwd_fee_remaining=" + td::dec_string(env.fwd_fee_remaining) +
                            " of this message (envelope)");
      }
      // ...
      break;
    }
    case block::gen::InMsg::msg_import_fin: {
      // msg_import_fin$100 in_msg:^MsgEnvelope transaction:^Transaction fwd_fee:Grams
      // importing and processing an internal message with destination in this shard
      CHECK(transaction.not_null());
      CHECK(shard_contains(shard_, next_prefix));
      if (shard_contains(shard_, cur_prefix)) {
        // we imported this message from our shard!
        block::gen::OutMsg::Record_msg_export_deq_imm out_msg;
        if (!tlb::csr_unpack_safe(out_msg_dict_->lookup(key, 256), out_msg)) {
          return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                              " is a msg_import_fin$100 with current address " + cur_prefix.to_str() +
                              "... already in our shard, but the corresponding OutMsg does not exist, or is not a "
                              "valid msg_export_deq_imm$100");
        }
        out_msg_env = std::move(out_msg.out_msg);
        reimport = std::move(out_msg.reimport);
      } else {
        CHECK(cur_prefix != next_prefix);
        // check that the message was present in the output queue of a neighbor, and that it has not been processed before
        if (!check_imported_message(msg_env)) {
          return false;
        }
      }
      // ...
      // fwd_fee must be equal to the fwd_fee_remaining of this MsgEnvelope
      if (*fwd_fee != *env.fwd_fee_remaining) {
        return reject_query("msg_import_imm$011 InMsg with hash "s + key.to_hex(256) +
                            " is invalid because its collected fwd_fee=" + td::dec_string(fwd_fee) +
                            " is not equal to fwd_fee_remaining=" + td::dec_string(env.fwd_fee_remaining) +
                            " of this message (envelope)");
      }
      // ...
      break;
    }
    case block::gen::InMsg::msg_import_tr: {
      // msg_import_tr$101 in_msg:^MsgEnvelope out_msg:^MsgEnvelope transit_fee:Grams
      // importing and relaying a (transit) internal message with destination outside this shard
      if (cur_prefix == dest_prefix) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_tr$101 (a transit message), but its current address " +
                            cur_prefix.to_str() + " is already equal to its final destination");
      }
      CHECK(transaction.is_null());
      CHECK(cur_prefix != next_prefix);
      auto out_msg_cs = out_msg_dict_->lookup(key, 256);
      if (out_msg_cs.is_null()) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_tr$101 (transit message), but the corresponding OutMsg does not exist");
      }
      if (shard_contains(shard_, cur_prefix)) {
        // we imported this message from our shard!
        // (very rare situation possible only after merge)
        tr_req = true;
        block::gen::OutMsg::Record_msg_export_tr_req out_msg;
        if (!tlb::csr_unpack_safe(out_msg_cs, out_msg)) {
          return reject_query(
              "inbound internal message with hash "s + key.to_hex(256) +
              " is a msg_import_tr$101 (transit message) with current address " + cur_prefix.to_str() +
              "... already in our shard, but the corresponding OutMsg is not a valid msg_export_tr_req$111");
        }
        out_msg_env = std::move(out_msg.out_msg);
        reimport = std::move(out_msg.imported);
      } else {
        block::gen::OutMsg::Record_msg_export_tr out_msg;
        if (!tlb::csr_unpack_safe(out_msg_cs, out_msg)) {
          return reject_query(
              "inbound internal message with hash "s + key.to_hex(256) +
              " is a msg_import_tr$101 (transit message) with current address " + cur_prefix.to_str() +
              "... outside of our shard, but the corresponding OutMsg is not a valid msg_export_tr$011");
        }
        out_msg_env = std::move(out_msg.out_msg);
        reimport = std::move(out_msg.imported);
        // check that the message was present in the output queue of a neighbor, and that it has not been processed before
        if (!check_imported_message(msg_env)) {
          return false;
        }
      }
      // perform hypercube routing for this transit message
      auto route_info = block::perform_hypercube_routing(next_prefix, dest_prefix, shard_);
      if ((unsigned)route_info.first > 96 || (unsigned)route_info.second > 96) {
        return reject_query("cannot perform (check) hypercube routing for transit inbound message with hash "s +
                            key.to_hex(256) + ": src=" + src_prefix.to_str() + " cur=" + cur_prefix.to_str() +
                            " next=" + next_prefix.to_str() + " dest=" + dest_prefix.to_str() + "; our shard is " +
                            shard_.to_str());
      }
      auto new_cur_prefix = block::interpolate_addr(next_prefix, dest_prefix, route_info.first);
      auto new_next_prefix = block::interpolate_addr(next_prefix, dest_prefix, route_info.second);
      // unpack out_msg:^MsgEnvelope from msg_import_tr
      block::tlb::MsgEnvelope::Record_std tr_env;
      if (!tlb::unpack_cell(tr_msg_env, tr_env)) {
        return reject_query("InMsg for transit message with hash "s + key.to_hex(256) +
                            " refers to an invalid rewritten message envelope");
      }
      // the rewritten transit message envelope must contain the same message
      if (tr_env.msg->get_hash() != msg->get_hash()) {
        return reject_query("InMsg for transit message with hash "s + key.to_hex(256) +
                            " refers to a rewritten message envelope containing another message");
      }
      // check that the message has been routed according to hypercube routing
      auto tr_cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, tr_env.cur_addr);
      auto tr_next_prefix = block::interpolate_addr(src_prefix, dest_prefix, tr_env.next_addr);
      if (tr_cur_prefix != new_cur_prefix || tr_next_prefix != new_next_prefix) {
        return reject_query("InMsg for transit message with hash "s + key.to_hex(256) +
                            " tells us that it has been adjusted to current address " + tr_cur_prefix.to_str() +
                            "... and hext hop address " + tr_next_prefix.to_str() +
                            " while the correct values dictated by hypercube routing are " + new_cur_prefix.to_str() +
                            "... and " + new_next_prefix.to_str() + "...");
      }
      // check that the collected transit fee with new fwd_fee_remaining equal the original fwd_fee_remaining
      // (correctness of fwd_fee itself will be checked later)
      if (tr_env.fwd_fee_remaining > orig_fwd_fee || *(tr_env.fwd_fee_remaining + fwd_fee) != *env.fwd_fee_remaining) {
        return reject_query("InMsg for transit message with hash "s + key.to_hex(256) + " declares transit fees of " +
                            td::dec_string(fwd_fee) + ", but fwd_fees_remaining has decreased from " +
                            td::dec_string(env.fwd_fee_remaining) + " to " + td::dec_string(tr_env.fwd_fee_remaining) +
                            " in transit");
      }
      if (tr_msg_env->get_hash() != out_msg_env->get_hash()) {
        return reject_query(
            "InMsg for transit message with hash "s + key.to_hex(256) +
            " contains rewritten MsgEnvelope different from that stored in corresponding OutMsgDescr (" +
            (tr_req ? "requeued" : "usual") + "transit)");
      }
      // check the amount of the transit fee
      td::RefInt256 transit_fee = action_phase_cfg_.fwd_std.get_next_part(env.fwd_fee_remaining);
      if (*transit_fee != *fwd_fee) {
        return reject_query("InMsg for transit message with hash "s + key.to_hex(256) +
                            " declared collected transit fees to be " + td::dec_string(fwd_fee) +
                            " (deducted from the remaining forwarding fees of " +
                            td::dec_string(env.fwd_fee_remaining) +
                            "), but we have computed another value of transit fees " + td::dec_string(transit_fee));
      }
      break;
    }
    default:
      return fatal_error(PSTRING() << "unknown InMsgTag " << tag);
  }

  if (reimport.not_null()) {
    // transit message: msg_export_tr + msg_import_tr
    // or message re-imported from this very shard
    // either msg_export_imm + msg_import_imm
    // or msg_export_deq_imm + msg_import_fin
    // or msg_export_tr_req + msg_import_tr (rarely, only after merge)
    // must have a corresponding OutMsg record
    if (!in_msg->contents_equal(vm::load_cell_slice(std::move(reimport)))) {
      return reject_query("OutMsg corresponding to reimport InMsg with hash "s + key.to_hex(256) +
                          " refers to a different reimport InMsg");
    }
    // for transit messages, OutMsg refers to the newly-created outbound messages (not to the re-imported old outbound message)
    if (tag != block::gen::InMsg::msg_import_tr && out_msg_env->get_hash() != msg_env->get_hash()) {
      return reject_query(
          "InMsg with hash "s + key.to_hex(256) +
          " is a reimport record, but the corresponding OutMsg exports a MsgEnvelope with a different hash");
    }
  }
  return true;
}

bool ValidateQuery::check_in_msg_descr() {
  LOG(INFO) << "checking inbound messages listed in InMsgDescr";
  try {
    CHECK(in_msg_dict_);
    if (!in_msg_dict_->validate_check_extra(
            [this](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
              CHECK(key_len == 256);
              return check_in_msg(key, std::move(value)) ||
                     reject_query("invalid InMsg with key (message hash) "s + key.to_hex(256) + " in the new block "s +
                                  id_.to_str());
            })) {
      return reject_query("invalid InMsgDescr dictionary in the new block "s + id_.to_str());
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid InMsgDescr dictionary: "s + err.get_msg());
  }
  return true;
}

bool ValidateQuery::check_out_msg(td::ConstBitPtr key, Ref<vm::CellSlice> out_msg) {
  LOG(DEBUG) << "checking OutMsg with key " << key.to_hex(256);
  CHECK(out_msg.not_null());
  int tag = block::gen::t_OutMsg.get_tag(*out_msg);
  CHECK(tag >= 0);  // NB: the block has been already checked to be valid TL-B in try_validate()
  ton::StdSmcAddress addr;
  ton::WorkchainId wc;
  Ref<vm::CellSlice> src, dest;
  Ref<vm::Cell> transaction;
  Ref<vm::Cell> msg, msg_env, tr_msg_env, reimport;
  td::Bits256 msg_env_hash;
  // msg_envelope#4 cur_addr:IntermediateAddress next_addr:IntermediateAddress fwd_fee_remaining:Grams msg:^(Message Any) = MsgEnvelope;
  block::tlb::MsgEnvelope::Record_std env;
  // int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
  //   src:MsgAddressInt dest:MsgAddressInt
  //   value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams
  //   created_lt:uint64 created_at:uint32 = CommonMsgInfo;
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  ton::AccountIdPrefixFull src_prefix, dest_prefix, cur_prefix, next_prefix;
  td::RefInt256 fwd_fee, orig_fwd_fee;
  ton::LogicalTime import_lt = ~0ULL;
  unsigned long long created_lt = 0;
  int mode = 0, in_tag = -2;
  bool is_short = false;
  // initial checks and unpack
  switch (tag) {
    case block::gen::OutMsg::msg_export_ext: {
      // msg_export_ext$000 msg:^(Message Any) transaction:^Transaction = OutMsg;
      // exporting an outbound external message
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info_ext;
      vm::CellSlice cs{*out_msg};
      CHECK(block::gen::t_OutMsg.unpack_msg_export_ext(cs, msg, transaction));
      if (msg->get_hash().as_bitslice() != key) {
        return reject_query("OutMsg with key "s + key.to_hex(256) + " refers to a message with different hash " +
                            msg->get_hash().to_hex());
      }
      if (!tlb::unpack_cell_inexact(msg, info_ext)) {
        return reject_query("OutMsg with key "s + key.to_hex(256) +
                            " is a msg_export_ext$000, but it does not refer to an outbound external message");
      }
      src_prefix = block::tlb::t_MsgAddressInt.get_prefix(info_ext.src);
      if (!src_prefix.is_valid()) {
        return reject_query("source of outbound external message with hash "s + key.to_hex(256) +
                            " is an invalid blockchain address");
      }
      if (!ton::shard_contains(shard_, src_prefix)) {
        return reject_query("outbound external message with hash "s + key.to_hex(256) + " has source address " +
                            src_prefix.to_str() + "... not in this shard");
      }
      src = std::move(info_ext.src);
      if (!block::tlb::t_MsgAddressInt.extract_std_address(src, wc, addr)) {
        return reject_query("cannot unpack source address of outbound external message with hash "s + key.to_hex(256));
      }
      break;
    }
    case block::gen::OutMsg::msg_export_imm: {
      block::gen::OutMsg::Record_msg_export_imm out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env));
      transaction = std::move(out.transaction);
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      reimport = std::move(out.reimport);
      in_tag = block::gen::InMsg::msg_import_imm;
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_new: {
      block::gen::OutMsg::Record_msg_export_new out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env) &&
            block::tlb::t_MsgEnvelope.get_created_lt(vm::load_cell_slice(out.out_msg), created_lt));
      transaction = std::move(out.transaction);
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      mode = 2;  // added to OutMsgQueue
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_tr: {
      block::gen::OutMsg::Record_msg_export_tr out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env));
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      reimport = std::move(out.imported);
      in_tag = block::gen::InMsg::msg_import_tr;
      mode = 2;  // added to OutMsgQueue
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_deq: {
      block::gen::OutMsg::Record_msg_export_deq out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env));
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      import_lt = out.import_block_lt;
      mode = 1;  // removed from OutMsgQueue
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_deq_short: {
      block::gen::OutMsg::Record_msg_export_deq_short out;
      CHECK(tlb::csr_unpack(out_msg, out));
      msg_env_hash = out.msg_env_hash;
      next_prefix.workchain = out.next_workchain;
      next_prefix.account_id_prefix = out.next_addr_pfx;
      import_lt = out.import_block_lt;
      is_short = true;
      mode = 1;  // removed from OutMsgQueue
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_tr_req: {
      block::gen::OutMsg::Record_msg_export_tr_req out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env));
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      reimport = std::move(out.imported);
      in_tag = block::gen::InMsg::msg_import_tr;
      mode = 3;  // removed from OutMsgQueue, and then added
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_deq_imm: {
      block::gen::OutMsg::Record_msg_export_deq_imm out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env));
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      reimport = std::move(out.reimport);
      in_tag = block::gen::InMsg::msg_import_fin;
      mode = 1;  // removed from OutMsgQueue (and processed)
      // ...
      break;
    }
    default:
      return reject_query(PSTRING() << "OutMsg with key (message hash) " << key.to_hex(256) << " has an unknown tag "
                                    << tag);
  }
  if (msg_env.not_null()) {
    msg_env_hash = msg_env->get_hash().bits();
  }

  // common checks for all (non-external) outbound messages
  if (!is_short) {
    CHECK(msg.not_null());
    if (msg->get_hash().as_bitslice() != key) {
      return reject_query("OutMsg with key "s + key.to_hex(256) + " refers to a message with different hash " +
                          msg->get_hash().to_hex());
    }
  }

  if (is_short) {
    // nothing to check here for msg_export_deq_short ?
  } else if (tag != block::gen::OutMsg::msg_export_ext) {
    // unpack int_msg_info$0 ... = CommonMsgInfo, especially message addresses
    if (!tlb::unpack_cell_inexact(msg, info)) {
      return reject_query("OutMsg with key "s + key.to_hex(256) +
                          " is not a msg_export_ext$000, but it does not refer to an internal message");
    }
    // extract source, current, next hop and destination address prefixes
    if (!block::tlb::t_MsgAddressInt.get_prefix_to(info.src, src_prefix)) {
      return reject_query("source of outbound internal message with hash "s + key.to_hex(256) +
                          " is an invalid blockchain address");
    }
    if (!block::tlb::t_MsgAddressInt.get_prefix_to(info.dest, dest_prefix)) {
      return reject_query("destination of outbound internal message with hash "s + key.to_hex(256) +
                          " is an invalid blockchain address");
    }
    cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
    next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
    if (!(cur_prefix.is_valid() && next_prefix.is_valid())) {
      return reject_query("cannot compute current and next hop addresses of outbound internal message with hash "s +
                          key.to_hex(256));
    }
    // check that next hop is nearer to the destination than the current address
    if (count_matching_bits(dest_prefix, next_prefix) < count_matching_bits(dest_prefix, cur_prefix)) {
      return reject_query("next hop address "s + next_prefix.to_str() + "... of outbound internal message with hash " +
                          key.to_hex(256) + " is further from its destination " + dest_prefix.to_str() +
                          "... than its current address " + cur_prefix.to_str() + "...");
    }
    // current address must belong to this shard (otherwise we should never had exported this message)
    if (!ton::shard_contains(shard_, cur_prefix)) {
      return reject_query("current address "s + cur_prefix.to_str() + "... of outbound internal message with hash " +
                          key.to_hex(256) + " does not belong to the current block's shard " + shard_.to_str());
    }
    // next hop may coincide with current address only if destination is already reached
    if (next_prefix == cur_prefix && cur_prefix != dest_prefix) {
      return reject_query(
          "next hop address "s + next_prefix.to_str() + "... of outbound internal message with hash " +
          key.to_hex(256) +
          " coincides with its current address, but this message has not reached its final destination " +
          dest_prefix.to_str() + "... yet");
    }
    // if a message is created by a transaction, it must have source inside the current shard
    if (transaction.not_null() && !ton::shard_contains(shard_, src_prefix)) {
      return reject_query("outbound internal message with hash "s + key.to_hex(256) + " has source address " +
                          src_prefix.to_str() +
                          "... not in this shard, but it has been created here by a Transaction nonetheless");
    }
    src = std::move(info.src);
    dest = std::move(info.dest);
    // unpack complete source address if it is inside this shard
    if (transaction.not_null() && !block::tlb::t_MsgAddressInt.extract_std_address(src, wc, addr)) {
      return reject_query("cannot unpack source address of outbound internal message with hash "s + key.to_hex(256) +
                          " created in this shard");
    }
    // unpack original forwarding fee
    orig_fwd_fee = block::tlb::t_Grams.as_integer(info.fwd_fee);
    CHECK(orig_fwd_fee.not_null());
    if (env.fwd_fee_remaining > orig_fwd_fee) {
      return reject_query("outbound internal message with hash "s + key.to_hex(256) + " has remaining forwarding fee " +
                          td::dec_string(env.fwd_fee_remaining) + " larger than the original (total) forwarding fee " +
                          td::dec_string(orig_fwd_fee));
    }
  }

  if (transaction.not_null()) {
    // check that the transaction reference is valid, and that it points to a Transaction which indeed creates this outbound internal message
    if (!is_valid_transaction_ref(transaction)) {
      return reject_query(
          "OutMsg corresponding to outbound message with key "s + key.to_hex(256) +
          " contains an invalid Transaction reference (transaction not in the block's transaction list)");
    }
    if (!block::is_transaction_out_msg(transaction, msg)) {
      return reject_query("OutMsg corresponding to outbound message with key "s + key.to_hex(256) +
                          " refers to transaction that does not create this outbound message");
    }
    ton::StdSmcAddress trans_addr;
    ton::LogicalTime trans_lt;
    CHECK(block::get_transaction_id(transaction, trans_addr, trans_lt));
    if (addr != trans_addr) {
      block::gen::t_OutMsg.print(std::cerr, *out_msg);
      return reject_query(PSTRING() << "OutMsg corresponding to outbound message with hash " << key.to_hex(256)
                                    << " and source address " << addr.to_hex()
                                    << " claims that the message was created by transaction " << trans_lt
                                    << " of another account " << trans_addr.to_hex());
    }
    // LOG(DEBUG) << "OutMsg " << key.to_hex(256) + " is indeed a valid outbound message of transaction " << trans_lt
    //           << " of " << trans_addr.to_hex();
  }

  if (tag == block::gen::OutMsg::msg_export_ext) {
    return true;  // nothing to check more for external messages
  }

  // check the OutMsgQueue update effected by this OutMsg
  td::BitArray<32 + 64 + 256> q_key;
  q_key.bits().store_int(next_prefix.workchain, 32);
  (q_key.bits() + 32).store_int(next_prefix.account_id_prefix, 64);
  (q_key.bits() + 96).copy_from(key, 256);
  auto q_entry = ns_.out_msg_queue_->lookup(q_key);
  auto old_q_entry = ps_.out_msg_queue_->lookup(q_key);
  if (old_q_entry.not_null() && q_entry.not_null()) {
    return reject_query("OutMsg with key (message hash) "s + key.to_hex(256) +
                        " should have removed or added OutMsgQueue entry with key " + q_key.to_hex() +
                        ", but it is present both in the old and in the new output queues");
  }
  if (old_q_entry.is_null() && q_entry.is_null() && mode) {
    return reject_query("OutMsg with key (message hash) "s + key.to_hex(256) +
                        " should have removed or added OutMsgQueue entry with key " + q_key.to_hex() +
                        ", but it is absent both from the old and from the new output queues");
  }
  if (!mode && (old_q_entry.not_null() || q_entry.not_null())) {
    return reject_query("OutMsg with key (message hash) "s + key.to_hex(256) +
                        " is a msg_export_imm$010, so the OutMsgQueue entry with key " + q_key.to_hex() +
                        " should never be created, but it is present in either the old or the new output queue");
  }
  // NB: if mode!=0, the OutMsgQueue entry has been changed, so we have already checked some conditions in precheck_one_message_queue_update()
  if (mode & 2) {
    if (q_entry.is_null()) {
      return reject_query("OutMsg with key "s + key.to_hex(256) +
                          " was expected to create OutMsgQueue entry with key " + q_key.to_hex() + " but it did not");
    }
    if (msg_env_hash != q_entry->prefetch_ref()->get_hash().bits()) {
      return reject_query("OutMsg with key "s + key.to_hex(256) + " has created OutMsgQueue entry with key " +
                          q_key.to_hex() + " containing a different MsgEnvelope");
    }
    // ...
  } else if (mode & 1) {
    if (old_q_entry.is_null()) {
      return reject_query("OutMsg with key "s + key.to_hex(256) +
                          " was expected to remove OutMsgQueue entry with key " + q_key.to_hex() +
                          " but it did not exist in the old queue");
    }
    if (msg_env_hash != old_q_entry->prefetch_ref()->get_hash().bits()) {
      return reject_query("OutMsg with key "s + key.to_hex(256) + " has dequeued OutMsgQueue entry with key " +
                          q_key.to_hex() + " containing a different MsgEnvelope");
    }
    // ...
  }

  // check reimport:^InMsg
  if (reimport.not_null()) {
    // transit message: msg_export_tr + msg_import_tr
    // or message re-imported from this very shard
    // either msg_export_imm + msg_import_imm
    // or msg_export_deq_imm + msg_import_fin (rarely)
    // or msg_export_tr_req + msg_import_tr (rarely)
    // (the last two cases possible only after merge)
    //
    // check that reimport is a valid InMsg registered in InMsgDescr
    auto in = in_msg_dict_->lookup(key, 256);
    if (in.is_null()) {
      return reject_query("OutMsg with key "s + key.to_hex(256) +
                          " refers to a (re)import InMsg, but there is no InMsg with such a key");
    }
    if (!in->contents_equal(vm::load_cell_slice(reimport))) {
      return reject_query(
          "OutMsg with key "s + key.to_hex(256) +
          " refers to a (re)import InMsg, but the actual InMsg with this key is different from the one referred to");
    }
    // NB: in check_in_msg(), we have already checked that all InMsg have correct keys (equal to the hash of the imported message), so the imported message is equal to the exported message (they have the same hash)
    // have only to check the envelope
    int i_tag = block::gen::t_InMsg.get_tag(*in);
    if (i_tag < 0 || i_tag != in_tag) {
      return reject_query("OutMsg with key "s + key.to_hex(256) +
                          " refers to a (re)import InMsg, which is not one of msg_import_imm, msg_import_fin or "
                          "msg_import_tr as expected");
    }
  }

  // ...
  switch (tag) {
    case block::gen::OutMsg::msg_export_imm: {
      block::gen::InMsg::Record_msg_import_imm in;
      block::tlb::MsgEnvelope::Record_std in_env;
      if (!(tlb::unpack_cell(reimport, in) && tlb::unpack_cell(in.in_msg, in_env))) {
        return reject_query(
            "cannot unpack msg_import_imm InMsg record corresponding to msg_export_imm OutMsg record with key "s +
            key.to_hex(256));
      }
      if (in.in_msg->get_hash() != msg_env->get_hash()) {
        return reject_query("msg_import_imm InMsg record corresponding to msg_export_imm OutMsg record with key "s +
                            key.to_hex(256) + " re-imported a different MsgEnvelope");
      }
      if (!shard_contains(shard_, dest_prefix)) {
        return reject_query("msg_export_imm OutMsg record with key "s + key.to_hex(256) +
                            " refers to a message with destination " + dest_prefix.to_str() + " outside this shard");
      }
      if (cur_prefix != dest_prefix || next_prefix != dest_prefix) {
        return reject_query("msg_export_imm OutMsg record with key "s + key.to_hex(256) +
                            " refers to a message that has not been routed to its final destination");
      }
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_new: {
      // perform hypercube routing for this new message
      auto route_info = block::perform_hypercube_routing(src_prefix, dest_prefix, shard_);
      if ((unsigned)route_info.first > 96 || (unsigned)route_info.second > 96) {
        return reject_query("cannot perform (check) hypercube routing for new outbound message with hash "s +
                            key.to_hex(256));
      }
      auto new_cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, route_info.first);
      auto new_next_prefix = block::interpolate_addr(src_prefix, dest_prefix, route_info.second);
      if (cur_prefix != new_cur_prefix || next_prefix != new_next_prefix) {
        return reject_query("OutMsg for new message with hash "s + key.to_hex(256) +
                            " tells us that it has been routed to current address " + cur_prefix.to_str() +
                            "... and hext hop address " + next_prefix.to_str() +
                            " while the correct values dictated by hypercube routing are " + new_cur_prefix.to_str() +
                            "... and " + new_next_prefix.to_str() + "...");
      }
      CHECK(shard_contains(shard_, src_prefix));
      if (shard_contains(shard_, dest_prefix)) {
        // LOG(DEBUG) << "(THIS) src=" << src_prefix.to_str() << " cur=" << cur_prefix.to_str() << " next=" << next_prefix.to_str() << " dest=" << dest_prefix.to_str() << " route_info=(" << route_info.first << "," << route_info.second << ")";
        CHECK(cur_prefix == dest_prefix);
        CHECK(next_prefix == dest_prefix);
        update_min_enqueued_lt_hash(created_lt, msg->get_hash().bits());
      } else {
        // sanity check of the implementation of hypercube routing
        // LOG(DEBUG) << "(THAT) src=" << src_prefix.to_str() << " cur=" << cur_prefix.to_str() << " next=" << next_prefix.to_str() << " dest=" << dest_prefix.to_str();
        CHECK(shard_contains(shard_, cur_prefix));
        CHECK(!shard_contains(shard_, next_prefix));
      }
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_tr: {
      block::gen::InMsg::Record_msg_import_tr in;
      block::tlb::MsgEnvelope::Record_std in_env;
      if (!(tlb::unpack_cell(reimport, in) && tlb::unpack_cell(in.in_msg, in_env))) {
        return reject_query(
            "cannot unpack msg_import_tr InMsg record corresponding to msg_export_tr OutMsg record with key "s +
            key.to_hex(256));
      }
      CHECK(in_env.msg->get_hash() == msg->get_hash());
      auto in_cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, in_env.cur_addr);
      auto in_next_prefix = block::interpolate_addr(src_prefix, dest_prefix, in_env.next_addr);
      if (shard_contains(shard_, in_cur_prefix)) {
        return reject_query("msg_export_tr OutMsg record with key "s + key.to_hex(256) +
                            " corresponds to msg_import_tr InMsg record with current imported message address " +
                            in_cur_prefix.to_str() +
                            " inside the current shard (msg_export_tr_req should have been used instead)");
      }
      // we have already checked correctness of hypercube routing in InMsg::msg_import_tr case of check_in_msg()
      CHECK(shard_contains(shard_, in_next_prefix));
      CHECK(shard_contains(shard_, cur_prefix));
      CHECK(!shard_contains(shard_, next_prefix));
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_deq:
    case block::gen::OutMsg::msg_export_deq_short: {
      // check that the message has been indeed processed by a neighbor
      CHECK(old_q_entry.not_null());
      block::EnqueuedMsgDescr enq_msg_descr;
      if (!enq_msg_descr.unpack(old_q_entry.write())) {  // unpack EnqueuedMsg
        return reject_query(
            "cannot unpack old OutMsgQueue entry corresponding to msg_export_deq OutMsg entry with key "s +
            key.to_hex(256));
      }
      bool delivered = false;
      ton::LogicalTime deliver_lt = 0;
      for (const auto& neighbor : neighbors_) {
        // could look up neighbor with shard containing enq_msg_descr.next_prefix more efficiently
        // (instead of checking all neighbors)
        if (!neighbor.is_disabled() && neighbor.processed_upto->already_processed(enq_msg_descr)) {
          delivered = true;
          deliver_lt = neighbor.end_lt();
          break;
        }
      }
      if (!delivered) {
        return reject_query("msg_export_deq OutMsg entry with key "s + key.to_hex(256) +
                            " attempts to dequeue a message with next hop " + next_prefix.to_str() +
                            " that has not been yet processed by the corresponding neighbor");
      }
      if (deliver_lt != import_lt) {
        LOG(WARNING) << "msg_export_deq OutMsg entry with key " << key.to_hex(256)
                     << " claims the dequeued message with next hop "
                     << next_prefix.to_str() + " has been delivered in block with end_lt=" << import_lt
                     << " while the correct value is " << deliver_lt;
      }
      break;
    }
    case block::gen::OutMsg::msg_export_tr_req: {
      block::gen::InMsg::Record_msg_import_tr in;
      block::tlb::MsgEnvelope::Record_std in_env;
      if (!(tlb::unpack_cell(reimport, in) && tlb::unpack_cell(in.in_msg, in_env))) {
        return reject_query(
            "cannot unpack msg_import_tr InMsg record corresponding to msg_export_tr_req OutMsg record with key "s +
            key.to_hex(256));
      }
      CHECK(in_env.msg->get_hash() == msg->get_hash());
      auto in_cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, in_env.cur_addr);
      auto in_next_prefix = block::interpolate_addr(src_prefix, dest_prefix, in_env.next_addr);
      if (!shard_contains(shard_, in_cur_prefix)) {
        return reject_query("msg_export_tr_req OutMsg record with key "s + key.to_hex(256) +
                            " corresponds to msg_import_tr InMsg record with current imported message address " +
                            in_cur_prefix.to_str() +
                            " outside the current shard (msg_export_tr should have been used instead, because there "
                            "was no re-queueing)");
      }
      // we have already checked correctness of hypercube routing in InMsg::msg_import_tr case of check_in_msg()
      CHECK(shard_contains(shard_, in_next_prefix));
      CHECK(shard_contains(shard_, cur_prefix));
      CHECK(!shard_contains(shard_, next_prefix));
      // so we have just to check that the rewritten message (envelope) has been enqueued
      // (already checked above for q_entry since mode = 3)
      // and that the original message (envelope) has been dequeued
      q_key.bits().store_int(in_next_prefix.workchain, 32);
      (q_key.bits() + 32).store_int(in_next_prefix.account_id_prefix, 64);
      q_entry = ns_.out_msg_queue_->lookup(q_key);
      old_q_entry = ps_.out_msg_queue_->lookup(q_key);
      if (old_q_entry.is_null()) {
        return reject_query("msg_export_tr_req OutMsg record with key "s + key.to_hex(256) +
                            " was expected to dequeue message from OutMsgQueue with key "s + q_key.to_hex() +
                            " but such a message is absent from the old OutMsgQueue");
      }
      if (q_entry.not_null()) {
        return reject_query("msg_export_tr_req OutMsg record with key "s + key.to_hex(256) +
                            " was expected to dequeue message from OutMsgQueue with key "s + q_key.to_hex() +
                            " but such a message is still present in the new OutMsgQueue");
      }
      block::EnqueuedMsgDescr enq_msg_descr;
      if (!enq_msg_descr.unpack(old_q_entry.write())) {  // unpack EnqueuedMsg
        return reject_query(
            "cannot unpack old OutMsgQueue entry corresponding to msg_export_tr_req OutMsg entry with key "s +
            key.to_hex(256));
      }
      if (enq_msg_descr.msg_env_->get_hash() != in.in_msg->get_hash()) {
        return reject_query("msg_import_tr InMsg entry corresponding to msg_export_tr_req OutMsg entry with key "s +
                            key.to_hex(256) +
                            " has re-imported a different MsgEnvelope from that present in the old OutMsgQueue");
      }
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_deq_imm: {
      block::gen::InMsg::Record_msg_import_fin in;
      block::tlb::MsgEnvelope::Record_std in_env;
      if (!(tlb::unpack_cell(reimport, in) && tlb::unpack_cell(in.in_msg, in_env))) {
        return reject_query(
            "cannot unpack msg_import_fin InMsg record corresponding to msg_export_deq_imm OutMsg record with key "s +
            key.to_hex(256));
      }
      if (in.in_msg->get_hash() != msg_env->get_hash()) {
        return reject_query("msg_import_fin InMsg record corresponding to msg_export_deq_imm OutMsg record with key "s +
                            key.to_hex(256) +
                            " somehow imported a different MsgEnvelope from that dequeued by msg_export_deq_imm");
      }
      if (!shard_contains(shard_, cur_prefix)) {
        return reject_query("msg_export_deq_imm OutMsg record with key "s + key.to_hex(256) +
                            " dequeued a MsgEnvelope with current address " + cur_prefix.to_str() +
                            "... outside current shard");
      }
      // we have already checked more conditions in check_in_msg() case msg_import_fin
      CHECK(shard_contains(shard_, next_prefix));  // sanity check
      CHECK(shard_contains(shard_, dest_prefix));  // sanity check
      // ...
      break;
    }
    default:
      return fatal_error(PSTRING() << "unknown OutMsg tag " << tag);
  }

  return true;
}

bool ValidateQuery::check_out_msg_descr() {
  LOG(INFO) << "checking outbound messages listed in OutMsgDescr";
  try {
    CHECK(out_msg_dict_);
    if (!out_msg_dict_->validate_check_extra(
            [this](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
              CHECK(key_len == 256);
              return check_out_msg(key, std::move(value)) ||
                     reject_query("invalid OutMsg with key "s + key.to_hex(256) + " in the new block "s + id_.to_str());
            })) {
      return reject_query("invalid OutMsgDescr dictionary in the new block "s + id_.to_str());
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid OutMsgDescr dictionary: "s + err.get_msg());
  }
  return true;
}

// compare to Collator::update_processed_upto()
bool ValidateQuery::check_processed_upto() {
  LOG(INFO) << "checking ProcessedInfo";
  CHECK(ps_.processed_upto_);
  CHECK(ns_.processed_upto_);
  if (!ns_.processed_upto_->is_reduced()) {
    return reject_query("new ProcessedInfo is not reduced (some entries completely cover other entries)");
  }
  bool ok = false;
  auto upd = ns_.processed_upto_->is_simple_update_of(*ps_.processed_upto_, ok);
  if (!ok) {
    return reject_query("new ProcessedInfo is not obtained from old ProcessedInfo by adding at most one new entry");
  }
  if (upd) {
    if (upd->shard != shard_.shard) {
      return reject_query("newly-added ProcessedInfo entry refers to shard "s +
                          ShardIdFull{workchain(), upd->shard}.to_str() + " distinct from the current shard " +
                          shard_.to_str());
    }
    auto ref_mc_seqno = is_masterchain() ? id_.seqno() : mc_seqno_;
    if (upd->mc_seqno != ref_mc_seqno) {
      return reject_query(PSTRING() << "newly-added ProcessedInfo entry refers to masterchain block " << upd->mc_seqno
                                    << " but the processed inbound message queue belongs to masterchain block "
                                    << ref_mc_seqno);
    }
    if (upd->last_inmsg_lt >= end_lt_) {
      return reject_query(PSTRING() << "newly-added ProcessedInfo entry claims that the last processed message has lt "
                                    << upd->last_inmsg_lt << " larger than this block's end lt " << end_lt_);
    }
    if (!upd->last_inmsg_lt) {
      return reject_query("newly-added ProcessedInfo entry claims that the last processed message has zero lt");
    }
    claimed_proc_lt_ = upd->last_inmsg_lt;
    claimed_proc_hash_ = upd->last_inmsg_hash;
  } else {
    claimed_proc_lt_ = 0;
    claimed_proc_hash_.set_zero();
  }
  LOG(INFO) << "ProcessedInfo claims to have processed all inbound messages up to (" << claimed_proc_lt_ << ","
            << claimed_proc_hash_.to_hex() << ")";
  if (claimed_proc_lt_ < proc_lt_ || (claimed_proc_lt_ == proc_lt_ && proc_lt_ && claimed_proc_hash_ < proc_hash_)) {
    return reject_query(PSTRING() << "the ProcessedInfo claims to have processed messages only upto ("
                                  << claimed_proc_lt_ << "," << claimed_proc_hash_.to_hex()
                                  << "), but there is a InMsg processing record for later message (" << proc_lt_ << ","
                                  << proc_hash_.to_hex());
  }
  if (min_enq_lt_ < claimed_proc_lt_ || (min_enq_lt_ == claimed_proc_lt_ && !(claimed_proc_hash_ < min_enq_hash_))) {
    return reject_query(PSTRING() << "the ProcessedInfo claims to have processed all messages upto ("
                                  << claimed_proc_lt_ << "," << claimed_proc_hash_.to_hex()
                                  << "), but there is a OutMsg enqueuing record for earlier message (" << min_enq_lt_
                                  << "," << min_enq_hash_.to_hex());
  }
  // ...
  return true;
}

// similar to Collator::process_inbound_message
bool ValidateQuery::check_neighbor_outbound_message(Ref<vm::CellSlice> enq_msg, ton::LogicalTime lt,
                                                    td::ConstBitPtr key, const block::McShardDescr& nb,
                                                    bool& unprocessed) {
  unprocessed = false;
  if (!block::gen::t_EnqueuedMsg.validate_csr(enq_msg)) {
    return reject_query("EnqueuedMsg with key "s + key.to_hex(352) + " in outbound queue of our neighbor " +
                        nb.blk_.to_str() + " failed to pass automated validity tests");
  }
  if (!block::tlb::t_EnqueuedMsg.validate_csr(enq_msg)) {
    return reject_query("EnqueuedMsg with key "s + key.to_hex(352) + " in outbound queue of our neighbor " +
                        nb.blk_.to_str() + " failed to pass hand-written validity tests");
  }
  block::EnqueuedMsgDescr enq;
  if (!enq.unpack(enq_msg.write())) {  // unpack EnqueuedMsg
    return reject_query("cannot unpack EnqueuedMsg with key "s + key.to_hex(352) +
                        " in outbound queue of our neighbor " + nb.blk_.to_str());
  }
  if (!enq.check_key(key)) {  // check key
    return reject_query("EnqueuedMsg with key "s + key.to_hex(352) + " in outbound queue of our neighbor " +
                        nb.blk_.to_str() + " has incorrect key for its contents and envelope");
  }
  if (enq.lt_ != lt) {
    return reject_query(PSTRING() << "EnqueuedMsg with key " << key.to_hex(352) << " in outbound queue of our neighbor "
                                  << nb.blk_.to_str() << " pretends to have been created at lt " << lt
                                  << " but its actual creation lt is " << enq.lt_);
  }
  CHECK(shard_contains(shard_, enq.next_prefix_));
  auto in_entry = in_msg_dict_->lookup(key + 96, 256);
  auto out_entry = out_msg_dict_->lookup(key + 96, 256);
  bool f0 = ps_.processed_upto_->already_processed(enq);
  bool f1 = ns_.processed_upto_->already_processed(enq);
  if (f0 && !f1) {
    return fatal_error(
        "a previously processed message has been un-processed (impossible situation after the validation of "
        "ProcessedInfo)");
  }
  if (f0) {
    // this message has been processed in a previous block of this shard
    // just check that we have not imported it once again
    if (in_entry.not_null()) {
      return reject_query("have an InMsg entry for processing again already processed EnqueuedMsg with key "s +
                          key.to_hex(352) + " of neighbor " + nb.blk_.to_str());
    }
    if (shard_contains(shard_, enq.cur_prefix_)) {
      // if this message comes from our own outbound queue, we must have dequeued it
      if (out_entry.is_null()) {
        return reject_query("our old outbound queue contains EnqueuedMsg with key "s + key.to_hex(352) +
                            " already processed by this shard, but there is no ext_message_deq OutMsg record for this "
                            "message in this block");
      }
      int tag = block::gen::t_OutMsg.get_tag(*out_entry);
      if (tag == block::gen::OutMsg::msg_export_deq_short) {
        block::gen::OutMsg::Record_msg_export_deq_short deq;
        if (!tlb::csr_unpack(std::move(out_entry), deq)) {
          return reject_query(
              "cannot unpack msg_export_deq_short OutMsg record for already processed EnqueuedMsg with key "s +
              key.to_hex(352) + " of old outbound queue");
        }
        if (deq.msg_env_hash != enq.msg_env_->get_hash().bits()) {
          return reject_query("unpack ext_message_deq OutMsg record for already processed EnqueuedMsg with key "s +
                              key.to_hex(352) + " of old outbound queue refers to MsgEnvelope with different hash " +
                              deq.msg_env_hash.to_hex());
        }
      } else {
        block::gen::OutMsg::Record_msg_export_deq deq;
        if (!tlb::csr_unpack(std::move(out_entry), deq)) {
          return reject_query(
              "cannot unpack msg_export_deq OutMsg record for already processed EnqueuedMsg with key "s +
              key.to_hex(352) + " of old outbound queue");
        }
        if (deq.out_msg->get_hash() != enq.msg_env_->get_hash()) {
          return reject_query("unpack ext_message_deq OutMsg record for already processed EnqueuedMsg with key "s +
                              key.to_hex(352) + " of old outbound queue contains a different MsgEnvelope");
        }
      }
    }
    // next check is incorrect after a merge, when ns_.processed_upto has > 1 entries
    // we effectively comment it out
    return true;
    // NB. we might have a non-trivial dequeueing out_entry with this message hash, but another envelope (for transit messages)
    // (so we cannot assert that out_entry is null)
    if (claimed_proc_lt_ && (claimed_proc_lt_ < lt || (claimed_proc_lt_ == lt && claimed_proc_hash_ < enq.hash_))) {
      LOG(WARNING) << "old processed_upto: " << ps_.processed_upto_->to_str();
      LOG(WARNING) << "new processed_upto: " << ns_.processed_upto_->to_str();
      return fatal_error(
          -669, PSTRING() << "internal inconsistency: new ProcessedInfo claims to have processed all messages up to ("
                          << claimed_proc_lt_ << "," << claimed_proc_hash_.to_hex()
                          << "), but we had somehow already processed a message (" << lt << "," << enq.hash_.to_hex()
                          << ") from OutMsgQueue of neighbor " << nb.blk_.to_str() << " key " << key.to_hex(352));
    }
    return true;
  }
  if (f1) {
    // this message must have been imported and processed in this very block
    // (because it is marked processed after this block, but not before)
    if (!claimed_proc_lt_ || claimed_proc_lt_ < lt || (claimed_proc_lt_ == lt && claimed_proc_hash_ < enq.hash_)) {
      return fatal_error(
          -669, PSTRING() << "internal inconsistency: new ProcessedInfo claims to have processed all messages up to ("
                          << claimed_proc_lt_ << "," << claimed_proc_hash_.to_hex()
                          << "), but we had somehow processed in this block a message (" << lt << ","
                          << enq.hash_.to_hex() << ") from OutMsgQueue of neighbor " << nb.blk_.to_str() << " key "
                          << key.to_hex(352));
    }
    // must have a msg_import_fin or msg_import_tr InMsg record
    if (in_entry.is_null()) {
      return reject_query("there is no InMsg entry for processing EnqueuedMsg with key "s + key.to_hex(352) +
                          " of neighbor " + nb.blk_.to_str() +
                          " which is claimed to be processed by new ProcessedInfo of this block");
    }
    int tag = block::gen::t_InMsg.get_tag(*in_entry);
    if (tag != block::gen::InMsg::msg_import_fin && tag != block::gen::InMsg::msg_import_tr) {
      return reject_query(
          "expected either a msg_import_fin or a msg_import_tr InMsg record for processing EnqueuedMsg with key "s +
          key.to_hex(352) + " of neighbor " + nb.blk_.to_str() +
          " which is claimed to be processed by new ProcessedInfo of this block");
    }
    if (in_entry->prefetch_ref()->get_hash() != enq.msg_env_->get_hash()) {
      return reject_query("InMsg record for processing EnqueuedMsg with key "s + key.to_hex(352) + " of neighbor " +
                          nb.blk_.to_str() +
                          " which is claimed to be processed by new ProcessedInfo of this block contains a reference "
                          "to a different MsgEnvelope");
    }
    // all other checks have been done while checking InMsgDescr
    return true;
  }
  unprocessed = true;
  // the message is left unprocessed in our virtual "inbound queue"
  // just a simple sanity check
  if (claimed_proc_lt_ && !(claimed_proc_lt_ < lt || (claimed_proc_lt_ == lt && claimed_proc_hash_ < enq.hash_))) {
    return fatal_error(
        -669, PSTRING() << "internal inconsistency: new ProcessedInfo claims to have processed all messages up to ("
                        << claimed_proc_lt_ << "," << claimed_proc_hash_.to_hex()
                        << "), but we somehow have not processed a message (" << lt << "," << enq.hash_.to_hex()
                        << ") from OutMsgQueue of neighbor " << nb.blk_.to_str() << " key " << key.to_hex(352));
  }
  return true;
}

bool ValidateQuery::check_in_queue() {
  block::OutputQueueMerger nb_out_msgs(shard_, neighbors_);
  while (!nb_out_msgs.is_eof()) {
    auto kv = nb_out_msgs.extract_cur();
    CHECK(kv && kv->msg.not_null());
    LOG(DEBUG) << "processing inbound message with (lt,hash)=(" << kv->lt << "," << kv->key.to_hex()
               << ") from neighbor #" << kv->source;
    if (verbosity > 3) {
      std::cerr << "inbound message: lt=" << kv->lt << " from=" << kv->source << " key=" << kv->key.to_hex() << " msg=";
      block::gen::t_EnqueuedMsg.print(std::cerr, *(kv->msg));
    }
    bool unprocessed = false;
    if (!check_neighbor_outbound_message(kv->msg, kv->lt, kv->key.cbits(), neighbors_.at(kv->source), unprocessed)) {
      if (verbosity > 1) {
        std::cerr << "invalid neighbor outbound message: lt=" << kv->lt << " from=" << kv->source
                  << " key=" << kv->key.to_hex() << " msg=";
        block::gen::t_EnqueuedMsg.print(std::cerr, *(kv->msg));
      }
      return reject_query("error processing outbound internal message "s + kv->key.to_hex() + " of neighbor " +
                          neighbors_.at(kv->source).blk_.to_str());
    }
    if (unprocessed) {
      inbound_queues_empty_ = false;
      return true;
    }
    nb_out_msgs.next();
  }
  inbound_queues_empty_ = true;
  return true;
}

// checks that all messages imported from our outbound queue into neighbor shards have been dequeued
// similar to Collator::out_msg_queue_cleanup()
// (but scans new outbound queue instead of the old)
bool ValidateQuery::check_delivered_dequeued() {
  LOG(INFO) << "scanning new outbound queue and checking delivery status of all messages";
  bool ok = false;
  for (const auto& nb : neighbors_) {
    if (!nb.is_disabled() && (!nb.processed_upto || !nb.processed_upto->can_check_processed())) {
      return fatal_error(-667, PSTRING() << "internal error: no info for checking processed messages from neighbor "
                                         << nb.blk_.to_str());
    }
  }
  return ns_.out_msg_queue_->check_for_each([&](Ref<vm::CellSlice> cs_ref, td::ConstBitPtr key, int n) -> bool {
    assert(n == 352);
    // LOG(DEBUG) << "key is " << key.to_hex(n);
    block::EnqueuedMsgDescr enq;
    unsigned long long created_lt;
    auto& cs = cs_ref.write();
    if (!(cs.fetch_ulong_bool(64, created_lt)  // augmentation
          && enq.unpack(cs)                    // unpack EnqueuedMsg
          && enq.check_key(key)                // check key
          && enq.lt_ == created_lt)) {
      return reject_query("cannot unpack EnqueuedMsg with key "s + key.to_hex(n) + " in the new OutMsgQueue");
    }
    LOG(DEBUG) << "scanning outbound message with (lt,hash)=(" << enq.lt_ << "," << enq.hash_.to_hex()
               << ") enqueued_lt=" << enq.enqueued_lt_;
    for (const auto& nb : neighbors_) {
      // could look up neighbor with shard containing enq_msg_descr.next_prefix more efficiently
      // (instead of checking all neighbors)
      if (!nb.is_disabled() && nb.processed_upto->already_processed(enq)) {
        // the message has been delivered but not removed from queue!
        LOG(WARNING) << "outbound queue not cleaned up completely (overfull block?): outbound message with (lt,hash)=("
                     << enq.lt_ << "," << enq.hash_.to_hex() << ") enqueued_lt=" << enq.enqueued_lt_
                     << " has been already delivered and processed by neighbor " << nb.blk_.to_str()
                     << " but it has not been dequeued in this block and it is still present in the new outbound queue";
        outq_cleanup_partial_ = true;
        ok = true;
        return false;  // skip scanning the remainder of the queue
      }
    }
    if (created_lt >= start_lt_) {
      LOG(DEBUG) << "stop scanning new outbound queue";
      ok = true;
      return false;
    }
    return true;
  }) || ok;
}

// similar to Collator::make_account_from()
std::unique_ptr<block::Account> ValidateQuery::make_account_from(td::ConstBitPtr addr, Ref<vm::CellSlice> account,
                                                                 Ref<vm::CellSlice> extra) {
  auto ptr = std::make_unique<block::Account>(workchain(), addr);
  if (account.is_null()) {
    if (!ptr->init_new(now_)) {
      return nullptr;
    }
  } else if (!ptr->unpack(std::move(account), std::move(extra), now_,
                          is_masterchain() && config_->is_special_smartcontract(addr))) {
    return nullptr;
  }
  ptr->block_lt = start_lt_;
  return ptr;
}

// similar to Collator::make_account()
std::unique_ptr<block::Account> ValidateQuery::unpack_account(td::ConstBitPtr addr) {
  auto dict_entry = ps_.account_dict_->lookup_extra(addr, 256);
  auto new_acc = make_account_from(addr, std::move(dict_entry.first), std::move(dict_entry.second));
  if (!new_acc) {
    reject_query("cannot load state of account "s + addr.to_hex(256) + " from previous shardchain state");
    return {};
  }
  if (!new_acc->belongs_to_shard(shard_)) {
    reject_query(PSTRING() << "old state of account " << addr.to_hex(256)
                           << " does not really belong to current shard");
    return {};
  }
  return new_acc;
}

bool ValidateQuery::check_one_transaction(block::Account& account, ton::LogicalTime lt, Ref<vm::Cell> trans_root,
                                          bool is_first, bool is_last) {
  LOG(DEBUG) << "checking transaction " << lt << " of account " << account.addr.to_hex();
  const StdSmcAddress& addr = account.addr;
  block::gen::Transaction::Record trans;
  block::gen::HASH_UPDATE::Record hash_upd;
  CHECK(tlb::unpack_cell(trans_root, trans) &&
        tlb::type_unpack_cell(std::move(trans.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd));
  auto in_msg_root = trans.r1.in_msg->prefetch_ref();
  bool external{false}, ihr_delivered{false}, need_credit_phase{false};
  // check input message
  block::CurrencyCollection money_imported(0), money_exported(0);
  if (in_msg_root.not_null()) {
    auto in_descr_cs = in_msg_dict_->lookup(in_msg_root->get_hash().as_bitslice());
    if (in_descr_cs.is_null()) {
      return reject_query(PSTRING() << "inbound message with hash " << in_msg_root->get_hash().to_hex()
                                    << " of transaction " << lt << " of account " << addr.to_hex()
                                    << " does not have a corresponding InMsg record");
    }
    auto tag = block::gen::t_InMsg.get_tag(*in_descr_cs);
    if (tag != block::gen::InMsg::msg_import_ext && tag != block::gen::InMsg::msg_import_fin &&
        tag != block::gen::InMsg::msg_import_imm && tag != block::gen::InMsg::msg_import_ihr) {
      return reject_query(PSTRING() << "inbound message with hash " << in_msg_root->get_hash().to_hex()
                                    << " of transaction " << lt << " of account " << addr.to_hex()
                                    << " has an invalid InMsg record (not one of msg_import_ext, msg_import_fin, "
                                       "msg_import_imm or msg_import_ihr)");
    }
    // once we know there is a InMsg with correct hash, we already know that it contains a message with this hash (by the verification of InMsg), so it is our message
    // have still to check its destination address and imported value
    // and that it refers to this transaction
    Ref<vm::CellSlice> dest;
    if (tag == block::gen::InMsg::msg_import_ext) {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      CHECK(tlb::unpack_cell_inexact(in_msg_root, info));
      dest = std::move(info.dest);
      external = true;
    } else {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      CHECK(tlb::unpack_cell_inexact(in_msg_root, info));
      if (info.created_lt >= lt) {
        return reject_query(PSTRING() << "transaction " << lt << " of " << addr.to_hex()
                                      << " processed inbound message created later at logical time "
                                      << info.created_lt);
      }
      if (info.created_lt != start_lt_ || !is_special_in_msg(*in_descr_cs)) {
        msg_proc_lt_.emplace_back(addr, lt, info.created_lt);
      }
      dest = std::move(info.dest);
      CHECK(money_imported.validate_unpack(info.value));
      ihr_delivered = (tag == block::gen::InMsg::msg_import_ihr);
      if (!ihr_delivered) {
        money_imported += block::tlb::t_Grams.as_integer(info.ihr_fee);
      }
      CHECK(money_imported.is_valid());
    }
    WorkchainId d_wc;
    StdSmcAddress d_addr;
    CHECK(block::tlb::t_MsgAddressInt.extract_std_address(dest, d_wc, d_addr));
    if (d_wc != workchain() || d_addr != addr) {
      return reject_query(PSTRING() << "inbound message of transaction " << lt << " of account " << addr.to_hex()
                                    << " has a different destination address " << d_wc << ":" << d_addr.to_hex());
    }
    auto in_msg_trans = in_descr_cs->prefetch_ref(1);  // trans:^Transaction
    CHECK(in_msg_trans.not_null());
    if (in_msg_trans->get_hash() != trans_root->get_hash()) {
      return reject_query(PSTRING() << "InMsg record for inbound message with hash " << in_msg_root->get_hash().to_hex()
                                    << " of transaction " << lt << " of account " << addr.to_hex()
                                    << " refers to a different processing transaction");
    }
  }
  // check output messages
  vm::Dictionary out_dict{trans.r1.out_msgs, 15};
  for (int i = 0; i < trans.outmsg_cnt; i++) {
    auto out_msg_root = out_dict.lookup_ref(td::BitArray<15>{i});
    CHECK(out_msg_root.not_null());  // we have pre-checked this
    auto out_descr_cs = out_msg_dict_->lookup(out_msg_root->get_hash().as_bitslice());
    if (out_descr_cs.is_null()) {
      return reject_query(PSTRING() << "outbound message #" << i + 1 << " with hash "
                                    << out_msg_root->get_hash().to_hex() << " of transaction " << lt << " of account "
                                    << addr.to_hex() << " does not have a corresponding OutMsg record");
    }
    auto tag = block::gen::t_OutMsg.get_tag(*out_descr_cs);
    if (tag != block::gen::OutMsg::msg_export_ext && tag != block::gen::OutMsg::msg_export_new &&
        tag != block::gen::OutMsg::msg_export_imm) {
      return reject_query(
          PSTRING() << "outbound message #" << i + 1 << " with hash " << out_msg_root->get_hash().to_hex()
                    << " of transaction " << lt << " of account " << addr.to_hex()
                    << " has an invalid OutMsg record (not one of msg_export_ext, msg_export_new or msg_export_imm)");
    }
    // once we know there is an OutMsg with correct hash, we already know that it contains a message with this hash (by the verification of OutMsg), so it is our message
    // have still to check its source address, lt and imported value
    // and that it refers to this transaction as its origin
    Ref<vm::CellSlice> src;
    if (tag == block::gen::OutMsg::msg_export_ext) {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info;
      CHECK(tlb::unpack_cell_inexact(out_msg_root, info));
      src = std::move(info.src);
    } else {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      CHECK(tlb::unpack_cell_inexact(out_msg_root, info));
      src = std::move(info.src);
      block::gen::MsgEnvelope::Record msg_env;
      CHECK(tlb::unpack_cell(out_descr_cs->prefetch_ref(), msg_env));
      // unpack exported message value (from this transaction)
      block::CurrencyCollection msg_export_value;
      CHECK(msg_export_value.unpack(info.value));
      msg_export_value += block::tlb::t_Grams.as_integer(info.ihr_fee);
      msg_export_value += block::tlb::t_Grams.as_integer(msg_env.fwd_fee_remaining);
      CHECK(msg_export_value.is_valid());
      money_exported += msg_export_value;
    }
    WorkchainId s_wc;
    StdSmcAddress ss_addr;  // s_addr is some macros in Windows
    CHECK(block::tlb::t_MsgAddressInt.extract_std_address(src, s_wc, ss_addr));
    if (s_wc != workchain() || ss_addr != addr) {
      return reject_query(PSTRING() << "outbound message #" << i + 1 << " of transaction " << lt << " of account "
                                    << addr.to_hex() << " has a different source address " << s_wc << ":"
                                    << ss_addr.to_hex());
    }
    auto out_msg_trans = out_descr_cs->prefetch_ref(1);  // trans:^Transaction
    CHECK(out_msg_trans.not_null());
    if (out_msg_trans->get_hash() != trans_root->get_hash()) {
      return reject_query(PSTRING() << "OutMsg record for outbound message #" << i + 1 << " with hash "
                                    << out_msg_root->get_hash().to_hex() << " of transaction " << lt << " of account "
                                    << addr.to_hex() << " refers to a different processing transaction");
    }
  }
  CHECK(money_exported.is_valid());
  // check general transaction data
  block::CurrencyCollection old_balance{account.get_balance()};
  auto td_cs = vm::load_cell_slice(trans.description);
  int tag = block::gen::t_TransactionDescr.get_tag(td_cs);
  CHECK(tag >= 0);  // we have already validated the serialization of all Transactions
  if (tag == block::gen::TransactionDescr::trans_merge_prepare ||
      tag == block::gen::TransactionDescr::trans_merge_install ||
      tag == block::gen::TransactionDescr::trans_split_prepare ||
      tag == block::gen::TransactionDescr::trans_split_install) {
    if (is_masterchain()) {
      return reject_query(
          PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                    << " is a split/merge prepare/install transaction, which is impossible in a masterchain block");
    }
    bool split = (tag == block::gen::TransactionDescr::trans_split_prepare ||
                  tag == block::gen::TransactionDescr::trans_split_install);
    if (split && !before_split_) {
      return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                    << " is a split prepare/install transaction, but this block is not before a split");
    }
    if (split && !is_last) {
      return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                    << " is a split prepare/install transaction, but it is not the last transaction "
                                       "for this account in this block");
    }
    if (!split && !after_merge_) {
      return reject_query(
          PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                    << " is a merge prepare/install transaction, but this block is not immediately after a merge");
    }
    if (!split && !is_first) {
      return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                    << " is a merge prepare/install transaction, but it is not the first transaction "
                                       "for this account in this block");
    }
    // check later a global configuration flag in config_.global_flags_
    // (for now, split/merge transactions are always globally disabled)
    return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                  << " is a split/merge prepare/install transaction, which are globally disabled");
  }
  if (tag == block::gen::TransactionDescr::trans_tick_tock) {
    if (!is_masterchain()) {
      return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                    << " is a tick-tock transaction, which is impossible outside a masterchain block");
    }
    if (!account.is_special) {
      return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                    << " is a tick-tock transaction, but this account is not listed as special");
    }
    bool is_tock = td_cs.prefetch_ulong(4) & 1;  // trans_tick_tock$001 is_tock:Bool ...
    if (!is_tock) {
      if (!is_first) {
        return reject_query(
            PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                      << " is a tick transaction, but this is not the first transaction of this account");
      }
      if (lt != start_lt_ + 1) {
        return reject_query(
            PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                      << " is a tick transaction, but its logical start time differs from block's start time "
                      << start_lt_ << " by more than one");
      }
      if (!account.tick) {
        return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                      << " is a tick transaction, but this account has not enabled tick transactions");
      }
    } else {
      if (!is_last) {
        return reject_query(
            PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                      << " is a tock transaction, but this is not the last transaction of this account");
      }
      if (!account.tock) {
        return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                      << " is a tock transaction, but this account has not enabled tock transactions");
      }
    }
  }
  if (is_first && is_masterchain() && account.is_special && account.tick &&
      (tag != block::gen::TransactionDescr::trans_tick_tock || (td_cs.prefetch_ulong(4) & 1)) && account.orig_status == block::Account::acc_active) {
    return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                  << " is the first transaction for this special tick account in this block, but the "
                                     "transaction is not a tick transaction");
  }
  if (is_last && is_masterchain() && account.is_special && account.tock &&
      (tag != block::gen::TransactionDescr::trans_tick_tock || !(td_cs.prefetch_ulong(4) & 1)) &&
      trans.end_status == block::gen::AccountStatus::acc_state_active) {
    return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                  << " is the last transaction for this special tock account in this block, but the "
                                     "transaction is not a tock transaction");
  }
  if (tag == block::gen::TransactionDescr::trans_storage && !is_first) {
    return reject_query(
        PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                  << " is a storage transaction, but it is not the first transaction for this account in this block");
  }
  // check that the original account state has correct hash
  CHECK(account.total_state.not_null());
  if (hash_upd.old_hash != account.total_state->get_hash().bits()) {
    return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                  << " claims that the original account state hash must be "
                                  << hash_upd.old_hash.to_hex() << " but the actual value is "
                                  << account.total_state->get_hash().to_hex());
  }
  // some type-specific checks
  int trans_type = block::Transaction::tr_none;
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord: {
      trans_type = block::Transaction::tr_ord;
      if (in_msg_root.is_null()) {
        return reject_query(PSTRING() << "ordinary transaction " << lt << " of account " << addr.to_hex()
                                      << " has no inbound message");
      }
      need_credit_phase = !external;
      break;
    }
    case block::gen::TransactionDescr::trans_storage: {
      trans_type = block::Transaction::tr_storage;
      if (in_msg_root.not_null()) {
        return reject_query(PSTRING() << "storage transaction " << lt << " of account " << addr.to_hex()
                                      << " has an inbound message");
      }
      if (trans.outmsg_cnt) {
        return reject_query(PSTRING() << "storage transaction " << lt << " of account " << addr.to_hex()
                                      << " has at least one outbound message");
      }
      // FIXME
      return reject_query(PSTRING() << "unable to verify storage transaction " << lt << " of account "
                                    << addr.to_hex());
      break;
    }
    case block::gen::TransactionDescr::trans_tick_tock: {
      bool is_tock = (td_cs.prefetch_ulong(4) & 1);
      trans_type = is_tock ? block::Transaction::tr_tock : block::Transaction::tr_tick;
      if (in_msg_root.not_null()) {
        return reject_query(PSTRING() << (is_tock ? "tock" : "tick") << " transaction " << lt << " of account "
                                      << addr.to_hex() << " has an inbound message");
      }
      break;
    }
    case block::gen::TransactionDescr::trans_merge_prepare: {
      trans_type = block::Transaction::tr_merge_prepare;
      if (in_msg_root.not_null()) {
        return reject_query(PSTRING() << "merge prepare transaction " << lt << " of account " << addr.to_hex()
                                      << " has an inbound message");
      }
      if (trans.outmsg_cnt != 1) {
        return reject_query(PSTRING() << "merge prepare transaction " << lt << " of account " << addr.to_hex()
                                      << " must have exactly one outbound message");
      }
      // FIXME
      return reject_query(PSTRING() << "unable to verify merge prepare transaction " << lt << " of account "
                                    << addr.to_hex());
      break;
    }
    case block::gen::TransactionDescr::trans_merge_install: {
      trans_type = block::Transaction::tr_merge_install;
      if (in_msg_root.is_null()) {
        return reject_query(PSTRING() << "merge install transaction " << lt << " of account " << addr.to_hex()
                                      << " has no inbound message");
      }
      need_credit_phase = true;
      // FIXME
      return reject_query(PSTRING() << "unable to verify merge install transaction " << lt << " of account "
                                    << addr.to_hex());
      break;
    }
    case block::gen::TransactionDescr::trans_split_prepare: {
      trans_type = block::Transaction::tr_split_prepare;
      if (in_msg_root.not_null()) {
        return reject_query(PSTRING() << "split prepare transaction " << lt << " of account " << addr.to_hex()
                                      << " has an inbound message");
      }
      if (trans.outmsg_cnt > 1) {
        return reject_query(PSTRING() << "split prepare transaction " << lt << " of account " << addr.to_hex()
                                      << " must have exactly one outbound message");
      }
      // FIXME
      return reject_query(PSTRING() << "unable to verify split prepare transaction " << lt << " of account "
                                    << addr.to_hex());
      break;
    }
    case block::gen::TransactionDescr::trans_split_install: {
      trans_type = block::Transaction::tr_split_install;
      if (in_msg_root.is_null()) {
        return reject_query(PSTRING() << "split install transaction " << lt << " of account " << addr.to_hex()
                                      << " has no inbound message");
      }
      // FIXME
      return reject_query(PSTRING() << "unable to verify split install transaction " << lt << " of account "
                                    << addr.to_hex());
      break;
    }
  }
  // ....
  // check transaction computation by re-doing it
  // similar to Collator::create_ordinary_transaction() and Collator::create_ticktock_transaction()
  // ....
  std::unique_ptr<block::Transaction> trs =
      std::make_unique<block::Transaction>(account, trans_type, lt, now_, in_msg_root);
  if (in_msg_root.not_null()) {
    if (!trs->unpack_input_msg(ihr_delivered, &action_phase_cfg_)) {
      // inbound external message was not accepted
      return reject_query(PSTRING() << "could not unpack inbound " << (external ? "external" : "internal")
                                    << " message processed by ordinary transaction " << lt << " of account "
                                    << addr.to_hex());
    }
  }
  if (trs->bounce_enabled) {
    if (!trs->prepare_storage_phase(storage_phase_cfg_, true)) {
      return reject_query(PSTRING() << "cannot re-create storage phase of transaction " << lt << " for smart contract "
                                    << addr.to_hex());
    }
    if (need_credit_phase && !trs->prepare_credit_phase()) {
      return reject_query(PSTRING() << "cannot create re-credit phase of transaction " << lt << " for smart contract "
                                    << addr.to_hex());
    }
  } else {
    if (need_credit_phase && !trs->prepare_credit_phase()) {
      return reject_query(PSTRING() << "cannot re-create credit phase of transaction " << lt << " for smart contract "
                                    << addr.to_hex());
    }
    if (!trs->prepare_storage_phase(storage_phase_cfg_, true, need_credit_phase)) {
      return reject_query(PSTRING() << "cannot re-create storage phase of transaction " << lt << " for smart contract "
                                    << addr.to_hex());
    }
  }
  if (!trs->prepare_compute_phase(compute_phase_cfg_)) {
    return reject_query(PSTRING() << "cannot re-create compute phase of transaction " << lt << " for smart contract "
                                  << addr.to_hex());
  }
  if (!trs->compute_phase->accepted) {
    if (external) {
      return reject_query(PSTRING() << "inbound external message claimed to be processed by ordinary transaction " << lt
                                    << " of account " << addr.to_hex()
                                    << " was in fact rejected (such transaction cannot appear in valid blocks)");
    } else if (trs->compute_phase->skip_reason == block::ComputePhase::sk_none) {
      return reject_query(PSTRING() << "inbound internal message processed by ordinary transaction " << lt
                                    << " of account " << addr.to_hex() << " was not processed without any reason");
    }
  }
  if (trs->compute_phase->success && !trs->prepare_action_phase(action_phase_cfg_)) {
    return reject_query(PSTRING() << "cannot re-create action phase of transaction " << lt << " for smart contract "
                                  << addr.to_hex());
  }
  if (trs->bounce_enabled && !trs->compute_phase->success && !trs->prepare_bounce_phase(action_phase_cfg_)) {
    return reject_query(PSTRING() << "cannot re-create bounce phase of  transaction " << lt << " for smart contract "
                                  << addr.to_hex());
  }
  if (!trs->serialize()) {
    return reject_query(PSTRING() << "cannot re-create the serialization of  transaction " << lt
                                  << " for smart contract " << addr.to_hex());
  }
  if (block_limit_status_ && !trs->update_limits(*block_limit_status_)) {
    return fatal_error(PSTRING() << "cannot update block limit status to include transaction " << lt << " of account "
                                 << addr.to_hex());
  }
  auto trans_root2 = trs->commit(account);
  if (trans_root2.is_null()) {
    return reject_query(PSTRING() << "the re-created transaction " << lt << " for smart contract " << addr.to_hex()
                                  << " could not be committed");
  }
  // now compare the re-created transaction with the one we have
  if (trans_root2->get_hash() != trans_root->get_hash()) {
    if (verbosity >= 3 * 0) {
      std::cerr << "original transaction " << lt << " of " << addr.to_hex() << ": ";
      block::gen::t_Transaction.print_ref(std::cerr, trans_root);
      std::cerr << "re-created transaction " << lt << " of " << addr.to_hex() << ": ";
      block::gen::t_Transaction.print_ref(std::cerr, trans_root2);
    }
    return reject_query(PSTRING() << "the transaction " << lt << " of " << addr.to_hex() << " has hash "
                                  << trans_root->get_hash().to_hex()
                                  << " different from that of the recreated transaction "
                                  << trans_root2->get_hash().to_hex());
  }
  block::gen::Transaction::Record trans2;
  block::gen::HASH_UPDATE::Record hash_upd2;
  if (!(tlb::unpack_cell(trans_root2, trans2) &&
        tlb::type_unpack_cell(std::move(trans2.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd2))) {
    return fatal_error(PSTRING() << "cannot unpack the re-created transaction " << lt << " of " << addr.to_hex());
  }
  if (hash_upd2.old_hash != hash_upd.old_hash) {
    return fatal_error(PSTRING() << "the re-created transaction " << lt << " of " << addr.to_hex()
                                 << " is invalid: it starts from account state with different hash");
  }
  if (hash_upd2.new_hash != account.total_state->get_hash().bits()) {
    return fatal_error(
        PSTRING() << "the re-created transaction " << lt << " of " << addr.to_hex()
                  << " is invalid: its claimed new account hash differs from the actual new account state");
  }
  if (hash_upd.new_hash != account.total_state->get_hash().bits()) {
    return reject_query(PSTRING() << "transaction " << lt << " of " << addr.to_hex()
                                  << " is invalid: it claims that the new account state hash is "
                                  << hash_upd.new_hash.to_hex() << " but the re-computed value is "
                                  << hash_upd2.new_hash.to_hex());
  }
  if (!trans.r1.out_msgs->contents_equal(*trans2.r1.out_msgs)) {
    return reject_query(
        PSTRING()
        << "transaction " << lt << " of " << addr.to_hex()
        << " is invalid: it has produced a set of outbound messages different from that listed in the transaction");
  }
  // check new balance and value flow
  auto new_balance = account.get_balance();
  block::CurrencyCollection total_fees;
  if (!total_fees.validate_unpack(trans.total_fees)) {
    return reject_query(PSTRING() << "transaction " << lt << " of " << addr.to_hex()
                                  << " has an invalid total_fees value");
  }
  if (old_balance + money_imported != new_balance + money_exported + total_fees) {
    return reject_query(PSTRING() << "transaction " << lt << " of " << addr.to_hex()
                                  << " violates the currency flow condition: old balance=" << old_balance.to_str()
                                  << " + imported=" << money_imported.to_str() << " does not equal new balance="
                                  << new_balance.to_str() << " + exported=" << money_exported.to_str()
                                  << " + total_fees=" << total_fees.to_str());
  }
  return true;
}

// NB: may be run in parallel for different accounts
bool ValidateQuery::check_account_transactions(const StdSmcAddress& acc_addr, Ref<vm::CellSlice> acc_blk_root) {
  block::gen::AccountBlock::Record acc_blk;
  CHECK(tlb::csr_unpack(std::move(acc_blk_root), acc_blk) && acc_blk.account_addr == acc_addr);
  auto account_p = unpack_account(acc_addr.cbits());
  if (!account_p) {
    return reject_query("cannot unpack old state of account "s + acc_addr.to_hex());
  }
  auto& account = *account_p;
  CHECK(account.addr == acc_addr);
  vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                     block::tlb::aug_AccountTransactions};
  td::BitArray<64> min_trans, max_trans;
  CHECK(trans_dict.get_minmax_key(min_trans).not_null() && trans_dict.get_minmax_key(max_trans, true).not_null());
  ton::LogicalTime min_trans_lt = min_trans.to_ulong(), max_trans_lt = max_trans.to_ulong();
  if (!trans_dict.check_for_each_extra([this, &account, min_trans_lt, max_trans_lt](Ref<vm::CellSlice> value,
                                                                                    Ref<vm::CellSlice> extra,
                                                                                    td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 64);
        ton::LogicalTime lt = key.get_uint(64);
        extra.clear();
        return check_one_transaction(account, lt, value->prefetch_ref(), lt == min_trans_lt, lt == max_trans_lt);
      })) {
    return reject_query("at least one Transaction of account "s + acc_addr.to_hex() + " is invalid");
  }
  if (is_masterchain() && account.libraries_changed()) {
    return scan_account_libraries(account.orig_library, account.library, acc_addr);
  } else {
    return true;
  }
}

bool ValidateQuery::check_transactions() {
  LOG(INFO) << "checking all transactions";
  return account_blocks_dict_->check_for_each_extra(
      [this](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 256);
        return check_account_transactions(key, std::move(value));
      });
}

// similar to Collator::update_account_public_libraries()
bool ValidateQuery::scan_account_libraries(Ref<vm::Cell> orig_libs, Ref<vm::Cell> final_libs, const td::Bits256& addr) {
  vm::Dictionary dict1{std::move(orig_libs), 256}, dict2{std::move(final_libs), 256};
  return dict1.scan_diff(
             dict2,
             [this, &addr](td::ConstBitPtr key, int n, Ref<vm::CellSlice> val1, Ref<vm::CellSlice> val2) -> bool {
               CHECK(n == 256);
               bool f = block::is_public_library(key, std::move(val1));
               bool g = block::is_public_library(key, val2);
               if (f != g) {
                 lib_publishers_.emplace_back(key, addr, g);
               }
               return true;
             },
             3) ||
         reject_query("error scanning old and new libraries of account "s + addr.to_hex());
}

bool ValidateQuery::check_all_ticktock_processed() {
  if (!is_masterchain()) {
    return true;
  }
  LOG(DEBUG) << "getting the list of special tick-tock smart contracts";
  auto res = config_->get_special_ticktock_smartcontracts(3);
  if (res.is_error()) {
    return fatal_error(res.move_as_error());
  }
  auto ticktock_smcs = res.move_as_ok();
  LOG(DEBUG) << "have " << ticktock_smcs.size() << " tick-tock smart contracts";
  for (auto addr : ticktock_smcs) {
    LOG(DEBUG) << "special smart contract " << addr.first.to_hex() << " with ticktock=" << addr.second;
    if (!account_blocks_dict_->key_exists(addr.first)) {
      return reject_query(
          PSTRING()
          << "there are no transactions (and in particular, no tick-tock transactions) for special smart contract "
          << addr.first.to_hex() << " with ticktock=" << addr.second);
    }
  }
  return true;
}

bool ValidateQuery::check_message_processing_order() {
  std::sort(msg_proc_lt_.begin(), msg_proc_lt_.end());
  for (std::size_t i = 1; i < msg_proc_lt_.size(); i++) {
    auto &a = msg_proc_lt_[i - 1], &b = msg_proc_lt_[i];
    if (std::get<0>(a) == std::get<0>(b) && std::get<2>(a) > std::get<2>(b)) {
      return reject_query(PSTRING() << "incorrect message processing order: transaction (" << std::get<1>(a) << ","
                                    << std::get<0>(a).to_hex() << ") processes message created at logical time "
                                    << std::get<2>(a) << ", but a later transaction (" << std::get<1>(b) << ","
                                    << std::get<0>(a).to_hex()
                                    << ") processes an earlier message created at logical time " << std::get<2>(b));
    }
  }
  return true;
}

bool ValidateQuery::check_special_message(Ref<vm::Cell> in_msg_root, const block::CurrencyCollection& amount,
                                          Ref<vm::Cell> addr_cell) {
  if (in_msg_root.is_null()) {
    return amount.is_zero();
  }
  CHECK(!amount.is_zero());
  if (!is_masterchain()) {
    return reject_query("special messages can be present in masterchain only");
  }
  block::gen::InMsg::Record_msg_import_imm in;
  block::tlb::MsgEnvelope::Record_std env;
  if (!(tlb::unpack_cell(in_msg_root, in) && tlb::unpack_cell(in.in_msg, env))) {
    return reject_query("cannot unpack msg_import_imm InMsg for a special message");
  }
  Bits256 msg_hash = env.msg->get_hash().bits();
  LOG(DEBUG) << "checking special message with hash " << msg_hash.to_hex() << " and expected amount "
             << amount.to_str();
  auto in_msg_cs = in_msg_dict_->lookup(msg_hash);
  if (in_msg_cs.is_null()) {
    return reject_query("InMsg of special message with hash "s + msg_hash.to_hex() +
                        " is not registered in InMsgDescr");
  }
  if (!vm::load_cell_slice(in_msg_root).contents_equal(*in_msg_cs)) {
    return reject_query("InMsg of special message with hash "s + msg_hash.to_hex() +
                        " differs from the InMsgDescr entry with this key");
  }
  vm::CellSlice cs{vm::NoVmOrd(), env.msg};
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  CHECK(tlb::unpack(cs, info));  // this has been already checked for all InMsgDescr
  auto src_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.src);
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  CHECK(src_prefix.is_valid() && dest_prefix.is_valid());  // we have checked this for all InMsgDescr
  auto cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
  auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
  if (cur_prefix != dest_prefix || next_prefix != dest_prefix) {
    return reject_query("special message with hash "s + msg_hash.to_hex() +
                        " has not been routed to its final destination");
  }
  if (!shard_contains(shard_, src_prefix)) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has source address " +
                        src_prefix.to_str() + " outside this shard");
  }
  if (!shard_contains(shard_, dest_prefix)) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has destination address " +
                        dest_prefix.to_str() + " outside this shard");
  }
  if (env.fwd_fee_remaining->sgn()) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has a non-zero fwd_fee_remaining");
  }
  if (block::tlb::t_Grams.as_integer(info.fwd_fee)->sgn()) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has a non-zero fwd_fee");
  }
  if (block::tlb::t_Grams.as_integer(info.ihr_fee)->sgn()) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has a non-zero ihr_fee");
  }
  block::CurrencyCollection value;
  if (!value.validate_unpack(info.value)) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has an invalid value");
  }
  if (value != amount) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " carries an incorrect amount " +
                        value.to_str() + " instead of " + amount.to_str() + " postulated by ValueFlow");
  }
  WorkchainId src_wc, dest_wc;
  StdSmcAddress src_addr, dest_addr, correct_addr;
  if (!(block::tlb::t_MsgAddressInt.extract_std_address(info.src, src_wc, src_addr) &&
        block::tlb::t_MsgAddressInt.extract_std_address(info.dest, dest_wc, dest_addr))) {
    return reject_query("cannot unpack source and destination addresses of special message with hash "s +
                        msg_hash.to_hex());
  }
  if (src_wc != masterchainId || !src_addr.is_zero()) {
    return reject_query(PSTRING() << "special message with hash " << msg_hash.to_hex()
                                  << " has a non-zero source address " << src_wc << ":" << src_addr.to_hex());
  }
  CHECK(dest_wc == masterchainId);
  if (addr_cell.is_null() || vm::load_cell_slice(addr_cell).size_ext() != 0x100) {
    return reject_query("special message with hash "s + msg_hash.to_hex() +
                        " has no correct destination address defined in the current configuration");
  }
  CHECK(vm::load_cell_slice(addr_cell).prefetch_bits_to(correct_addr));
  if (dest_addr != correct_addr) {
    return reject_query("special message with hash "s + msg_hash.to_hex() +
                        " has destination address -1:" + dest_addr.to_hex() +
                        " but the correct address defined by the configuration is " + correct_addr.to_hex());
  }
  if (cs.size_ext() != 2 || cs.prefetch_ulong(2)) {
    return reject_query("special message with hash "s + msg_hash.to_hex() + " has a non-empty body");
  }
  return true;
}

bool ValidateQuery::check_special_messages() {
  return check_special_message(recover_create_msg_, value_flow_.recovered, config_->get_config_param(3, 1)) &&
         check_special_message(mint_msg_, value_flow_.minted, config_->get_config_param(2, 0));
}

bool ValidateQuery::check_one_library_update(td::ConstBitPtr key, Ref<vm::CellSlice> old_value,
                                             Ref<vm::CellSlice> new_value) {
  // shared_lib_descr$00 lib:^Cell publishers:(Hashmap 256 True) = LibDescr;
  std::unique_ptr<vm::Dictionary> old_publishers, new_publishers;
  if (new_value.not_null()) {
    if (!block::gen::t_LibDescr.validate_csr(new_value)) {
      return reject_query("LibDescr with key "s + key.to_hex(256) +
                          " in the libraries dictionary of the new state failed to pass automatic validity tests");
    }
    auto lib_ref = new_value->prefetch_ref();
    CHECK(lib_ref.not_null());
    if (lib_ref->get_hash().as_bitslice() != key) {
      return reject_query("LibDescr with key "s + key.to_hex(256) +
                          " in the libraries dictionary of the new state contains a library with different root hash " +
                          lib_ref->get_hash().to_hex());
    }
    CHECK(new_value.write().advance_ext(2, 1));
    new_publishers = std::make_unique<vm::Dictionary>(vm::DictNonEmpty(), std::move(new_value), 256);
  } else {
    new_publishers = std::make_unique<vm::Dictionary>(256);
  }
  if (old_value.not_null() && !block::gen::t_LibDescr.validate_csr(old_value)) {
    return reject_query("LibDescr with key "s + key.to_hex(256) +
                        " in the libraries dictionary of the old state failed to pass automatic validity tests");
    CHECK(old_value.write().advance_ext(2, 1));
    old_publishers = std::make_unique<vm::Dictionary>(vm::DictNonEmpty(), std::move(old_value), 256);
  } else {
    old_publishers = std::make_unique<vm::Dictionary>(256);
  }
  if (!old_publishers->scan_diff(
          *new_publishers,
          [this, lib_key = key](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val,
                                Ref<vm::CellSlice> new_val) {
            CHECK(key_len == 256);
            if (old_val.not_null() && !old_val->empty_ext()) {
              return false;
            }
            if (new_val.not_null() && !new_val->empty_ext()) {
              return false;
            }
            CHECK(old_val.not_null() != new_val.not_null());
            lib_publishers2_.emplace_back(lib_key, key, new_val.not_null());
            return true;
          },
          3 /* check augmentation of changed nodes */)) {
    return reject_query("invalid publishers set for shard library with hash "s + key.to_hex(256));
  }
  return true;
}

bool ValidateQuery::check_shard_libraries() {
  CHECK(ps_.shard_libraries_ && ns_.shard_libraries_);
  if (!ps_.shard_libraries_->scan_diff(
          *ns_.shard_libraries_,
          [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val, Ref<vm::CellSlice> new_val) {
            CHECK(key_len == 256);
            return check_one_library_update(key, std::move(old_val), std::move(new_val));
          },
          3 /* check augmentation of changed nodes */)) {
    return reject_query("invalid shard libraries dictionary in the new state");
  }
  std::sort(lib_publishers_.begin(), lib_publishers_.end());
  std::sort(lib_publishers2_.begin(), lib_publishers2_.end());
  if (lib_publishers_ != lib_publishers2_) {
    // TODO: better error message with by-element comparison?
    return reject_query("the set of public libraries and their publishing accounts has not been updated correctly");
  }
  return true;
}

bool ValidateQuery::check_new_state() {
  LOG(INFO) << "checking header of the new shardchain state";
  block::gen::ShardStateUnsplit::Record info;
  if (!tlb::unpack_cell(state_root_, info)) {
    return reject_query("the header of the new shardchain state cannot be unpacked");
  }
  // shard_state#9023afe2 global_id:int32 -> checked in unpack_next_state()
  // shard_id:ShardIdent -> checked in unpack_next_state()
  // seq_no:uint32 vert_seq_no:# -> checked in unpack_next_state()
  // gen_utime:uint32 gen_lt:uint64 -> checked in unpack_next_state()
  // min_ref_mc_seqno:uint32
  ton::BlockSeqno my_mc_seqno = is_masterchain() ? id_.seqno() : mc_seqno_;
  ton::BlockSeqno ref_mc_seqno =
      std::min(std::min(my_mc_seqno, min_shard_ref_mc_seqno_), ns_.processed_upto_->min_mc_seqno());
  if (ns_.min_ref_mc_seqno_ != ref_mc_seqno) {
    return reject_query(
        PSTRING() << "new state of " << id_.to_str() << " has minimal referenced masterchain block seqno "
                  << ns_.min_ref_mc_seqno_
                  << " but the value computed from all shard references and previous masterchain block reference is "
                  << ref_mc_seqno << " = min(" << my_mc_seqno << "," << min_shard_ref_mc_seqno_ << ","
                  << ns_.processed_upto_->min_mc_seqno() << ")");
  }
  // out_msg_queue_info:^OutMsgQueueInfo
  // -> _ out_queue:OutMsgQueue proc_info:ProcessedInfo
  //      ihr_pending:IhrPendingInfo = OutMsgQueueInfo;
  if (!ns_.ihr_pending_->is_empty()) {
    return reject_query("IhrPendingInfo in the new state of "s + id_.to_str() +
                        " is non-empty, but IHR delivery is now disabled");
  }
  // before_split:(## 1) -> checked in unpack_next_state()
  // accounts:^ShardAccounts -> checked in precheck_account_updates() + other
  // ^[ overload_history:uint64 underload_history:uint64
  if (ns_.overload_history_ & ns_.underload_history_ & 1) {
    return reject_query(
        "lower-order bits both set in the new state's overload_history and underload history (block cannot be both "
        "overloaded and underloaded)");
  }
  if (after_split_ || after_merge_) {
    if ((ns_.overload_history_ | ns_.underload_history_) & ~1ULL) {
      return reject_query(
          "new block is immediately after split or after merge, but the old underload or overload history has not been "
          "cleared");
    }
  } else {
    if ((ns_.overload_history_ ^ (ps_.overload_history_ << 1)) & ~1ULL) {
      return reject_query(PSTRING() << "new overload history " << ns_.overload_history_
                                    << " is not compatible with the old overload history " << ps_.overload_history_);
    }
    if ((ns_.underload_history_ ^ (ps_.underload_history_ << 1)) & ~1ULL) {
      return reject_query(PSTRING() << "new underload history " << ns_.underload_history_
                                    << " is not compatible with the old underload history " << ps_.underload_history_);
    }
  }
  // total_balance:CurrencyCollection
  // total_validator_fees:CurrencyCollection
  block::CurrencyCollection total_balance, total_validator_fees, old_total_validator_fees(ps_.total_validator_fees_);
  if (!(total_balance.validate_unpack(info.r1.total_balance) &&
        total_validator_fees.validate_unpack(info.r1.total_validator_fees))) {
    return reject_query("cannot unpack total_balance and total_validator_fees in the header of the new state");
  }
  if (total_balance != value_flow_.to_next_blk) {
    return reject_query(
        "new state declares total balance "s + total_balance.to_str() +
        " different from to_next_blk in value flow (obtained by summing balances of all accounts in the new state): " +
        value_flow_.to_next_blk.to_str());
  }
  LOG(DEBUG) << "checking total validator fees: new=" << total_validator_fees.to_str()
             << "+recovered=" << value_flow_.recovered.to_str() << " == old=" << old_total_validator_fees.to_str()
             << "+collected=" << value_flow_.fees_collected.to_str();
  if (total_validator_fees + value_flow_.recovered != old_total_validator_fees + value_flow_.fees_collected) {
    return reject_query("new state declares total validator fees "s + total_validator_fees.to_str() +
                        " not equal to the sum of old total validator fees " + old_total_validator_fees.to_str() +
                        " and the fees collected in this block " + value_flow_.fees_collected.to_str() +
                        " minus the recovered fees " + value_flow_.recovered.to_str());
  }
  // libraries:(HashmapE 256 LibDescr)
  if (is_masterchain()) {
    if (!check_shard_libraries()) {
      return reject_query("the set of public libraries in the new state is invalid");
    }
  } else if (!ns_.shard_libraries_->is_empty()) {
    return reject_query(
        "new state contains a non-empty public library collection, which is not allowed for non-masterchain blocks");
  }
  // master_ref:(Maybe BlkMasterInfo) ]
  if (info.r1.master_ref->prefetch_ulong(1) != !is_masterchain()) {
    return reject_query("new state "s + (is_masterchain() ? "contains" : "does not contain") +
                        " a masterchain block reference (master_ref)");
  }
  // custom:(Maybe ^McStateExtra) -> checked in check_mc_state_extra()
  // = ShardStateUnsplit;
  return true;
}

bool ValidateQuery::check_config_update(Ref<vm::CellSlice> old_conf_params, Ref<vm::CellSlice> new_conf_params) {
  if (!block::gen::t_ConfigParams.validate_csr(10000, new_conf_params)) {
    return reject_query("new configuration failed to pass automated validity checks");
  }
  if (!block::gen::t_ConfigParams.validate_csr(10000, old_conf_params)) {
    return fatal_error("old configuration failed to pass automated validity checks");
  }
  td::Bits256 old_cfg_addr, new_cfg_addr;
  Ref<vm::Cell> old_cfg_root, new_cfg_root;
  CHECK(block::gen::t_ConfigParams.unpack_cons1(old_conf_params.write(), old_cfg_addr, old_cfg_root) &&
        block::gen::t_ConfigParams.unpack_cons1(new_conf_params.write(), new_cfg_addr, new_cfg_root));
  if (!block::valid_config_data(new_cfg_root, new_cfg_addr, true, false, old_mparams_)) {
    return reject_query(
        "new configuration parameters failed to pass per-parameter automated validity checks, or one of mandatory "
        "configuration parameters is missing");
  }
  auto ocfg_res = block::get_config_data_from_smc(ns_.account_dict_->lookup(old_cfg_addr));
  if (ocfg_res.is_error()) {
    return reject_query("cannot extract configuration from the new state of the (old) configuration smart contract "s +
                        old_cfg_addr.to_hex() + " : " + ocfg_res.move_as_error().to_string());
  }
  auto ocfg_root = ocfg_res.move_as_ok();
  CHECK(ocfg_root.not_null());
  auto ncfg_res = block::get_config_data_from_smc(ns_.account_dict_->lookup(new_cfg_addr));
  if (ncfg_res.is_error()) {
    return reject_query("cannot extract configuration from the new state of the (new) configuration smart contract "s +
                        new_cfg_addr.to_hex() + " : " + ncfg_res.move_as_error().to_string());
  }
  auto ncfg_root = ncfg_res.move_as_ok();
  CHECK(ncfg_root.not_null());
  bool cfg_acc_changed = (new_cfg_addr != old_cfg_addr);
  if (ncfg_root->get_hash() != new_cfg_root->get_hash()) {
    return reject_query(
        "the new configuration is different from that stored in the persistent data of the (new) configuration smart contract "s +
        old_cfg_addr.to_hex());
  }
  if (!block::valid_config_data(ocfg_root, old_cfg_addr, true, true, old_mparams_)) {
    return reject_query("configuration extracted from (old) configuration smart contract "s + old_cfg_addr.to_hex() +
                        " failed to pass per-parameter validity checks, or one of mandatory parameters is missing");
  }
  if (block::important_config_parameters_changed(new_cfg_root, old_cfg_root)) {
    // same as the check in Collator::create_mc_state_extra()
    LOG(WARNING) << "the global configuration changes in block " << id_.to_str();
    if (!is_key_block_) {
      return reject_query(
          "important parameters in the global configuration have changed, but the block is not marked as a key block");
    }
  } else if (is_key_block_ &&
             !(cfg_acc_changed || block::important_config_parameters_changed(new_cfg_root, old_cfg_root, true))) {
    return reject_query("no important parameters have been changed, but the block is marked as a key block");
  }
  vm::Dictionary dict1{ocfg_root, 32};
  auto param0 = dict1.lookup_ref(td::BitArray<32>{1 - 1});
  if (param0.is_null()) {
    if (cfg_acc_changed) {
      return reject_query("new state of old configuration smart contract "s + old_cfg_addr.to_hex() +
                          " contains no value for parameter 0 (new configuration smart contract address), but the "
                          "configuration smart contract has been somehow changed to " +
                          new_cfg_addr.to_hex());
    }
    return true;
  }
  td::Bits256 want_cfg_addr;
  CHECK(vm::load_cell_slice(std::move(param0)).prefetch_bits_to(want_cfg_addr));
  if (want_cfg_addr == old_cfg_addr) {
    if (cfg_acc_changed) {
      return reject_query("new state of old configuration smart contract "s + old_cfg_addr.to_hex() +
                          " contains the same value for parameter 0 (configuration smart contract address), but the "
                          "configuration smart contract has been somehow changed to " +
                          new_cfg_addr.to_hex());
    }
    return true;
  }
  if (want_cfg_addr != new_cfg_addr && cfg_acc_changed) {
    return reject_query("new state of old configuration smart contract "s + old_cfg_addr.to_hex() + " contains " +
                        want_cfg_addr.to_hex() +
                        " as the value for parameter 0 (new configuration smart contract address), but the "
                        "configuration smart contract has been somehow changed to a different value " +
                        new_cfg_addr.to_hex());
  }
  // now old_cfg_addr = new_cfg_addr != want_cfg_addr
  // the configuration smart contract has not been switched to want_cfg_addr, have to check why
  auto wcfg_res = block::get_config_data_from_smc(ns_.account_dict_->lookup(want_cfg_addr));
  if (wcfg_res.is_error()) {
    LOG(WARNING) << "switching of configuration smart contract did not happen because the suggested new configuration "
                    "smart contract "
                 << want_cfg_addr.to_hex() << " does not contain a valid configuration : " << wcfg_res.move_as_error();
    return true;
  }
  auto wcfg_root = wcfg_res.move_as_ok();
  if (!block::valid_config_data(wcfg_root, want_cfg_addr, true, false, old_mparams_)) {
    LOG(WARNING)
        << "switching of configuration smart contract did not happen because the configuration extracted from "
           "suggested new configuration smart contract "
        << want_cfg_addr.to_hex()
        << " failed to pass per-parameter validity checks, or one of mandatory configuration parameters is missing";
    return true;
  }
  return reject_query("old configuration smart contract "s + old_cfg_addr.to_hex() + " suggested " +
                      want_cfg_addr.to_hex() +
                      " as the new configuration smart contract, but the switchover did not happen without a good "
                      "reason (the suggested configuration appears to be valid)");
}

bool ValidateQuery::check_one_prev_dict_update(ton::BlockSeqno seqno, Ref<vm::CellSlice> old_val_extra,
                                               Ref<vm::CellSlice> new_val_extra) {
  if (old_val_extra.not_null() && new_val_extra.is_null()) {
    // if this becomes allowed in some situations, then check necessary conditions and return true
    return reject_query(
        PSTRING()
        << "entry with seqno " << seqno
        << " disappeared in the new previous blocks dictionary as compared to the old previous blocks dictionary");
  }
  CHECK(new_val_extra.not_null());
  vm::CellSlice cs{*new_val_extra};
  if (!(block::gen::t_KeyMaxLt.validate_skip_upto(16, cs) && block::gen::t_KeyExtBlkRef.validate_skip_upto(16, cs) &&
        cs.empty_ext())) {
    return reject_query(PSTRING() << "entry with seqno " << seqno
                                  << " in the new previous blocks dictionary failed to pass automated validity checks "
                                     "form KeyMaxLt + KeyExtBlkRef");
  }
  if (old_val_extra.not_null()) {
    CHECK(!new_val_extra->contents_equal(*old_val_extra));
    return reject_query(PSTRING() << "entry with seqno " << seqno
                                  << " changed in the new previous blocks dictionary as compared to its old version "
                                     "(entries should never change once they have been added)");
  }
  auto& cs2 = new_val_extra.write();
  bool is_key;
  BlockIdExt blkid;
  LogicalTime lt;
  CHECK(block::gen::t_KeyMaxLt.skip(cs2) && cs2.fetch_bool_to(is_key) &&
        block::tlb::t_ExtBlkRef.unpack(cs2, blkid, &lt) && cs2.empty_ext());
  if (seqno != mc_seqno_) {
    return reject_query(PSTRING() << "new previous blocks dictionary contains a new entry with seqno " << seqno
                                  << " while the only new entry must be for the previous block with seqno "
                                  << mc_seqno_);
  }
  if (blkid.seqno() != seqno) {
    return reject_query(PSTRING() << "new previous blocks dictionary entry with seqno " << seqno
                                  << " in fact describes a block " << blkid.to_str() << " with different seqno");
  }
  if (blkid != prev_blocks.at(0)) {
    return reject_query("new previous blocks dictionary has a new entry for previous block " + blkid.to_str() +
                        " while the correct previous block is " + prev_blocks[0].to_str());
  }
  if (lt != config_->lt) {
    return reject_query(PSTRING() << "previous blocks dictionary has new entry for previous block " << blkid.to_str()
                                  << " indicating end_lt=" << lt << " but the correct value is " << config_->lt);
  }
  if (is_key != config_->is_key_state()) {
    return reject_query(PSTRING() << "previous blocks dictionary has new entry for previous block " << blkid.to_str()
                                  << " indicating is_key_block=" << is_key << " but the correct value is "
                                  << (int)config_->is_key_state());
  }
  return true;
}

// somewhat similar to Collator::create_mc_state_extra()
bool ValidateQuery::check_mc_state_extra() {
  if (!is_masterchain()) {
    if (ns_.mc_state_extra_.not_null()) {
      return reject_query("new state defined by non-masterchain block "s + id_.to_str() + " contains a McStateExtra");
    }
    return true;
  }
  LOG(INFO) << "checking header of McStateExtra in the new masterchain state";
  if (ps_.mc_state_extra_.is_null()) {
    return fatal_error("previous masterchain state did not contain a McStateExtra");
  }
  if (ns_.mc_state_extra_.is_null()) {
    return reject_query("new masterchain state does not contain a McStateExtra");
  }
  block::gen::McStateExtra::Record old_extra, new_extra;
  if (!tlb::unpack_cell(ps_.mc_state_extra_, old_extra)) {
    return reject_query("cannot unpack old McStateExtra");
  }
  if (!tlb::unpack_cell(ns_.mc_state_extra_, new_extra)) {
    return reject_query("cannot unpack new McStateExtra");
  }
  // masterchain_state_extra#cc26
  // shard_hashes:ShardHashes has been checked separately
  // config:ConfigParams
  if (!check_config_update(old_extra.config, new_extra.config)) {
    return reject_query("invalid configuration update");
  }
  // ...
  // flags:(## 16) { flags <= 1 }
  if (new_extra.r1.flags & ~1) {
    return reject_query(PSTRING() << "new McStateExtra has non-zero (unsupported) extension flags "
                                  << new_extra.r1.flags << "; validator too old?");
  }
  if ((bool)(new_extra.r1.flags & 1) != create_stats_enabled_) {
    return reject_query(PSTRING() << "new McStateExtra has extension flags " << new_extra.r1.flags
                                  << " but active configuration defines create_stats_enabled="
                                  << create_stats_enabled_);
  }
  // validator_info:ValidatorInfo
  // (already checked in check_mc_validator_info())
  // prev_blocks:OldMcBlocksInfo
  try {
    vm::AugmentedDictionary old_prev_dict{old_extra.r1.prev_blocks, 32, block::tlb::aug_OldMcBlocksInfo};
    vm::AugmentedDictionary new_prev_dict{new_extra.r1.prev_blocks, 32, block::tlb::aug_OldMcBlocksInfo};
    if (!old_prev_dict.scan_diff(
            new_prev_dict,
            [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val_extra,
                   Ref<vm::CellSlice> new_val_extra) {
              CHECK(key_len == 32);
              return check_one_prev_dict_update((unsigned)key.get_uint(32), std::move(old_val_extra),
                                                std::move(new_val_extra));
            },
            3 /* check augmentation of changed nodes */)) {
      return reject_query("invalid previous block dictionary in the new state");
    }
    td::BitArray<32> key;
    auto val = old_prev_dict.get_minmax_key(key, true);
    if (val.not_null() && key.to_ulong() >= mc_seqno_) {
      return reject_query(PSTRING() << "previous block dictionary for the previous state with seqno " << mc_seqno_
                                    << " contains information about 'previous' masterchain block with seqno "
                                    << key.to_ulong());
    }
    val = new_prev_dict.get_minmax_key(key, true);
    if (val.is_null()) {
      return reject_query(
          "new previous blocks dictionary is empty (at least the immediately previous block should be there)");
    }
    CHECK(id_.seqno() == mc_seqno_ + 1);
    if (key.to_ulong() > mc_seqno_) {
      return reject_query(PSTRING() << "previous block dictionary for the new state with seqno " << id_.seqno()
                                    << " contains information about a future masterchain block with seqno "
                                    << key.to_ulong());
    }
    if (key.to_ulong() != mc_seqno_) {
      return reject_query(
          PSTRING() << "previous block dictionary for the new state of masterchain block " << id_.to_str()
                    << " does not contain information about immediately previous block with seqno " << mc_seqno_);
    }
  } catch (vm::VmError& err) {
    return reject_query(
        "error scanning new previous blocks dictionary in McStateExtra of the new masterchain state : "s +
        err.get_msg());
  }
  // after_key_block:Bool
  if (new_extra.r1.after_key_block != is_key_block_) {
    return reject_query(PSTRING() << "new McStateExtra has after_key_block=" << new_extra.r1.after_key_block
                                  << " while the block header claims is_key_block=" << is_key_block_);
  }
  // last_key_block:(Maybe ExtBlkRef)
  if (!block::gen::t_Maybe_ExtBlkRef.validate_csr(16, new_extra.r1.last_key_block)) {
    return reject_query(
        "last_key_block:(Maybe ExtBlkRef) in the new masterchain state failed to pass automated validity checks");
  }
  if (old_extra.r1.last_key_block->prefetch_ulong(1) && !new_extra.r1.last_key_block->prefetch_ulong(1)) {
    return reject_query("old McStateExtra had a non-trivial last_key_block, but the new one does not");
  }
  if (new_extra.r1.last_key_block->contents_equal(*old_extra.r1.last_key_block)) {
    if (config_->is_key_state()) {
      return reject_query(
          "last_key_block remains unchanged in the new masterchain state, but the previous block is a key block (it "
          "should become the new last_key_block)");
    }
  } else if (!new_extra.r1.last_key_block->prefetch_ulong(1)) {
    return reject_query("last_key_block:(Maybe ExtBlkRef) changed in the new state, but it became a nothing$0");
  } else {
    vm::CellSlice cs = *new_extra.r1.last_key_block;
    BlockIdExt blkid;
    LogicalTime lt;
    CHECK(cs.fetch_ulong(1) == 1 && block::tlb::t_ExtBlkRef.unpack(cs, blkid, &lt));
    if (blkid != prev_blocks.at(0) || lt != config_->lt) {
      return reject_query(PSTRING() << "last_key_block has been set in the new masterchain state to " << blkid.to_str()
                                    << " with lt " << lt
                                    << ", but the only possible value for this update is the previous block "
                                    << prev_blocks[0].to_str() << " with lt " << config_->lt);
    }
    if (!config_->is_key_state()) {
      return reject_query("last_key_block has been updated to the previous block "s + blkid.to_str() +
                          ", but it is not a key block");
    }
  }
  if (new_extra.r1.last_key_block->prefetch_ulong(1)) {
    auto& cs = new_extra.r1.last_key_block.write();
    BlockIdExt blkid;
    LogicalTime lt;
    CHECK(cs.fetch_ulong(1) == 1 && block::tlb::t_ExtBlkRef.unpack(cs, blkid, &lt));
    if (blkid != prev_key_block_) {
      return reject_query("new masterchain state declares previous key block to be "s + blkid.to_str() +
                          " but the value computed from previous masterchain state is " + prev_key_block_.to_str());
    }
  } else if (prev_key_block_exists_) {
    return reject_query(PSTRING() << "new masterchain state declares no previous key block, but the block header "
                                     "announces previous key block seqno "
                                  << prev_key_block_seqno_);
  }
  // block_create_stats:(flags . 0)?BlockCreateStats
  if (new_extra.r1.flags & 1) {
    block::gen::BlockCreateStats::Record_block_create_stats rec;
    if (!tlb::csr_unpack(new_extra.r1.block_create_stats, rec)) {
      return reject_query("cannot unpack BlockCreateStats in the new masterchain state");
    }
    if (!check_block_create_stats()) {
      return reject_query("invalid BlockCreateStats update in the new masterchain state");
    }
  }
  // global_balance:CurrencyCollection
  block::CurrencyCollection global_balance, old_global_balance;
  if (!global_balance.validate_unpack(new_extra.global_balance)) {
    return reject_query("cannot unpack global_balance in the new masterchain state");
  }
  if (!old_global_balance.validate_unpack(old_extra.global_balance)) {
    return reject_query("cannot unpack global_balance in the old masterchain state");
  }
  CHECK(old_global_balance == ps_.global_balance_);
  CHECK(global_balance == ns_.global_balance_);
  auto expected_global_balance = old_global_balance + value_flow_.minted + value_flow_.created + import_created_;
  if (global_balance != expected_global_balance) {
    return reject_query("global balance changed in unexpected way: expected old+minted+created+import_created = "s +
                        old_global_balance.to_str() + "+" + value_flow_.minted.to_str() + "+" +
                        value_flow_.created.to_str() + "+" + import_created_.to_str() + " = " +
                        expected_global_balance.to_str() + ", found " + global_balance.to_str());
  }
  // ...
  return true;
}

td::Status ValidateQuery::check_counter_update(const block::DiscountedCounter& oc, const block::DiscountedCounter& nc,
                                               unsigned expected_incr) {
  block::DiscountedCounter cc{oc};
  if (nc.is_zero()) {
    if (expected_incr) {
      return td::Status::Error(PSTRING() << "new counter total is zero, but the total should have been increased by "
                                         << expected_incr);
    }
    if (oc.is_zero()) {
      return td::Status::OK();
    }
    cc.increase_by(0, now_);
    if (!cc.almost_zero()) {
      return td::Status::Error(
          "counter has been reset to zero, but it still has non-zero components after relaxation: "s + cc.to_str() +
          "; original value before relaxation was " + oc.to_str());
    }
    return td::Status::OK();
  }
  if (!expected_incr) {
    if (oc == nc) {
      return td::Status::OK();
    } else {
      return td::Status::Error("unnecessary relaxation of counter from "s + oc.to_str() + " to " + nc.to_str() +
                               " without an increment");
    }
  }
  if (nc.total < oc.total) {
    return td::Status::Error(PSTRING() << "total counter goes back from " << oc.total << " to " << nc.total
                                       << " (increment by " << expected_incr << " expected instead)");
  }
  if (nc.total != oc.total + expected_incr) {
    return td::Status::Error(PSTRING() << "total counter has been incremented by " << nc.total - oc.total << ", from "
                                       << oc.total << " to " << nc.total << " (increment by " << expected_incr
                                       << " expected instead)");
  }
  if (!cc.increase_by(expected_incr, now_)) {
    return td::Status::Error(PSTRING() << "old counter value " << oc.to_str() << " cannot be increased by "
                                       << expected_incr);
  }
  if (!cc.almost_equals(nc)) {
    return td::Status::Error(PSTRING() << "counter " << oc.to_str() << " has been increased by " << expected_incr
                                       << " with an incorrect resulting value " << nc.to_str()
                                       << "; correct result should be " << cc.to_str()
                                       << " (up to +/-1 in the last two components)");
  }
  return td::Status::OK();
}

bool ValidateQuery::check_one_block_creator_update(td::ConstBitPtr key, Ref<vm::CellSlice> old_val,
                                                   Ref<vm::CellSlice> new_val) {
  LOG(DEBUG) << "checking update of CreatorStats for "s + key.to_hex(256);
  block::DiscountedCounter mc0, shard0, mc1, shard1;
  if (!block::unpack_CreatorStats(std::move(old_val), mc0, shard0)) {
    return reject_query("cannot unpack CreatorStats for "s + key.to_hex(256) + " from previous masterchain state");
  }
  bool nv_exists = new_val.not_null();
  if (!block::unpack_CreatorStats(std::move(new_val), mc1, shard1)) {
    return reject_query("cannot unpack CreatorStats for "s + key.to_hex(256) + " from new masterchain state");
  }
  unsigned mc_incr = (created_by_ == key);
  unsigned shard_incr = 0;
  if (key.is_zero(256)) {
    mc_incr = !created_by_.is_zero();
    shard_incr = block_create_total_;
  } else {
    auto it = block_create_count_.find(td::Bits256{key});
    shard_incr = (it == block_create_count_.end() ? 0 : it->second);
  }
  auto err = check_counter_update(mc0, mc1, mc_incr);
  if (err.is_error()) {
    return reject_query("invalid update of created masterchain blocks counter in CreatorStats for "s + key.to_hex(256) +
                        " : " + err.to_string());
  }
  err = check_counter_update(shard0, shard1, shard_incr);
  if (err.is_error()) {
    return reject_query("invalid update of created shardchain blocks counter in CreatorStats for "s + key.to_hex(256) +
                        " : " + err.to_string());
  }
  if (mc1.is_zero() && shard1.is_zero() && nv_exists) {
    return reject_query("new CreatorStats for "s + key.to_hex(256) +
                        " contains two zero counters (it should have been completely deleted instead)");
  }
  return true;
}

// similar to Collator::update_block_creator_stats()
bool ValidateQuery::check_block_create_stats() {
  LOG(INFO) << "checking all CreatorStats updates between the old and the new state";
  try {
    CHECK(ps_.block_create_stats_ && ns_.block_create_stats_);
    if (!ps_.block_create_stats_->scan_diff(
            *ns_.block_create_stats_,
            [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val, Ref<vm::CellSlice> new_val) {
              CHECK(key_len == 256);
              return check_one_block_creator_update(key, std::move(old_val), std::move(new_val));
            },
            3 /* check augmentation of changed nodes */)) {
      return reject_query("invalid BlockCreateStats dictionary in the new state");
    }
    for (const auto& p : block_create_count_) {
      auto old_val = ps_.block_create_stats_->lookup(p.first);
      auto new_val = ns_.block_create_stats_->lookup(p.first);
      if (old_val.is_null() != new_val.is_null()) {
        continue;
      }
      if (old_val.not_null() && !new_val->contents_equal(*old_val)) {
        continue;
      }
      if (!check_one_block_creator_update(p.first.bits(), std::move(old_val), std::move(new_val))) {
        return reject_query("invalid update of BlockCreator entry for "s + p.first.to_hex());
      }
    }
    auto key = td::Bits256::zero();
    auto old_val = ps_.block_create_stats_->lookup(key);
    auto new_val = ns_.block_create_stats_->lookup(key);
    if (new_val.is_null() && (!created_by_.is_zero() || block_create_total_)) {
      return reject_query(
          "new masterchain state does not contain a BlockCreator entry with zero key with total statistics");
    }
    if (!check_one_block_creator_update(key.bits(), std::move(old_val), std::move(new_val))) {
      return reject_query("invalid update of BlockCreator entry for "s + key.to_hex());
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid BlockCreateStats dictionary difference between the old and the new state: "s +
                        err.get_msg());
  }
  return true;
}

bool ValidateQuery::check_one_shard_fee(ShardIdFull shard, const block::CurrencyCollection& fees,
                                        const block::CurrencyCollection& created) {
  auto descr = new_shard_conf_->get_shard_hash(shard);
  if (descr.is_null()) {
    return reject_query("ShardFees contains a record for shard "s + shard.to_str() +
                        " but there is no corresponding record in the new shard configuration");
  }
  if (descr->reg_mc_seqno_ != id_.seqno()) {
    return reject_query("ShardFees contains a record for shard "s + shard.to_str() +
                        " but the corresponding record in the shard configuration has not been updated by this block");
  }
  if (fees != descr->fees_collected_) {
    return reject_query("ShardFees record for shard "s + shard.to_str() + " contains fees_collected value " +
                        fees.to_str() + " different from that present in shard configuration " +
                        descr->fees_collected_.to_str());
  }
  if (created != descr->funds_created_) {
    return reject_query("ShardFees record for shard "s + shard.to_str() + " contains funds_created value " +
                        created.to_str() + " different from that present in shard configuration " +
                        descr->funds_created_.to_str());
  }
  return true;
}

bool ValidateQuery::check_mc_block_extra() {
  if (!is_masterchain()) {
    return true;
  }
  // masterchain_block_extra#cca5
  // key_block:(## 1) -> checked in init_parse()
  // shard_hashes:ShardHashes -> checked in compute_next_state() and check_shard_layout()
  // shard_fees:ShardFees
  if (!fees_import_dict_->validate_check_extra(
          [this](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
            CHECK(key_len == 96);
            ShardIdFull shard{(int)key.get_int(32), (key + 32).get_uint(64)};
            block::gen::ShardFeeCreated::Record fc;
            block::CurrencyCollection fees, create;
            return (value->contents_equal(*extra) && tlb::csr_unpack(value, fc) && fees.validate_unpack(fc.fees) &&
                    create.unpack(fc.create) && check_one_shard_fee(shard, fees, create)) ||
                   reject_query("ShardFees entry with key "s + key.to_hex(96) + " corresponding to shard " +
                                shard.to_str() + " is invalid");
          })) {
    return reject_query("ShardFees dictionary is invalid");
  }
  block::gen::ShardFeeCreated::Record fc;
  block::CurrencyCollection fees_imported;
  if (!(tlb::csr_unpack(fees_import_dict_->get_root_extra(), fc) && fees_imported.validate_unpack(fc.fees) &&
        import_created_.validate_unpack(fc.create))) {
    return reject_query("cannot deserialize total fees_imported from the root of ShardFees");
  }
  if (fees_imported != value_flow_.fees_imported) {
    return reject_query("invalid fees_imported in value flow: declared "s + value_flow_.fees_imported.to_str() +
                        ", correct value is " + fees_imported.to_str());
  }
  // ^[ prev_blk_signatures:(HashmapE 16 CryptoSignaturePair)
  if (prev_signatures_.not_null() && id_.seqno() == 1) {
    return reject_query("block contains non-empty signature set for the zero state of the masterchain");
  }
  if (id_.seqno() > 1) {
    if (prev_signatures_.not_null()) {
      // TODO: check signatures here
    } else if (!is_fake_ && false) {  // FIXME: remove "&& false" when collator serializes signatures
      return reject_query("block contains an empty signature set for the previous block");
    }
  }
  //   recover_create_msg:(Maybe ^InMsg)
  //   mint_msg:(Maybe ^InMsg) ]
  // config:key_block?ConfigParams -> checked in compute_next_state() and ???
  return true;
}

/*
 * 
 *   MAIN VALIDATOR FUNCTION
 *     (invokes other methods in a suitable order)
 * 
 */

bool ValidateQuery::try_validate() {
  if (pending) {
    return true;
  }
  try {
    if (!stage_) {
      if (!compute_prev_state()) {
        return fatal_error(-666, "cannot compute previous state");
      }
      if (!compute_next_state()) {
        return reject_query("cannot compute next state");
      }
      if (!request_neighbor_queues()) {
        return fatal_error("cannot request neighbor output queues");
      }
      if (!unpack_prev_state()) {
        return fatal_error("cannot unpack previous state");
      }
      if (!unpack_next_state()) {
        return fatal_error("cannot unpack previous state");
      }
      if (is_masterchain() && !check_shard_layout()) {
        return fatal_error("new shard layout is invalid");
      }
      if (!check_cur_validator_set()) {
        return fatal_error("current validator set is not entitled to generate this block");
      }
      if (!check_utime_lt()) {
        return reject_query("creation utime/lt of the new block is invalid");
      }
      stage_ = 1;
      if (pending) {
        return true;
      }
    }
    LOG(INFO) << "running automated validity checks for block candidate " << id_.to_str();
    if (!block::gen::t_Block.validate_ref(1000000, block_root_)) {
      return reject_query("block "s + id_.to_str() + " failed to pass automated validity checks");
    }
    if (!fix_all_processed_upto()) {
      return fatal_error("cannot adjust all ProcessedUpto of neighbor and previous blocks");
    }
    if (!add_trivial_neighbor()) {
      return fatal_error("cannot add previous block as a trivial neighbor");
    }
    if (!unpack_block_data()) {
      return reject_query("cannot unpack block data");
    }
    if (!precheck_account_updates()) {
      return reject_query("invalid AccountState update");
    }
    if (!precheck_account_transactions()) {
      return reject_query("invalid collection of account transactions in ShardAccountBlocks");
    }
    if (!precheck_message_queue_update()) {
      return reject_query("invalid OutMsgQueue update");
    }
    if (!check_in_msg_descr()) {
      return reject_query("invalid InMsgDescr");
    }
    if (!check_out_msg_descr()) {
      return reject_query("invalid OutMsgDescr");
    }
    if (!check_processed_upto()) {
      return reject_query("invalid ProcessedInfo");
    }
    if (!check_in_queue()) {
      return reject_query("cannot check inbound message queues");
    }
    if (!check_delivered_dequeued()) {
      return reject_query("cannot check delivery status of all outbound messages");
    }
    if (!check_transactions()) {
      return reject_query("invalid collection of account transactions in ShardAccountBlocks");
    }
    if (!check_all_ticktock_processed()) {
      return reject_query("not all tick-tock transactions have been run for special accounts");
    }
    if (!check_message_processing_order()) {
      return reject_query("some messages have been processed by transactions in incorrect order");
    }
    if (!check_special_messages()) {
      return reject_query("special messages are invalid");
    }
    if (!check_new_state()) {
      return reject_query("the header of the new shardchain state is invalid");
    }
    if (!check_mc_block_extra()) {
      return reject_query("McBlockExtra of the new block is invalid");
    }
    if (!check_mc_state_extra()) {
      return reject_query("new McStateExtra is invalid");
    }
  } catch (vm::VmError& err) {
    return fatal_error(-666, err.get_msg());
  } catch (vm::VmVirtError& err) {
    return fatal_error(-666, err.get_msg());
  }
  return save_candidate();
}

bool ValidateQuery::save_candidate() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ValidateQuery::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ValidateQuery::written_candidate);
    }
  });

  td::actor::send_closure(manager, &ValidatorManager::set_block_candidate, id_, block_candidate.clone(), std::move(P));
  return true;
}

void ValidateQuery::written_candidate() {
  finish_query();
}

}  // namespace validator

}  // namespace ton
