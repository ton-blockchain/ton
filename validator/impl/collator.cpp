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
#include "collator-impl.h"
#include "vm/boc.h"
#include "td/db/utils/BlobView.h"
#include "vm/db/StaticBagOfCellsDb.h"
#include "block/mc-config.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "crypto/openssl/rand.hpp"
#include "ton/ton-shard.h"
#include "adnl/utils.hpp"
#include <cassert>
#include <algorithm>
#include "fabric.h"
#include "validator-set.hpp"
#include "top-shard-descr.hpp"
#include <ctime>
#include "td/utils/Random.h"

namespace ton {

int collator_settings = 0;

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

#define DBG(__n) dbg(__n)&&
#define DSTART int __dcnt = 0;
#define DEB DBG(++__dcnt)

static inline bool dbg(int c) TD_UNUSED;
static inline bool dbg(int c) {
  std::cerr << '[' << (char)('0' + c / 10) << (char)('0' + c % 10) << ']';
  return true;
}

Collator::Collator(ShardIdFull shard, bool is_hardfork, UnixTime min_ts, BlockIdExt min_masterchain_block_id,
                   std::vector<BlockIdExt> prev, td::Ref<ValidatorSet> validator_set, Ed25519_PublicKey collator_id,
                   td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                   td::Promise<BlockCandidate> promise)
    : shard_(shard)
    , is_hardfork_(is_hardfork)
    , min_ts(min_ts)
    , min_mc_block_id{min_masterchain_block_id}
    , prev_blocks(std::move(prev))
    , created_by_(collator_id)
    , validator_set_(std::move(validator_set))
    , manager(manager)
    , timeout(timeout)
    , main_promise(std::move(promise)) {
}

void Collator::start_up() {
  LOG(DEBUG) << "Collator for shard " << shard_.to_str() << " started";
  LOG(DEBUG) << "Previous block #1 is " << prev_blocks.at(0).to_str();
  if (prev_blocks.size() > 1) {
    LOG(DEBUG) << "Previous block #2 is " << prev_blocks.at(1).to_str();
  }
  if (is_hardfork_ && workchain() == masterchainId) {
    is_key_block_ = true;
  }
  // 1. check validity of parameters, especially prev_blocks, shard and min_mc_block_id
  if (workchain() != ton::masterchainId && workchain() != ton::basechainId) {
    fatal_error(-667, "can create block candidates only for masterchain (-1) and base workchain (0)");
    return;
  }
  if (is_busy()) {
    fatal_error(-666, "collator is busy creating another block candidate");
    return;
  }
  if (!shard_.is_valid_ext()) {
    fatal_error(-666, "requested to generate a block for an invalid shard");
    return;
  }
  td::uint64 x = td::lower_bit64(get_shard());
  if (x < 8) {
    fatal_error(-666, "cannot split a shard more than 60 times");
    return;
  }
  if (is_masterchain() && !shard_.is_masterchain_ext()) {
    fatal_error(-666, "sub-shards cannot exist in the masterchain");
    return;
  }
  if (!ShardIdFull(min_mc_block_id).is_masterchain_ext()) {
    fatal_error(-666, "requested minimal masterchain block id does not belong to masterchain");
    return;
  }
  if (prev_blocks.size() > 2) {
    fatal_error(-666, "cannot have more than two previous blocks");
    return;
  }
  if (!prev_blocks.size()) {
    fatal_error(-666, "must have one or two previous blocks to generate a next block");
    return;
  }
  if (prev_blocks.size() == 2) {
    if (is_masterchain()) {
      fatal_error(-666, "cannot merge shards in masterchain");
      return;
    }
    if (!(shard_is_parent(shard_, ShardIdFull(prev_blocks[0])) &&
          shard_is_parent(shard_, ShardIdFull(prev_blocks[1])) && prev_blocks[0].id.shard < prev_blocks[1].id.shard)) {
      fatal_error(
          -666, "the two previous blocks for a merge operation are not siblings or are not children of current shard");
      return;
    }
    for (const auto& blk : prev_blocks) {
      if (!blk.seqno()) {
        fatal_error(-666, "previous blocks for a block merge operation must have non-zero seqno");
        return;
      }
    }
    after_merge_ = true;
    LOG(INFO) << "AFTER_MERGE set for the new block of " << shard_.to_str();
  } else {
    CHECK(prev_blocks.size() == 1);
    // creating next block
    if (!ShardIdFull(prev_blocks[0]).is_valid_ext()) {
      fatal_error(-666, "previous block does not have a valid id");
      return;
    }
    if (ShardIdFull(prev_blocks[0]) != shard_) {
      after_split_ = true;
      right_child_ = ton::is_right_child(shard_);
      LOG(INFO) << "AFTER_SPLIT set for the new block of " << shard_.to_str() << " (generating "
                << (right_child_ ? "right" : "left") << " child)";
      if (!shard_is_parent(ShardIdFull(prev_blocks[0]), shard_)) {
        fatal_error(-666, "previous block does not belong to the shard we are generating a new block for");
        return;
      }
      if (is_masterchain()) {
        fatal_error(-666, "cannot split shards in masterchain");
        return;
      }
    }
    if (is_masterchain() && min_mc_block_id.seqno() > prev_blocks[0].seqno()) {
      fatal_error(-666,
                  "cannot refer to specified masterchain block because it is later than the immediately preceding "
                  "masterchain block");
      return;
    }
  }
  busy_ = true;
  step = 1;
  if (!is_masterchain()) {
    // 2. learn latest masterchain state and block id
    LOG(DEBUG) << "sending get_top_masterchain_state_block() to Manager";
    ++pending;
    if (!is_hardfork_) {
      td::actor::send_closure_later(manager, &ValidatorManager::get_top_masterchain_state_block,
                                    [self = get_self()](td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
                                      LOG(DEBUG) << "got answer to get_top_masterchain_state_block";
                                      td::actor::send_closure_later(std::move(self), &Collator::after_get_mc_state,
                                                                    std::move(res));
                                    });
    } else {
      td::actor::send_closure_later(
          manager, &ValidatorManager::get_shard_state_from_db_short, min_mc_block_id,
          [self = get_self(), block_id = min_mc_block_id](td::Result<Ref<ShardState>> res) {
            LOG(DEBUG) << "got answer to get_top_masterchain_state_block";
            if (res.is_error()) {
              td::actor::send_closure_later(std::move(self), &Collator::after_get_mc_state, res.move_as_error());
            } else {
              td::actor::send_closure_later(std::move(self), &Collator::after_get_mc_state,
                                            std::make_pair(Ref<MasterchainState>(res.move_as_ok()), block_id));
            }
          });
    }
  }
  // 3. load previous block(s) and corresponding state(s)
  prev_states.resize(prev_blocks.size());
  prev_block_data.resize(prev_blocks.size());
  for (int i = 0; (unsigned)i < prev_blocks.size(); i++) {
    // 3.1. load state
    LOG(DEBUG) << "sending wait_block_state() query #" << i << " for " << prev_blocks[i].to_str() << " to Manager";
    ++pending;
    td::actor::send_closure_later(manager, &ValidatorManager::wait_block_state_short, prev_blocks[i], priority(),
                                  timeout, [self = get_self(), i](td::Result<Ref<ShardState>> res) {
                                    LOG(DEBUG) << "got answer to wait_block_state query #" << i;
                                    td::actor::send_closure_later(std::move(self), &Collator::after_get_shard_state, i,
                                                                  std::move(res));
                                  });
    if (prev_blocks[i].seqno()) {
      // 3.2. load block
      // NB: we need the block itself only for extracting start_lt and end_lt to create correct prev_blk:ExtBlkRef and related Merkle proofs
      LOG(DEBUG) << "sending wait_block_data() query #" << i << " for " << prev_blocks[i].to_str() << " to Manager";
      ++pending;
      td::actor::send_closure_later(manager, &ValidatorManager::wait_block_data_short, prev_blocks[i], priority(),
                                    timeout, [self = get_self(), i](td::Result<Ref<BlockData>> res) {
                                      LOG(DEBUG) << "got answer to wait_block_data query #" << i;
                                      td::actor::send_closure_later(std::move(self), &Collator::after_get_block_data, i,
                                                                    std::move(res));
                                    });
    }
  }
  if (is_hardfork_) {
    LOG(WARNING) << "generating a hardfork block";
  }
  // 4. load external messages
  if (!is_hardfork_) {
    LOG(DEBUG) << "sending get_external_messages() query to Manager";
    ++pending;
    td::actor::send_closure_later(manager, &ValidatorManager::get_external_messages, shard_,
                                  [self = get_self()](td::Result<std::vector<Ref<ExtMessage>>> res) -> void {
                                    LOG(DEBUG) << "got answer to get_external_messages() query";
                                    td::actor::send_closure_later(
                                        std::move(self), &Collator::after_get_external_messages, std::move(res));
                                  });
  }
  if (is_masterchain() && !is_hardfork_) {
    // 5. load shard block info messages
    LOG(DEBUG) << "sending get_shard_blocks() query to Manager";
    ++pending;
    td::actor::send_closure_later(
        manager, &ValidatorManager::get_shard_blocks, prev_blocks[0],
        [self = get_self()](td::Result<std::vector<Ref<ShardTopBlockDescription>>> res) -> void {
          LOG(DEBUG) << "got answer to get_shard_blocks() query";
          td::actor::send_closure_later(std::move(self), &Collator::after_get_shard_blocks, std::move(res));
        });
  }
  // 6. set timeout
  alarm_timestamp() = timeout;
  CHECK(pending);
}

void Collator::alarm() {
  fatal_error(ErrorCode::timeout, "timeout");
}

std::string show_shard(ton::WorkchainId workchain, ton::ShardId shard) {
  char tmp[128];
  char* ptr = tmp + snprintf(tmp, 31, "%d:", workchain);
  if (!(shard & ((1ULL << 63) - 1))) {
    *ptr++ = '_';
    return {tmp, ptr};
  }
  while (shard & ((1ULL << 63) - 1)) {
    *ptr++ = ((long long)shard < 0) ? '1' : '0';
    shard <<= 1;
  }
  return {tmp, ptr};
}

std::string show_shard(const ton::BlockId blk_id) {
  return show_shard(blk_id.workchain, blk_id.shard);
}

std::string show_shard(const ton::ShardIdFull blk_id) {
  return show_shard(blk_id.workchain, blk_id.shard);
}

bool Collator::fatal_error(td::Status error) {
  error.ensure_error();
  LOG(ERROR) << "cannot generate block candidate for " << show_shard(shard_) << " : " << error.to_string();
  if (busy_) {
    main_promise(std::move(error));
    busy_ = false;
  }
  stop();
  return false;
}

bool Collator::fatal_error(int err_code, std::string err_msg) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

bool Collator::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

void Collator::check_pending() {
  // LOG(DEBUG) << "pending = " << pending;
  if (!pending) {
    step = 2;
    try {
      if (!try_collate()) {
        fatal_error("cannot create new block");
      }
    } catch (vm::VmError vme) {
      fatal_error(td::Status::Error(PSLICE() << vme.get_msg()));
    }
  }
}

bool Collator::register_mc_state(Ref<MasterchainStateQ> other_mc_state) {
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

bool Collator::request_aux_mc_state(BlockSeqno seqno, Ref<MasterchainStateQ>& state) {
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
                                  td::actor::send_closure_later(std::move(self), &Collator::after_get_aux_shard_state,
                                                                blkid, std::move(res));
                                });
  state.clear();
  return true;
}

Ref<MasterchainStateQ> Collator::get_aux_mc_state(BlockSeqno seqno) const {
  auto it = aux_mc_states_.find(seqno);
  if (it != aux_mc_states_.end()) {
    return it->second;
  } else {
    return {};
  }
}

void Collator::after_get_aux_shard_state(ton::BlockIdExt blkid, td::Result<Ref<ShardState>> res) {
  LOG(DEBUG) << "in Collator::after_get_aux_shard_state(" << blkid.to_str() << ")";
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
  check_pending();
}

bool Collator::preprocess_prev_mc_state() {
  LOG(DEBUG) << "in Collator::preprocess_prev_mc_state()";
  if (mc_state_.is_null()) {
    return fatal_error(-666, "unable to load latest masterchain state");
  }
  if (!ShardIdFull(mc_block_id_).is_masterchain_ext()) {
    return fatal_error(-666, "invalid last masterchain block id");
  }
  if (mc_block_id_.seqno() < min_mc_block_id.seqno()) {
    return fatal_error(-666, "requested to create a block referring to a non-existent future masterchain block");
  }
  if (mc_block_id_ != mc_state_->get_block_id()) {
    if (ShardIdFull(mc_block_id_) != ShardIdFull(mc_state_->get_block_id()) || mc_block_id_.seqno() != 0) {
      return fatal_error(-666, "latest masterchain state does not match latest masterchain block");
    }
  }
  mc_state_root = mc_state_->root_cell();
  if (mc_state_root.is_null()) {
    return fatal_error(-666, "latest masterchain state does not have a root cell");
  }
  if (!register_mc_state(mc_state_)) {
    return fatal_error(-666, "cannot register previous masterchain state");
  }
  return true;
}

void Collator::after_get_mc_state(td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
  LOG(DEBUG) << "in Collator::after_get_mc_state()";
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto state_blk = res.move_as_ok();
  mc_state_ = Ref<MasterchainStateQ>(std::move(state_blk.first));
  mc_block_id_ = state_blk.second;
  prev_mc_block_seqno = mc_block_id_.seqno();
  if (!preprocess_prev_mc_state()) {
    return;
  }
  if (mc_block_id_.seqno()) {
    // load most recent masterchain block itself
    // NB. it is needed only for creating a correct ExtBlkRef reference to it, which requires start_lt and end_lt
    LOG(DEBUG) << "sending wait_block_data() query #-1 for " << mc_block_id_.to_str() << " to Manager";
    ++pending;
    td::actor::send_closure_later(manager, &ValidatorManager::wait_block_data_short, mc_block_id_, priority(), timeout,
                                  [self = get_self()](td::Result<Ref<BlockData>> res) {
                                    LOG(DEBUG) << "got answer to wait_block_data query #-1";
                                    td::actor::send_closure_later(std::move(self), &Collator::after_get_block_data, -1,
                                                                  std::move(res));
                                  });
  }
  check_pending();
}

void Collator::after_get_shard_state(int idx, td::Result<Ref<ShardState>> res) {
  LOG(DEBUG) << "in Collator::after_get_shard_state(" << idx << ")";
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
    mc_block_id_ = prev_blocks[0];
    prev_mc_block_seqno = mc_block_id_.seqno();
    CHECK(ShardIdFull(mc_block_id_).is_masterchain_ext());
    mc_state_ = static_cast<Ref<MasterchainStateQ>>(prev_states[0]);
    mc_state_root = mc_state_->root_cell();
    if (!preprocess_prev_mc_state()) {
      return;
    }
  }
  check_pending();
}

void Collator::after_get_block_data(int idx, td::Result<Ref<BlockData>> res) {
  LOG(DEBUG) << "in Collator::after_get_block_data(" << idx << ")";
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  if (idx == -1) {
    // loaded last masterchain block
    prev_mc_block = res.move_as_ok();
    CHECK(prev_mc_block.not_null());
    CHECK(prev_mc_block->block_id() == mc_block_id_);
    mc_block_root = prev_mc_block->root_cell();
    CHECK(mc_block_root.not_null());
    CHECK(!is_masterchain());
  } else {
    // got previous block #i
    CHECK((unsigned)idx < prev_blocks.size());
    prev_block_data.at(idx) = res.move_as_ok();
    CHECK(prev_block_data[idx].not_null());
    CHECK(prev_block_data[idx]->block_id() == prev_blocks[idx]);
    CHECK(prev_block_data[idx]->root_cell().not_null());
    if (is_masterchain()) {
      CHECK(!idx);
      prev_mc_block = prev_block_data[0];
      mc_block_root = prev_mc_block->root_cell();
    }
  }
  check_pending();
}

void Collator::after_get_shard_blocks(td::Result<std::vector<Ref<ShardTopBlockDescription>>> res) {
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto vect = res.move_as_ok();
  shard_block_descr_ = std::move(vect);
  LOG(INFO) << "after_get_shard_blocks: got " << shard_block_descr_.size() << " ShardTopBlockDescriptions";
  check_pending();
}

bool Collator::unpack_last_mc_state() {
  auto res = block::ConfigInfo::extract_config(
      mc_state_root,
      block::ConfigInfo::needShardHashes | block::ConfigInfo::needLibraries | block::ConfigInfo::needValidatorSet |
          block::ConfigInfo::needWorkchainInfo | block::ConfigInfo::needCapabilities |
          (is_masterchain() ? block::ConfigInfo::needAccountsRoot | block::ConfigInfo::needSpecialSmc : 0));
  if (res.is_error()) {
    td::Status err = res.move_as_error();
    LOG(ERROR) << "cannot extract configuration from most recent masterchain state: " << err.to_string();
    return fatal_error(std::move(err));
  }
  config_ = res.move_as_ok();
  CHECK(config_);
  config_->set_block_id_ext(mc_block_id_);
  global_id_ = config_->get_global_blockchain_id();
  ihr_enabled_ = config_->ihr_enabled();
  create_stats_enabled_ = config_->create_stats_enabled();
  report_version_ = config_->has_capability(ton::capReportVersion);
  short_dequeue_records_ = config_->has_capability(ton::capShortDequeue);
  shard_conf_ = std::make_unique<block::ShardConfig>(*config_);
  prev_key_block_exists_ = config_->get_last_key_block(prev_key_block_, prev_key_block_lt_);
  if (prev_key_block_exists_) {
    prev_key_block_seqno_ = prev_key_block_.seqno();
  } else {
    prev_key_block_seqno_ = 0;
  }
  LOG(DEBUG) << "previous key block is " << prev_key_block_.to_str() << " (exists=" << prev_key_block_exists_ << ")";
  vert_seqno_ = config_->get_vert_seqno() + (is_hardfork_ ? 1 : 0);
  LOG(DEBUG) << "vertical seqno (vert_seqno) is " << vert_seqno_;
  auto limits = config_->get_block_limits(is_masterchain());
  if (limits.is_error()) {
    return fatal_error(limits.move_as_error());
  }
  block_limits_ = limits.move_as_ok();
  LOG(DEBUG) << "block limits: bytes [" << block_limits_->bytes.underload() << ", " << block_limits_->bytes.soft()
             << ", " << block_limits_->bytes.hard() << "]";
  LOG(DEBUG) << "block limits: gas [" << block_limits_->gas.underload() << ", " << block_limits_->gas.soft() << ", "
             << block_limits_->gas.hard() << "]";
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
  // TODO: extract start_lt and end_lt from prev_mc_block as well
  // std::cerr << "  block::gen::ShardState::print_ref(mc_state_root) = ";
  // block::gen::t_ShardState.print_ref(std::cerr, mc_state_root, 2);
  return true;
}

bool Collator::check_cur_validator_set() {
  if (is_hardfork_) {
    return true;
  }
  CatchainSeqno cc_seqno = 0;
  auto nodes = config_->compute_validator_set_cc(shard_, now_, &cc_seqno);
  if (nodes.empty()) {
    return fatal_error("cannot compute validator set for shard "s + shard_.to_str() + " from old masterchain state");
  }
  std::vector<ValidatorDescr> export_nodes;
  if (validator_set_.not_null()) {
    if (validator_set_->get_catchain_seqno() != cc_seqno) {
      return fatal_error(PSTRING() << "current validator set catchain seqno mismatch: this validator set has cc_seqno="
                                   << validator_set_->get_catchain_seqno() << ", only validator set with cc_seqno="
                                   << cc_seqno << " is entitled to create block in shardchain " << shard_.to_str());
    }
    export_nodes = validator_set_->export_vector();
  }
  if (export_nodes != nodes /* && !is_fake_ */) {
    return fatal_error(
        "current validator set mismatch: this validator set is not entitled to create block in shardchain "s +
        shard_.to_str());
  }
  return true;
}

bool Collator::request_neighbor_msg_queues() {
  assert(config_ && shard_conf_);
  auto neighbor_list = shard_conf_->get_neighbor_shard_hash_ids(shard_);
  LOG(DEBUG) << "got a preliminary list of " << neighbor_list.size() << " neighbors for " << shard_.to_str();
  for (ton::BlockId blk_id : neighbor_list) {
    auto shard_ptr = shard_conf_->get_shard_hash(ton::ShardIdFull(blk_id));
    if (shard_ptr.is_null()) {
      return fatal_error(-667, "cannot obtain shard hash for neighbor "s + blk_id.to_str());
    }
    if (shard_ptr->blk_.id != blk_id) {
      return fatal_error(-667, "invalid block id "s + shard_ptr->blk_.to_str() +
                                   " returned in information for neighbor " + blk_id.to_str());
    }
    neighbors_.emplace_back(*shard_ptr);
  }
  int i = 0;
  for (block::McShardDescr& descr : neighbors_) {
    LOG(DEBUG) << "neighbor #" << i << " : " << descr.blk_.to_str();
    ++pending;
    send_closure_later(manager, &ValidatorManager::wait_block_message_queue_short, descr.blk_, priority(), timeout,
                       [self = get_self(), i](td::Result<Ref<MessageQueue>> res) {
                         td::actor::send_closure(std::move(self), &Collator::got_neighbor_out_queue, i, std::move(res));
                       });
    ++i;
  }
  return true;
}

void Collator::got_neighbor_out_queue(int i, td::Result<Ref<MessageQueue>> res) {
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
  // comment the next two lines in the future when the output queues become huge
  //  CHECK(block::gen::t_OutMsgQueueInfo.validate_ref(1000000, outq_descr->root_cell()));
  //  CHECK(block::tlb::t_OutMsgQueueInfo.validate_ref(1000000, outq_descr->root_cell()));
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
  }
  check_pending();
}

bool Collator::unpack_merge_last_state() {
  LOG(DEBUG) << "unpack/merge last states";
  // 0. mechanically merge two ShardStateUnsplit into split_state constructor
  CHECK(prev_states.size() == 2);
  CHECK(prev_states.at(0).not_null() && prev_states.at(1).not_null());
  // create a virtual split_state ... = ShardState
  if (!block::gen::t_ShardState.cell_pack_split_state(prev_state_root_pure_, prev_states[0]->root_cell(),
                                                      prev_states[1]->root_cell())) {
    return fatal_error(-667, "cannot construct a virtual split_state after a merge");
  }
  // 1. prepare for creating a MerkleUpdate based on previous state
  state_usage_tree_ = std::make_shared<vm::CellUsageTree>();
  prev_state_root_ = vm::UsageCell::create(prev_state_root_pure_, state_usage_tree_->root_ptr());
  // 2. extract back slightly virtualized roots of the two original states
  Ref<vm::Cell> root0, root1;
  if (!block::gen::t_ShardState.cell_unpack_split_state(prev_state_root_, root0, root1)) {
    return fatal_error(-667, "cannot unsplit a virtualized virtual split_state after a merge");
  }
  // 3. unpack previous states
  // 3.1. unpack left ancestor
  block::ShardState ss0;
  if (!unpack_one_last_state(ss0, prev_blocks.at(0), std::move(root0))) {
    return fatal_error("cannot unpack the state of left ancestor "s + prev_blocks.at(0).to_str());
  }
  // 3.2. unpack right ancestor
  block::ShardState ss1;
  if (!unpack_one_last_state(ss1, prev_blocks.at(1), std::move(root1))) {
    return fatal_error("cannot unpack the state of right ancestor "s + prev_blocks.at(1).to_str());
  }
  // 4. merge the two ancestors of the current state
  LOG(INFO) << "merging the two previous states";
  auto res = ss0.merge_with(ss1);
  if (res.is_error()) {
    return fatal_error(std::move(res)) || fatal_error("cannot merge the two previous states");
  }
  return import_shard_state_data(ss0);
}

bool Collator::unpack_last_state() {
  if (after_merge_) {
    if (!unpack_merge_last_state()) {
      return fatal_error("unable to unpack/merge last states immediately after a merge");
    }
    return true;
  }
  CHECK(prev_states.size() == 1);
  CHECK(prev_states.at(0).not_null());
  prev_state_root_pure_ = prev_states.at(0)->root_cell();
  // prepare for creating a MerkleUpdate based on previous state
  state_usage_tree_ = std::make_shared<vm::CellUsageTree>();
  prev_state_root_ = vm::UsageCell::create(prev_state_root_pure_, state_usage_tree_->root_ptr());
  // unpack previous state
  block::ShardState ss;
  return unpack_one_last_state(ss, prev_blocks.at(0), prev_state_root_) && (!after_split_ || split_last_state(ss)) &&
         import_shard_state_data(ss);
}

bool Collator::unpack_one_last_state(block::ShardState& ss, BlockIdExt blkid, Ref<vm::Cell> prev_state_root) {
  auto res = ss.unpack_state_ext(blkid, std::move(prev_state_root), global_id_, prev_mc_block_seqno, after_split_,
                                 after_split_ | after_merge_, [self = this](ton::BlockSeqno mc_seqno) {
                                   Ref<MasterchainStateQ> state;
                                   return self->request_aux_mc_state(mc_seqno, state);
                                 });
  if (res.is_error()) {
    return fatal_error(std::move(res));
  }
  if (ss.vert_seqno_ > vert_seqno_) {
    return fatal_error(
        PSTRING() << "cannot create new block with vertical seqno " << vert_seqno_
                  << " prescribed by the current masterchain configuration because the previous state of shard "
                  << ss.id_.to_str() << " has larger vertical seqno " << ss.vert_seqno_);
  }
  return true;
}

bool Collator::split_last_state(block::ShardState& ss) {
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

// SETS: account_dict, shard_libraries_, mc_state_extra
//    total_balance_ = old_total_balance_, total_validator_fees_
// SETS: overload_history_, underload_history_
// SETS: prev_state_utime_, prev_state_lt_, prev_vert_seqno_
// SETS: out_msg_queue, processed_upto_, ihr_pending
bool Collator::import_shard_state_data(block::ShardState& ss) {
  account_dict = std::move(ss.account_dict_);
  shard_libraries_ = std::move(ss.shard_libraries_);
  mc_state_extra_ = std::move(ss.mc_state_extra_);
  overload_history_ = ss.overload_history_;
  underload_history_ = ss.underload_history_;
  prev_state_utime_ = ss.utime_;
  prev_state_lt_ = ss.lt_;
  prev_vert_seqno_ = ss.vert_seqno_;
  total_balance_ = old_total_balance_ = std::move(ss.total_balance_);
  value_flow_.from_prev_blk = old_total_balance_;
  total_validator_fees_ = std::move(ss.total_validator_fees_);
  old_global_balance_ = std::move(ss.global_balance_);
  out_msg_queue_ = std::move(ss.out_msg_queue_);
  processed_upto_ = std::move(ss.processed_upto_);
  ihr_pending = std::move(ss.ihr_pending_);
  block_create_stats_ = std::move(ss.block_create_stats_);
  return true;
}

bool Collator::add_trivial_neighbor_after_merge() {
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
        nb.set_queue_root(out_msg_queue_->get_root_cell());
        nb.processed_upto = processed_upto_;
        nb.blk_.id.shard = get_shard();
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

bool Collator::add_trivial_neighbor() {
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
  CHECK(prev_block_root.not_null());
  CHECK(prev_state_root_pure_.not_null());
  auto descr_ref = block::McShardDescr::from_block(prev_block_root, prev_state_root_pure_, prev_blocks[0].file_hash);
  if (descr_ref.is_null()) {
    return fatal_error("cannot deserialize header of previous state");
  }
  CHECK(descr_ref->blk_ == prev_blocks[0]);
  CHECK(out_msg_queue_);
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
          nb.set_queue_root(out_msg_queue_->get_root_cell());
          nb.processed_upto = processed_upto_;
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
          nb2.blk_.id.shard = ton::shard_sibling(get_shard());
          LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb2.blk_.to_str()
                     << " with shard shrinking to our sibling (immediate after-split adjustment)";
          auto& nb1 = neighbors_.at(n);
          nb1.set_queue_root(out_msg_queue_->get_root_cell());
          nb1.processed_upto = processed_upto_;
          nb1.blk_.id.shard = get_shard();
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
        pfx.bits().store_int(workchain(), 32);
        (pfx.bits() + 32).store_uint(get_shard(), 64);
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
        nb2.blk_.id.shard = ton::shard_sibling(get_shard());
        LOG(DEBUG) << "adjusted neighbor #" << i << " : " << nb2.blk_.to_str()
                   << " with shard shrinking to our sibling (continued after-split adjustment)";
        auto& nb1 = neighbors_.at(n);
        nb1.set_queue_root(out_msg_queue_->get_root_cell());
        nb1.processed_upto = processed_upto_;
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
          nb.set_queue_root(out_msg_queue_->get_root_cell());
          nb.processed_upto = processed_upto_;
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

bool Collator::check_prev_block(const BlockIdExt& listed, const BlockIdExt& prev, bool chk_chain_len) {
  if (listed.seqno() > prev.seqno()) {
    return fatal_error(PSTRING() << "cannot generate a shardchain block after previous block " << prev.to_str()
                                 << " because masterchain configuration already contains a newer block "
                                 << listed.to_str());
  }
  if (listed.seqno() == prev.seqno() && listed != prev) {
    return fatal_error(PSTRING() << "cannot generate a shardchain block after previous block " << prev.to_str()
                                 << " because masterchain configuration lists another block " << listed.to_str()
                                 << " of the same height");
  }
  if (chk_chain_len && prev.seqno() >= listed.seqno() + 8) {
    return fatal_error(PSTRING() << "cannot generate next block after " << prev.to_str()
                                 << " because this would lead to an unregistered chain of length > 8 (only "
                                 << listed.to_str() << " is registered in the masterchain)");
  }
  return true;
}

bool Collator::check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev) {
  if (listed != prev) {
    return fatal_error(PSTRING() << "cannot generate shardchain block for shard " << shard_.to_str()
                                 << " after previous block " << prev.to_str()
                                 << " because masterchain configuration expects another previous block "
                                 << listed.to_str() << " and we are immediately after a split/merge event");
  }
  return true;
}

bool Collator::check_this_shard_mc_info() {
  wc_info_ = config_->get_workchain_info(workchain());
  if (wc_info_.is_null()) {
    return fatal_error(PSTRING() << "cannot create new block for workchain " << workchain()
                                 << " absent from workchain configuration");
  }
  if (!wc_info_->active) {
    return fatal_error(PSTRING() << "cannot create new block for disabled workchain " << workchain());
  }
  if (!wc_info_->basic) {
    return fatal_error(PSTRING() << "cannot create new block for non-basic workchain " << workchain());
  }
  if (wc_info_->enabled_since && wc_info_->enabled_since > config_->utime) {
    return fatal_error(PSTRING() << "cannot create new block for workchain " << workchain()
                                 << " which is not enabled yet");
  }
  if (wc_info_->min_addr_len != 0x100 || wc_info_->max_addr_len != 0x100) {
    return false;
  }
  accept_msgs_ = wc_info_->accept_msgs;
  if (!config_->has_workchain(workchain())) {
    // creating first block for a new workchain
    LOG(INFO) << "creating first block for workchain " << workchain();
    return fatal_error(PSTRING() << "cannot create first block for workchain " << workchain()
                                 << " after previous block "
                                 << (prev_blocks.size() ? prev_blocks[0].to_str() : "(null)")
                                 << " because no shard for this workchain is declared yet");
  }
  auto left = config_->get_shard_hash(shard_ - 1, false);
  if (left.is_null()) {
    return fatal_error(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                 << " because there is no similar shard in existing masterchain configuration");
  }
  if (left->shard() == shard_) {
    // no split/merge
    if (after_merge_ || after_split_) {
      return fatal_error(
          PSTRING() << "cannot generate new shardchain block for " << shard_.to_str()
                    << " after a supposed split or merge event because this event is not reflected in the masterchain");
    }
    if (!check_prev_block(left->blk_, prev_blocks[0])) {
      return false;
    }
    if (left->before_split_) {
      return fatal_error(PSTRING() << "cannot generate new unsplit shardchain block for " << shard_.to_str()
                                   << " after previous block " << left->blk_.to_str() << " with before_split set");
    }
    auto sib = config_->get_shard_hash(shard_sibling(shard_));
    if (left->before_merge_ && sib->before_merge_) {
      return fatal_error(PSTRING() << "cannot generate new unmerged shardchain block for " << shard_.to_str()
                                   << " after both " << left->blk_.to_str() << " and " << sib->blk_.to_str()
                                   << " set before_merge flags");
    }
    if (left->is_fsm_split()) {
      auto tmp_now = std::max<td::uint32>(config_->utime, (unsigned)std::time(nullptr));
      if (shard_splitting_enabled && tmp_now >= left->fsm_utime() && tmp_now + 13 < left->fsm_utime_end()) {
        now_upper_limit_ = left->fsm_utime_end() - 11;  // ultimate value of now_ must be at most now_upper_limit_
        before_split_ = true;
        LOG(INFO) << "BEFORE_SPLIT set for the new block of shard " << shard_.to_str();
      }
    }
  } else if (shard_is_parent(shard_, left->shard())) {
    // after merge
    if (!left->before_merge_) {
      return fatal_error(PSTRING() << "cannot create new merged block for shard " << shard_.to_str()
                                   << " because its left ancestor " << left->blk_.to_str()
                                   << " has no before_merge flag");
    }
    auto right = config_->get_shard_hash(shard_ + 1, false);
    if (right.is_null()) {
      return fatal_error(
          PSTRING()
          << "cannot create new block for shard " << shard_.to_str()
          << " after a preceding merge because there is no right ancestor shard in existing masterchain configuration");
    }
    if (!shard_is_parent(shard_, right->shard())) {
      return fatal_error(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                   << " after a preceding merge because its right ancestor appears to be "
                                   << right->blk_.to_str());
    }
    if (!right->before_merge_) {
      return fatal_error(PSTRING() << "cannot create new merged block for shard " << shard_.to_str()
                                   << " because its right ancestor " << right->blk_.to_str()
                                   << " has no before_merge flag");
    }
    if (after_split_) {
      return fatal_error(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                   << " after a purported split because existing shard configuration suggests a merge");
    } else if (after_merge_) {
      if (!(check_prev_block_exact(left->blk_, prev_blocks[0]) &&
            check_prev_block_exact(right->blk_, prev_blocks[1]))) {
        return false;
      }
    } else {
      auto cseqno = std::max(left->seqno(), right->seqno());
      if (prev_blocks[0].seqno() <= cseqno) {
        return fatal_error(PSTRING() << "cannot create new block for shard " << shard_.to_str()
                                     << " after previous block " << prev_blocks[0].to_str()
                                     << " because masterchain contains newer possible ancestors " << left->blk_.to_str()
                                     << " and " << right->blk_.to_str());
      }
      if (prev_blocks[0].seqno() >= cseqno + 8) {
        return fatal_error(
            PSTRING() << "cannot create new block for shard " << shard_.to_str() << " after previous block "
                      << prev_blocks[0].to_str()
                      << " because this would lead to an unregistered chain of length > 8 (masterchain contains only "
                      << left->blk_.to_str() << " and " << right->blk_.to_str() << ")");
      }
    }
  } else if (shard_is_parent(left->shard(), shard_)) {
    // after split
    if (!left->before_split_) {
      return fatal_error(PSTRING() << "cannot generate new split shardchain block for " << shard_.to_str()
                                   << " after previous block " << left->blk_.to_str() << " without before_split");
    }
    if (after_merge_) {
      return fatal_error(PSTRING() << "cannot create new block for shard " << shard_.to_str()
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
    return fatal_error(PSTRING() << "masterchain configuration contains only block " << left->blk_.to_str()
                                 << " which belongs to a different shard from ours " << shard_.to_str());
  }
  return true;
}

bool Collator::init_block_limits() {
  CHECK(block_limits_);
  CHECK(state_usage_tree_);
  block_limits_->usage_tree = state_usage_tree_.get();
  block_limit_status_ = std::make_unique<block::BlockLimitStatus>(*block_limits_);
  return true;
}

bool Collator::do_preinit() {
  CHECK(prev_blocks.size() == 1U + after_merge_);
  last_block_seqno = prev_blocks[0].seqno();
  if (prev_block_data[0].not_null()) {
    CHECK(last_block_seqno);
    prev_block_root = prev_block_data[0]->root_cell();
  } else {
    CHECK(!last_block_seqno);
  }
  if (after_merge_ && prev_blocks[1].seqno() > last_block_seqno) {
    last_block_seqno = prev_blocks[1].seqno();
  }
  new_block_seqno = last_block_seqno + 1;
  new_id = ton::BlockId{shard_, new_block_seqno};
  CHECK(!config_);
  CHECK(mc_state_root.not_null());
  LOG(INFO) << "unpacking most recent masterchain state";
  if (!unpack_last_mc_state()) {
    return false;
  }
  CHECK(config_);
  if (config_->block_id.seqno() != prev_mc_block_seqno) {
    return fatal_error("loaded masterchain configuration has incorrect seqno");
  }
  if (!is_masterchain() && !check_this_shard_mc_info()) {
    return fatal_error("fatal error while checking masterchain configuration of the current shard");
  }
  if (!check_cur_validator_set()) {
    return fatal_error("this validator set is not entitled to create a block for this shardchain");
  }
  CHECK(!prev_mc_block_seqno || mc_block_root.not_null());
  if (!unpack_last_state()) {
    return fatal_error("cannot unpack previous state of current shardchain");
  }
  CHECK(account_dict);
  if (!init_utime()) {
    return fatal_error("cannot initialize unix time");
  }
  if (is_masterchain() && !adjust_shard_config()) {
    return fatal_error("cannot adjust shardchain configuration");
  }
  if (is_masterchain() && !import_new_shard_top_blocks()) {
    return fatal_error("cannot import new shard top block configuration");
  }
  if (!init_lt()) {
    return fatal_error("cannot initialize logical time");
  }
  if (!init_block_limits()) {
    return fatal_error("cannot initialize block limits");
  }
  if (!request_neighbor_msg_queues()) {
    return false;
  }
  return true;
}

bool Collator::adjust_shard_config() {
  CHECK(is_masterchain() && config_ && shard_conf_);
  const block::WorkchainSet& wset = config_->get_workchain_list();
  LOG(DEBUG) << "adjust_shard_config() started";
  fees_import_dict_ = std::make_unique<vm::AugmentedDictionary>(96, block::tlb::aug_ShardFees);
  int wc_act = 0;
  for (const auto& wpair : wset) {
    ton::WorkchainId wc = wpair.first;
    const block::WorkchainInfo* winfo = wpair.second.get();
    LOG(DEBUG) << "have workchain " << wc << " in configuration; active=" << winfo->active
               << ", enabled_since=" << winfo->enabled_since << ", now=" << now_;
    if (winfo->active && winfo->enabled_since <= now_) {
      if (!shard_conf_->has_workchain(wc)) {
        LOG(INFO) << "adding new workchain " << wc << " to shard configuration in masterchain state";
        ++wc_act;
        if (!shard_conf_->new_workchain(wc, new_block_seqno, winfo->zerostate_root_hash, winfo->zerostate_file_hash)) {
          return fatal_error(PSTRING() << "cannot add new workchain " << wc << " to shard configuration");
        }
        CHECK(store_shard_fees(ShardIdFull{wc}, block::CurrencyCollection{0}, block::CurrencyCollection{0}));
      }
    }
  }
  if (wc_act) {
    shard_conf_adjusted_ = true;
  }
  return true;
}

static bool cmp_shard_block_descr_ref(const Ref<ShardTopBlockDescription>& a, const Ref<ShardTopBlockDescription>& b) {
  BlockId x = a->block_id().id, y = b->block_id().id;
  return x.workchain < y.workchain ||
         (x.workchain == y.workchain && (x.shard < y.shard || (x.shard == y.shard && x.seqno > y.seqno)));
}

bool Collator::store_shard_fees(ShardIdFull shard, const block::CurrencyCollection& fees,
                                const block::CurrencyCollection& created) {
  if (shard.is_valid() && fees.is_valid()) {
    td::BitArray<96> key;
    key.bits().store_int(shard.workchain, 32);
    (key.bits() + 32).store_uint(shard.shard, 64);
    vm::CellBuilder cb;
    return fees.store(cb) &&
           created.store(cb)  // _ fees:CurrencyCollection create:CurrencyCollection = ShardFeeCreated;
           && fees_import_dict_->set(key, vm::load_cell_slice_ref(cb.finalize()), vm::Dictionary::SetMode::Add);
  } else {
    return false;
  }
}

bool Collator::store_shard_fees(Ref<block::McShardHash> descr) {
  CHECK(descr.not_null());
  CHECK(descr->fees_collected_.is_valid());
  CHECK(descr->funds_created_.is_valid());
  CHECK(store_shard_fees(descr->shard(), descr->fees_collected_, descr->funds_created_));
  return true;
}

bool Collator::import_new_shard_top_blocks() {
  if (shard_block_descr_.empty()) {
    return true;
  }
  if (skip_topmsgdescr_) {
    return true;
  }
  auto lt_limit = config_->lt + config_->get_max_lt_growth();
  std::sort(shard_block_descr_.begin(), shard_block_descr_.end(), cmp_shard_block_descr_ref);
  int tb_act = 0;
  Ref<ShardTopBlockDescrQ> prev_bd;
  Ref<block::McShardHash> prev_descr;
  ShardIdFull prev_shard{ton::workchainInvalid, ~0ULL};
  int prev_chain_len = 0;
  for (auto entry : shard_block_descr_) {
    auto sh_bd = Ref<ShardTopBlockDescrQ>(entry);
    CHECK(sh_bd.not_null());
    int res_flags = 0;
    auto chk_res = sh_bd->prevalidate(mc_block_id_, mc_state_,
                                      ShardTopBlockDescrQ::fail_new | ShardTopBlockDescrQ::fail_too_new, res_flags);
    if (chk_res.is_error()) {
      LOG(DEBUG) << "ShardTopBlockDescr for " << sh_bd->block_id().to_str() << " skipped: res_flags=" << res_flags
                 << " " << chk_res.move_as_error().to_string();
      continue;
    }
    int chain_len = chk_res.move_as_ok();
    if (chain_len <= 0 || chain_len > 8) {
      LOG(DEBUG) << "ShardTopBlockDescr for " << sh_bd->block_id().to_str() << " skipped: its chain length is "
                 << chain_len;
      continue;
    }
    if (sh_bd->generated_at() >= now_) {
      LOG(DEBUG) << "ShardTopBlockDescr for " << sh_bd->block_id().to_str() << " skipped: it claims to be generated at "
                 << sh_bd->generated_at() << " while it is still " << now_;
      continue;
    }
    Ref<block::McShardHash> descr = sh_bd->get_top_descr(chain_len);
    CHECK(descr.not_null());
    CHECK(descr->top_block_id() == sh_bd->block_id());
    ShardIdFull shard = ShardIdFull(descr->top_block_id());
    auto start_blks = sh_bd->get_prev_at(chain_len);
    auto res = shard_conf_->may_update_shard_block_info(descr, start_blks, lt_limit);
    if (res.is_error()) {
      LOG(DEBUG) << "cannot add new top shard block " << sh_bd->block_id().to_str()
                 << " to shard configuration: " << res.move_as_error().to_string();
      continue;
    }
    if (!res.move_as_ok()) {
      CHECK(start_blks.size() == 1);
      if (shard_is_sibling(prev_shard, shard)) {
        auto start_blks2 = prev_bd->get_prev_at(prev_chain_len);
        CHECK(start_blks.size() == 1);
        CHECK(start_blks2.size() == 1);
        CHECK(start_blks == start_blks2);
        prev_descr.write().set_reg_mc_seqno(new_block_seqno);
        descr.write().set_reg_mc_seqno(new_block_seqno);
        auto end_lt = std::max(prev_descr->end_lt_, descr->end_lt_);
        auto ures = shard_conf_->update_shard_block_info2(prev_descr, descr, std::move(start_blks2));
        if (ures.is_error()) {
          LOG(DEBUG) << "cannot add new split top shard blocks " << sh_bd->block_id().to_str() << " and "
                     << prev_bd->block_id().to_str() << " to shard configuration: " << ures.move_as_error().to_string();
          prev_descr.clear();
          descr.clear();
        } else {
          LOG(INFO) << "updated top shard block information with " << sh_bd->block_id().to_str() << " and "
                    << prev_bd->block_id().to_str();
          CHECK(ures.move_as_ok());
          store_shard_fees(std::move(prev_descr));
          store_shard_fees(std::move(descr));
          register_shard_block_creators(prev_bd->get_creator_list(prev_chain_len));
          register_shard_block_creators(sh_bd->get_creator_list(chain_len));
          used_shard_block_descr_.emplace_back(std::move(prev_bd));
          used_shard_block_descr_.emplace_back(sh_bd);
          tb_act += 2;
          prev_bd.clear();
          prev_descr.clear();
          prev_shard = ShardIdFull{};
          shards_max_end_lt_ = std::max(shards_max_end_lt_, end_lt);
        }
      } else if (shard == prev_shard) {
        LOG(DEBUG) << "skip postponing new top shard block " << sh_bd->block_id().to_str();
      } else {
        LOG(DEBUG) << "postpone adding new top shard block " << sh_bd->block_id().to_str();
        prev_bd = std::move(sh_bd);
        prev_descr = std::move(descr);
        prev_shard = shard;
        prev_chain_len = chain_len;
      }
      continue;
    }
    if (prev_bd.not_null()) {
      prev_bd.clear();
      prev_descr.clear();
      prev_shard = ShardIdFull{};
    }
    descr.write().set_reg_mc_seqno(new_block_seqno);
    auto end_lt = descr->end_lt_;
    auto ures = shard_conf_->update_shard_block_info(descr, std::move(start_blks));
    if (ures.is_error()) {
      LOG(DEBUG) << "cannot add new top shard block " << sh_bd->block_id().to_str()
                 << " to shard configuration: " << ures.move_as_error().to_string();
      descr.clear();
      continue;
    }
    store_shard_fees(std::move(descr));
    register_shard_block_creators(sh_bd->get_creator_list(chain_len));
    shards_max_end_lt_ = std::max(shards_max_end_lt_, end_lt);
    LOG(INFO) << "updated top shard block information with " << sh_bd->block_id().to_str();
    CHECK(ures.move_as_ok());
    ++tb_act;
    used_shard_block_descr_.emplace_back(sh_bd);
  }
  if (tb_act) {
    shard_conf_adjusted_ = true;
  }
  if (tb_act && verbosity >= 0) {  // DEBUG
    LOG(INFO) << "updated shard block configuration to ";
    auto csr = shard_conf_->get_root_csr();
    block::gen::t_ShardHashes.print(std::cerr, csr.write());
  }
  block::gen::ShardFeeCreated::Record fc;
  if (!(tlb::csr_unpack(fees_import_dict_->get_root_extra(),
                        fc)  // _ fees:CurrencyCollection create:CurrencyCollection = ShardFeeCreated;
        && value_flow_.fees_imported.validate_unpack(fc.fees) && import_created_.validate_unpack(fc.create))) {
    return fatal_error("cannot read the total imported fees from the augmentation of the root of ShardFees");
  }
  LOG(INFO) << "total fees_imported = " << value_flow_.fees_imported.to_str()
            << " ; out of them, total fees_created = " << import_created_.to_str();
  value_flow_.fees_collected += value_flow_.fees_imported;
  return true;
}

bool Collator::register_shard_block_creators(std::vector<td::Bits256> creator_list) {
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

bool Collator::try_collate() {
  if (!preinit_complete) {
    LOG(DEBUG) << "running do_preinit()";
    if (!do_preinit()) {
      return fatal_error(-667, "error preinitializing data required by collator");
    }
    preinit_complete = true;
  }
  if (pending) {
    return true;
  }
  CHECK(config_);
  last_proc_int_msg_.first = 0;
  last_proc_int_msg_.second.set_zero();
  first_unproc_int_msg_.first = ~0ULL;
  first_unproc_int_msg_.second.set_ones();
  if (is_masterchain()) {
    LOG(DEBUG) << "getting the list of special smart contracts";
    auto res = config_->get_special_smartcontracts();
    if (res.is_error()) {
      return fatal_error(res.move_as_error());
    }
    special_smcs = res.move_as_ok();
    LOG(DEBUG) << "have " << special_smcs.size() << " special smart contracts";
    for (auto addr : special_smcs) {
      LOG(DEBUG) << "special smart contract " << addr.to_hex();
    }
  }
  if (is_masterchain()) {
    LOG(DEBUG) << "getting the list of special tick-tock smart contracts";
    auto res = config_->get_special_ticktock_smartcontracts(3);
    if (res.is_error()) {
      return fatal_error(res.move_as_error());
    }
    ticktock_smcs = res.move_as_ok();
    LOG(DEBUG) << "have " << ticktock_smcs.size() << " tick-tock smart contracts";
    for (auto addr : ticktock_smcs) {
      LOG(DEBUG) << "special smart contract " << addr.first.to_hex() << " with ticktock=" << addr.second;
    }
  }
  if (is_masterchain() && prev_mc_block_seqno != last_block_seqno) {
    return fatal_error("Cannot generate new masterchain block unless most recent masterchain state is computed");
  }
  CHECK(processed_upto_);
  if (!fix_processed_upto(*processed_upto_)) {
    return fatal_error("Cannot adjust ProcessedUpto of our shard state");
  }
  if (sibling_processed_upto_ && !fix_processed_upto(*sibling_processed_upto_)) {
    return fatal_error("Cannot adjust ProcessedUpto of the shard state of our virtual sibling");
  }
  for (auto& descr : neighbors_) {
    CHECK(descr.processed_upto);
    if (!fix_processed_upto(*descr.processed_upto)) {
      return fatal_error(std::string{"Cannot adjust ProcessedUpto of neighbor "} + descr.blk_.to_str());
    }
  }
  return do_collate();
}

bool Collator::fix_one_processed_upto(block::MsgProcessedUpto& proc, const ton::ShardIdFull& owner) {
  if (proc.compute_shard_end_lt) {
    return true;
  }
  auto seqno = std::min(proc.mc_seqno, prev_mc_block_seqno);
  auto state = get_aux_mc_state(seqno);
  if (state.is_null()) {
    return fatal_error(
        -666, PSTRING() << "cannot obtain masterchain state with seqno " << seqno << " (originally required "
                        << proc.mc_seqno << ") in a MsgProcessedUpto record for "
                        << ton::ShardIdFull{owner.workchain, proc.shard}.to_str() << " owned by " << owner.to_str());
  }
  proc.compute_shard_end_lt = state->get_config()->get_compute_shard_end_lt_func();
  return (bool)proc.compute_shard_end_lt;
}

bool Collator::fix_processed_upto(block::MsgProcessedUptoCollection& upto) {
  for (auto& entry : upto.list) {
    if (!fix_one_processed_upto(entry, upto.owner)) {
      return false;
    }
  }
  return true;
}

bool Collator::init_utime() {
  CHECK(config_);
  // consider unixtime and lt from previous block(s) of the same shardchain
  prev_now_ = prev_state_utime_;
  auto prev = std::max<td::uint32>(config_->utime, prev_now_);
  now_ = std::max<td::uint32>(prev + 1, (unsigned)std::time(nullptr));
  if (now_ > now_upper_limit_) {
    return fatal_error(
        "error initializing unix time for the new block: failed to observe end of fsm_split time interval for this "
        "shard");
  }
  // check whether masterchain catchain rotation is overdue
  auto ccvc = config_->get_catchain_validators_config();
  unsigned lifetime = ccvc.mc_cc_lifetime;
  if (is_masterchain() && now_ / lifetime > prev_now_ / lifetime && now_ > (prev_now_ / lifetime + 1) * lifetime + 20) {
    auto overdue = now_ - (prev_now_ / lifetime + 1) * lifetime;
    // masterchain catchain rotation overdue, skip topsharddescr with some probability
    skip_topmsgdescr_ = (td::Random::fast(0, 1023) < 256);  // probability 1/4
    skip_extmsg_ = (td::Random::fast(0, 1023) < 256);       // skip ext msg probability 1/4
    if (skip_topmsgdescr_) {
      LOG(WARNING)
          << "randomly skipping import of new shard data because of overdue masterchain catchain rotation (overdue by "
          << overdue << " seconds)";
    }
    if (skip_extmsg_) {
      LOG(WARNING)
          << "randomly skipping external message import because of overdue masterchain catchain rotation (overdue by "
          << overdue << " seconds)";
    }
  } else if (is_masterchain() && now_ > prev_now_ + 60) {
    auto interval = now_ - prev_now_;
    skip_topmsgdescr_ = (td::Random::fast(0, 1023) < 128);  // probability 1/8
    skip_extmsg_ = (td::Random::fast(0, 1023) < 128);       // skip ext msg probability 1/8
    if (skip_topmsgdescr_) {
      LOG(WARNING) << "randomly skipping import of new shard data because of overdue masterchain block (last block was "
                   << interval << " seconds ago)";
    }
    if (skip_extmsg_) {
      LOG(WARNING) << "randomly skipping external message import because of overdue masterchain block (last block was "
                   << interval << " seconds ago)";
    }
  }
  return true;
}

bool Collator::init_lt() {
  CHECK(config_);
  start_lt = config_->lt;
  if (!is_masterchain()) {
    start_lt = std::max(start_lt, prev_state_lt_);
  } else {
    start_lt = std::max(start_lt, shards_max_end_lt_);
  }
  ton::LogicalTime align = config_->get_lt_align(), incr = align - start_lt % align;
  if (incr < align || !start_lt) {
    if (start_lt >= td::bits_negate64(incr)) {
      return fatal_error(
          td::Status::Error("cannot compute start logical time (uint64 overflow)"));  // cannot compute start lt
    }
    start_lt += incr;
  }
  LOG(INFO) << "start_lt set to " << start_lt;
  max_lt = start_lt + shard_conf_adjusted_;
  block_limits_->start_lt = start_lt;
  return true;
}

bool Collator::fetch_config_params() {
  auto res = impl_fetch_config_params(std::move(config_),
                                      &old_mparams_, &storage_prices_, &storage_phase_cfg_,
                                      &rand_seed_, &compute_phase_cfg_, &action_phase_cfg_,
                                      &masterchain_create_fee_, &basechain_create_fee_,
                                      workchain()
                                     );
  if (res.is_error()) {
      return fatal_error(res.move_as_error());
  }
  config_ = res.move_as_ok();
  return true;
}

td::Result<std::unique_ptr<block::ConfigInfo>>
           Collator::impl_fetch_config_params(std::unique_ptr<block::ConfigInfo> config,
                                              Ref<vm::Cell>* old_mparams,
                                              std::vector<block::StoragePrices>* storage_prices,
                                              block::StoragePhaseConfig* storage_phase_cfg,
                                              td::BitArray<256>* rand_seed,
                                              block::ComputePhaseConfig* compute_phase_cfg,
                                              block::ActionPhaseConfig* action_phase_cfg,
                                              td::RefInt256* masterchain_create_fee,
                                              td::RefInt256* basechain_create_fee,
                                              WorkchainId wc) {
  *old_mparams = config->get_config_param(9);
  {
    auto res = config->get_storage_prices();
    if (res.is_error()) {
      return res.move_as_error();
    }
    *storage_prices = res.move_as_ok();
  }
  {
    // generate rand seed
    prng::rand_gen().strong_rand_bytes(rand_seed->data(), 32);
    LOG(DEBUG) << "block random seed set to " << rand_seed->to_hex();
  }
  {
    // compute compute_phase_cfg / storage_phase_cfg
    auto cell = config->get_config_param(wc == ton::masterchainId ? 20 : 21);
    if (cell.is_null()) {
      return td::Status::Error(-668, "cannot fetch current gas prices and limits from masterchain configuration");
    }
    if (!compute_phase_cfg->parse_GasLimitsPrices(std::move(cell), storage_phase_cfg->freeze_due_limit,
                                                  storage_phase_cfg->delete_due_limit)) {
      return td::Status::Error(-668, "cannot unpack current gas prices and limits from masterchain configuration");
    }
    compute_phase_cfg->block_rand_seed = *rand_seed;
    compute_phase_cfg->libraries = std::make_unique<vm::Dictionary>(config->get_libraries_root(), 256);
    compute_phase_cfg->global_config = config->get_root_cell();
  }
  {
    // compute action_phase_cfg
    block::gen::MsgForwardPrices::Record rec;
    auto cell = config->get_config_param(24);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return td::Status::Error(-668, "cannot fetch masterchain message transfer prices from masterchain configuration");
    }
    action_phase_cfg->fwd_mc =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    cell = config->get_config_param(25);
    if (cell.is_null() || !tlb::unpack_cell(std::move(cell), rec)) {
      return td::Status::Error(-668, "cannot fetch standard message transfer prices from masterchain configuration");
    }
    action_phase_cfg->fwd_std =
        block::MsgPrices{rec.lump_price,           rec.bit_price,          rec.cell_price, rec.ihr_price_factor,
                         (unsigned)rec.first_frac, (unsigned)rec.next_frac};
    action_phase_cfg->workchains = &config->get_workchain_list();
    action_phase_cfg->bounce_msg_body = (config->has_capability(ton::capBounceMsgBody) ? 256 : 0);
  }
  {
    // fetch block_grams_created
    auto cell = config->get_config_param(14);
    if (cell.is_null()) {
      *basechain_create_fee = *masterchain_create_fee = td::zero_refint();
    } else {
      block::gen::BlockCreateFees::Record create_fees;
      if (!(tlb::unpack_cell(cell, create_fees) &&
            block::tlb::t_Grams.as_integer_to(create_fees.masterchain_block_fee, *masterchain_create_fee) &&
            block::tlb::t_Grams.as_integer_to(create_fees.basechain_block_fee, *basechain_create_fee))) {
        return td::Status::Error(-668, "cannot unpack BlockCreateFees from configuration parameter #14");
      }
    }
  }
  return std::move(config);
}

bool Collator::compute_minted_amount(block::CurrencyCollection& to_mint) {
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
  vm::Dictionary dict{vm::load_cell_slice(cell).prefetch_ref(), 32}, dict2{old_global_balance_.extra, 32}, dict3{32};
  if (!dict.check_for_each([this, &dict2, &dict3](Ref<vm::CellSlice> value, td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 32);
        int curr_id = (int)key.get_int(32);
        auto amount = block::tlb::t_VarUInteger_32.as_integer(value);
        if (amount.is_null() || !amount->is_valid()) {
          return fatal_error(PSTRING() << "cannot parse amount of currency #" << curr_id
                                       << " to be minted from configuration parameter #7");
        }
        auto value2 = dict2.lookup(key, 32);
        auto amount2 = value2.not_null() ? block::tlb::t_VarUInteger_32.as_integer(value2) : td::make_refint(0);
        if (amount2.is_null() || !amount2->is_valid()) {
          return fatal_error(PSTRING() << "cannot parse amount of currency #" << curr_id << " from old global balance");
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
                   fatal_error(PSTRING() << "cannot add " << delta << " of currency #" << curr_id << " to be minted");
          }
        }
        return true;
      })) {
    return fatal_error("error scanning extra currencies to be minted");
  }
  to_mint.extra = std::move(dict3).extract_root_cell();
  if (!to_mint.is_zero()) {
    LOG(INFO) << "new currencies to be minted: " << to_mint.to_str();
  }
  return true;
}

bool Collator::init_value_create() {
  value_flow_.created.set_zero();
  value_flow_.minted.set_zero();
  value_flow_.recovered.set_zero();
  if (is_masterchain()) {
    value_flow_.created = block::CurrencyCollection{masterchain_create_fee_};
    value_flow_.recovered = value_flow_.created + value_flow_.fees_collected + total_validator_fees_;
    auto cell = config_->get_config_param(3, 1);
    if (cell.is_null() || vm::load_cell_slice(cell).size_ext() != 0x100) {
      LOG(INFO) << "fee recovery disabled (no collector smart contract defined in configuration)";
      value_flow_.recovered.set_zero();
    } else if (value_flow_.recovered.grams < 1 * 1000000000LL /* 1 Gram */) {
      LOG(INFO) << "fee recovery skipped (" << value_flow_.recovered.to_str() << ")";
      value_flow_.recovered.set_zero();
    }
    if (!compute_minted_amount(value_flow_.minted)) {
      return fatal_error("cannot compute the amount of extra currencies to be minted");
    }
    cell = config_->get_config_param(2, 0);
    if (!value_flow_.minted.is_zero() && (cell.is_null() || vm::load_cell_slice(cell).size_ext() != 0x100)) {
      LOG(WARNING) << "minting of " << value_flow_.minted.to_str() << " disabled: no minting smart contract defined";
      value_flow_.minted.set_zero();
    }
  } else if (workchain() == basechainId) {
    value_flow_.created = block::CurrencyCollection{basechain_create_fee_ >> ton::shard_prefix_length(shard_)};
  }
  value_flow_.fees_collected += value_flow_.created;
  return true;
}

bool Collator::do_collate() {
  LOG(DEBUG) << "do_collate() : start";
  if (!fetch_config_params()) {
    return fatal_error("cannot fetch required configuration parameters from masterchain state");
  }
  LOG(DEBUG) << "config parameters fetched, creating message dictionaries";
  in_msg_dict = std::make_unique<vm::AugmentedDictionary>(256, block::tlb::aug_InMsgDescr);
  out_msg_dict = std::make_unique<vm::AugmentedDictionary>(256, block::tlb::aug_OutMsgDescr);
  LOG(DEBUG) << "message dictionaries created";
  if (max_lt == start_lt) {
    ++max_lt;
  }
  // NB: interchanged 1.2 and 1.1 (is this always correct?)
  // 1.1. re-adjust neighbors' out_msg_queues (for oneself)
  if (!add_trivial_neighbor()) {
    return fatal_error("cannot add previous block as a trivial neighbor");
  }
  // 1.2. delete delivered messages from output queue
  if (!out_msg_queue_cleanup()) {
    return fatal_error("cannot scan OutMsgQueue and remove already delivered messages");
  }
  // 1.3. create OutputQueueMerger from adjusted neighbors
  CHECK(!nb_out_msgs_);
  LOG(DEBUG) << "creating OutputQueueMerger";
  nb_out_msgs_ = std::make_unique<block::OutputQueueMerger>(shard_, neighbors_);
  // 1.4. compute created / minted / recovered
  if (!init_value_create()) {
    return fatal_error("cannot compute the value to be created / minted / recovered");
  }
  // 2. tick transactions
  LOG(INFO) << "create tick transactions";
  if (!create_ticktock_transactions(2)) {
    return fatal_error("cannot generate tick transactions");
  }
  if (is_masterchain() && !create_special_transactions()) {
    return fatal_error("cannot generate special transactions");
  }
  if (after_merge_) {
    // 3. merge prepare / merge install
    LOG(DEBUG) << "create merge prepare/install transactions (NOT IMPLEMENTED YET)";
    // TODO: implement merge prepare/install transactions for "large" smart contracts
    // ...
  }
  // 4. import inbound internal messages, process or transit
  LOG(INFO) << "process inbound internal messages";
  if (!process_inbound_internal_messages()) {
    return fatal_error("cannot process inbound internal messages");
  }
  // 5. import inbound external messages (if space&gas left)
  LOG(INFO) << "process inbound external messages";
  if (!process_inbound_external_messages()) {
    return fatal_error("cannot process inbound external messages");
  }
  // 6. process newly-generated messages (if space&gas left)
  //    (if we were unable to process all inbound messages, all new messages must be queued)
  LOG(INFO) << "process newly-generated messages";
  if (!process_new_messages(!inbound_queues_empty_)) {
    return fatal_error("cannot process newly-generated outbound messages");
  }
  if (before_split_) {
    // 7. split prepare / split install
    LOG(DEBUG) << "create split prepare/install transactions (NOT IMPLEMENTED YET)";
    // TODO: implement split prepare/install transactions for "large" smart contracts
    // ...
  }
  // 8. tock transactions
  LOG(INFO) << "create tock transactions";
  if (!create_ticktock_transactions(1)) {
    return fatal_error("cannot generate tock transactions");
  }
  // 9. process newly-generated messages (only by including them into output queue)
  LOG(INFO) << "enqueue newly-generated messages";
  if (!process_new_messages(true)) {
    return fatal_error("cannot process newly-generated outbound messages");
  }
  // 10. check block overload/underload
  LOG(DEBUG) << "check block overload/underload";
  if (!check_block_overload()) {
    return fatal_error("cannot check block overload/underload");
  }
  // 11. update public libraries
  if (is_masterchain()) {
    LOG(DEBUG) << "update public libraries";
    if (!update_public_libraries()) {
      return fatal_error("cannot update public libraries");
    }
  }
  // serialize everything
  // A. serialize ShardAccountBlocks and new ShardAccounts
  LOG(DEBUG) << "serialize account states and blocks";
  if (!combine_account_transactions()) {
    return fatal_error("cannot combine separate Account transactions into a new ShardAccountBlocks");
  }
  // B. serialize McStateExtra
  LOG(DEBUG) << "serialize McStateExtra";
  if (!create_mc_state_extra()) {
    return fatal_error("cannot create new McStateExtra");
  }
  // C. serialize ShardState
  LOG(DEBUG) << "serialize ShardState";
  if (!create_shard_state()) {
    return fatal_error("cannot create new ShardState");
  }
  // D. serialize Block
  LOG(DEBUG) << "serialize Block";
  if (!create_block()) {
    return fatal_error("cannot create new Block");
  }
  // E. create collated data
  if (!create_collated_data()) {
    return fatal_error("cannot create collated data for new Block candidate");
  }
  // F. create a block candidate
  LOG(DEBUG) << "create a Block candidate";
  if (!create_block_candidate()) {
    return fatal_error("cannot serialize a new Block candidate");
  }
  return true;
}

bool Collator::dequeue_message(Ref<vm::Cell> msg_envelope, ton::LogicalTime delivered_lt) {
  LOG(DEBUG) << "dequeueing outbound message";
  vm::CellBuilder cb;
  if (short_dequeue_records_) {
    td::BitArray<352> out_queue_key;
    return block::compute_out_msg_queue_key(msg_envelope, out_queue_key)  // (compute key)
           && cb.store_long_bool(13, 4)                                   // msg_export_deq_short$1101
           && cb.store_bits_bool(msg_envelope->get_hash().as_bitslice())  // msg_env_hash:bits256
           && cb.store_bits_bool(out_queue_key.bits(), 96)                // next_workchain:int32 next_addr_pfx:uint64
           && cb.store_long_bool(delivered_lt, 64)                        // import_block_lt:uint64
           && insert_out_msg(cb.finalize(), out_queue_key.bits() + 96);
  } else {
    return cb.store_long_bool(12, 4)                // msg_export_deq$1100
           && cb.store_ref_bool(msg_envelope)       // out_msg:^MsgEnvelope
           && cb.store_long_bool(delivered_lt, 63)  // import_block_lt:uint63
           && insert_out_msg(cb.finalize());
  }
}

bool Collator::out_msg_queue_cleanup() {
  LOG(INFO) << "cleaning outbound queue from messages already imported by neighbors";
  if (verbosity >= 2) {
    auto rt = out_msg_queue_->get_root();
    std::cerr << "old out_msg_queue is ";
    block::gen::t_OutMsgQueue.print(std::cerr, *rt);
    rt->print_rec(std::cerr);
  }
  for (const auto& nb : neighbors_) {
    if (!nb.is_disabled() && (!nb.processed_upto || !nb.processed_upto->can_check_processed())) {
      return fatal_error(-667, PSTRING() << "internal error: no info for checking processed messages from neighbor "
                                         << nb.blk_.to_str());
    }
  }

  auto res = out_msg_queue_->filter([&](vm::CellSlice& cs, td::ConstBitPtr key, int n) -> int {
    assert(n == 352);
    // LOG(DEBUG) << "key is " << key.to_hex(n);
    if (block_full_) {
      LOG(WARNING) << "BLOCK FULL while cleaning up outbound queue, cleanup completed only partially";
      outq_cleanup_partial_ = true;
      return (1 << 30) + 1;  // retain all remaining outbound queue entries including this one without processing
    }
    block::EnqueuedMsgDescr enq_msg_descr;
    unsigned long long created_lt;
    if (!(cs.fetch_ulong_bool(64, created_lt)  // augmentation
          && enq_msg_descr.unpack(cs)          // unpack EnqueuedMsg
          && enq_msg_descr.check_key(key)      // check key
          && enq_msg_descr.lt_ == created_lt)) {
      LOG(ERROR) << "cannot unpack EnqueuedMsg with key " << key.to_hex(n);
      return -1;
    }
    LOG(DEBUG) << "scanning outbound message with (lt,hash)=(" << enq_msg_descr.lt_ << ","
               << enq_msg_descr.hash_.to_hex() << ") enqueued_lt=" << enq_msg_descr.enqueued_lt_;
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
    if (delivered) {
      LOG(DEBUG) << "outbound message with (lt,hash)=(" << enq_msg_descr.lt_ << "," << enq_msg_descr.hash_.to_hex()
                 << ") enqueued_lt=" << enq_msg_descr.enqueued_lt_ << " has been already delivered, dequeueing";
      if (!dequeue_message(std::move(enq_msg_descr.msg_env_), deliver_lt)) {
        fatal_error(PSTRING() << "cannot dequeue outbound message with (lt,hash)=(" << enq_msg_descr.lt_ << ","
                              << enq_msg_descr.hash_.to_hex() << ") by inserting a msg_export_deq record");
        return -1;
      }
      register_out_msg_queue_op();
      if (!block_limit_status_->fits(block::ParamLimits::cl_normal)) {
        block_full_ = true;
      }
    }
    return !delivered;
  });
  LOG(DEBUG) << "deleted " << res << " messages from out_msg_queue";
  if (res < 0) {
    return fatal_error("error scanning/updating OutMsgQueue");
  }
  auto rt = out_msg_queue_->get_root();
  if (verbosity >= 2) {
    std::cerr << "new out_msg_queue is ";
    block::gen::t_OutMsgQueue.print(std::cerr, *rt);
    rt->print_rec(std::cerr);
  }
  // CHECK(block::gen::t_OutMsgQueue.validate_upto(100000, *rt));  // DEBUG, comment later if SLOW
  return register_out_msg_queue_op(true);
}

std::unique_ptr<block::Account> Collator::make_account_from(td::ConstBitPtr addr, Ref<vm::CellSlice> account,
                                                            Ref<vm::CellSlice> extra, bool force_create) {
  if (account.is_null() && !force_create) {
    return nullptr;
  }
  auto ptr = std::make_unique<block::Account>(workchain(), addr);
  if (account.is_null()) {
    if (!ptr->init_new(now_)) {
      return nullptr;
    }
  } else if (!ptr->unpack(std::move(account), std::move(extra), now_,
                          is_masterchain() && config_->is_special_smartcontract(addr))) {
    return nullptr;
  }
  ptr->block_lt = start_lt;
  return ptr;
}

block::Account* Collator::lookup_account(td::ConstBitPtr addr) const {
  auto found = accounts.find(addr);
  return found != accounts.end() ? found->second.get() : nullptr;
}

td::Result<block::Account*> Collator::make_account(td::ConstBitPtr addr, bool force_create) {
  auto found = lookup_account(addr);
  if (found) {
    return found;
  }
  auto dict_entry = account_dict->lookup_extra(addr, 256);
  if (dict_entry.first.is_null()) {
    if (!force_create) {
      return nullptr;
    }
  }
  auto new_acc = make_account_from(addr, std::move(dict_entry.first), std::move(dict_entry.second), force_create);
  if (!new_acc) {
    return td::Status::Error(PSTRING() << "cannot load account " << addr.to_hex(256) << " from previous state");
  }
  if (!new_acc->belongs_to_shard(shard_)) {
    return td::Status::Error(PSTRING() << "account " << addr.to_hex(256) << " does not really belong to current shard "
                                       << shard_.to_str());
  }
  auto ins = accounts.emplace(addr, std::move(new_acc));
  if (!ins.second) {
    return td::Status::Error(PSTRING() << "cannot insert newly-extracted account " << addr.to_hex(256)
                                       << "into account collection");
  }
  return ins.first->second.get();
}

bool Collator::combine_account_transactions() {
  vm::AugmentedDictionary dict{256, block::tlb::aug_ShardAccountBlocks};
  for (auto& z : accounts) {
    block::Account& acc = *(z.second);
    CHECK(acc.addr == z.first);
    if (!acc.transactions.empty()) {
      // have transactions for this account
      vm::CellBuilder cb;
      if (!acc.create_account_block(cb)) {
        return fatal_error("cannot create AccountBlock for account "s + z.first.to_hex());
      }
      auto cell = cb.finalize();
      auto csr = vm::load_cell_slice_ref(cell);
      if (verbosity > 2) {
        std::cerr << "new AccountBlock for " << z.first.to_hex() << ": ";
        block::gen::t_AccountBlock.print_ref(std::cerr, cell);
        csr->print_rec(std::cerr);
      }
      if (!block::gen::t_AccountBlock.validate_ref(100000, cell)) {
        block::gen::t_AccountBlock.print_ref(std::cerr, cell);
        csr->print_rec(std::cerr);
        return fatal_error(std::string{"new AccountBlock for "} + z.first.to_hex() +
                           " failed to pass automatic validation tests");
      }
      if (!block::tlb::t_AccountBlock.validate_ref(100000, cell)) {
        block::gen::t_AccountBlock.print_ref(std::cerr, cell);
        csr->print_rec(std::cerr);
        return fatal_error(std::string{"new AccountBlock for "} + z.first.to_hex() +
                           " failed to pass handwritten validation tests");
      }
      if (!dict.set(z.first, csr, vm::Dictionary::SetMode::Add)) {
        return fatal_error(std::string{"new AccountBlock for "} + z.first.to_hex() +
                           " could not be added to ShardAccountBlocks");
      }
      // update account_dict
      if (acc.total_state->get_hash() != acc.orig_total_state->get_hash()) {
        // account changed
        if (acc.orig_status == block::Account::acc_nonexist) {
          // account created
          CHECK(acc.status != block::Account::acc_nonexist);
          vm::CellBuilder cb;
          if (!(cb.store_ref_bool(acc.total_state)             // account_descr$_ account:^Account
                && cb.store_bits_bool(acc.last_trans_hash_)    // last_trans_hash:bits256
                && cb.store_long_bool(acc.last_trans_lt_, 64)  // last_trans_lt:uint64
                && account_dict->set_builder(acc.addr, cb, vm::Dictionary::SetMode::Add))) {
            return fatal_error(std::string{"cannot add newly-created account "} + acc.addr.to_hex() +
                               " into ShardAccounts");
          }
        } else if (acc.status == block::Account::acc_nonexist) {
          // account deleted
          if (verbosity > 2) {
            std::cerr << "deleting account " << acc.addr.to_hex() << " with empty new value ";
            block::gen::t_Account.print_ref(std::cerr, acc.total_state);
          }
          if (account_dict->lookup_delete(acc.addr).is_null()) {
            return fatal_error(std::string{"cannot delete account "} + acc.addr.to_hex() + " from ShardAccounts");
          }
        } else {
          // existing account modified
          if (verbosity > 4) {
            std::cerr << "modifying account " << acc.addr.to_hex() << " to ";
            block::gen::t_Account.print_ref(std::cerr, acc.total_state);
          }
          if (!(cb.store_ref_bool(acc.total_state)             // account_descr$_ account:^Account
                && cb.store_bits_bool(acc.last_trans_hash_)    // last_trans_hash:bits256
                && cb.store_long_bool(acc.last_trans_lt_, 64)  // last_trans_lt:uint64
                && account_dict->set_builder(acc.addr, cb, vm::Dictionary::SetMode::Replace))) {
            return fatal_error(std::string{"cannot modify existing account "} + acc.addr.to_hex() +
                               " in ShardAccounts");
          }
        }
      }
    } else {
      if (acc.total_state->get_hash() != acc.orig_total_state->get_hash()) {
        return fatal_error(std::string{"total state of account "} + z.first.to_hex() +
                           " miraculously changed without transactions");
      }
    }
  }
  vm::CellBuilder cb;
  if (!(cb.append_cellslice_bool(std::move(dict).extract_root()) && cb.finalize_to(shard_account_blocks_))) {
    return fatal_error("cannot serialize ShardAccountBlocks");
  }
  if (verbosity > 2) {
    std::cerr << "new ShardAccountBlocks: ";
    block::gen::t_ShardAccountBlocks.print_ref(std::cerr, shard_account_blocks_);
    vm::load_cell_slice(shard_account_blocks_).print_rec(std::cerr);
  }
  if (!block::gen::t_ShardAccountBlocks.validate_ref(100000, shard_account_blocks_)) {
    return fatal_error("new ShardAccountBlocks failed to pass automatic validity tests");
  }
  if (!block::tlb::t_ShardAccountBlocks.validate_ref(100000, shard_account_blocks_)) {
    return fatal_error("new ShardAccountBlocks failed to pass handwritten validity tests");
  }
  auto shard_accounts = account_dict->get_root();
  if (verbosity > 2) {
    std::cerr << "new ShardAccounts: ";
    block::gen::t_ShardAccounts.print(std::cerr, *shard_accounts);
    shard_accounts->print_rec(std::cerr);
  }
  if (verify >= 2) {
    LOG(INFO) << "verifying new ShardAccounts";
    if (!block::gen::t_ShardAccounts.validate_upto(100000, *shard_accounts)) {
      return fatal_error("new ShardAccounts failed to pass automatic validity tests");
    }
    if (!block::tlb::t_ShardAccounts.validate_upto(100000, *shard_accounts)) {
      return fatal_error("new ShardAccounts failed to pass handwritten validity tests");
    }
  }
  return true;
}

bool Collator::create_special_transaction(block::CurrencyCollection amount, Ref<vm::Cell> dest_addr_cell,
                                          Ref<vm::Cell>& in_msg) {
  if (amount.is_zero()) {
    return true;
  }
  CHECK(dest_addr_cell.not_null());
  ton::StdSmcAddress addr;
  CHECK(vm::load_cell_slice(dest_addr_cell).prefetch_bits_to(addr));
  LOG(INFO) << "creating special transaction to recover " << amount.to_str() << " to account " << addr.to_hex();
  CHECK(in_msg.is_null());
  ton::LogicalTime lt = start_lt;
  vm::CellBuilder cb;
  Ref<vm::Cell> msg;
  if (!(cb.store_long_bool(6, 4)          // int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
        && cb.store_long_bool(0x4ff, 11)  // addr_std$10 anycast:(Maybe Anycast) workchain_id:int8
        && cb.store_zeroes_bool(256)      //   address:bits256 => src:MsgAddressInt
        && cb.store_long_bool(0x4ff, 11)  // addr_std$10 anycast:(Maybe Anycast) workchain_id:int8
        && cb.store_bits_bool(addr)       //   address:bits256 => dest:MsgAddressInt
        && amount.store(cb)               // value:CurrencyCollection
        && cb.store_zeroes_bool(4 + 4)    // ihr_fee:Grams fwd_fee:Grams
        && cb.store_long_bool(lt, 64)     // created_lt:uint64
        && cb.store_long_bool(now_, 32)   // created_at:uint32
        && cb.store_zeroes_bool(2)        // init:(Maybe ...) body:(Either X ^X) = Message X
        && cb.finalize_to(msg))) {        // -> msg
    return fatal_error("cannot generate special internal message for recovering "s + amount.to_str() + " to account " +
                       addr.to_hex());
  }
  if (verbosity >= 4) {
    block::gen::t_Message_Any.print_ref(std::cerr, msg);
  }
  CHECK(block::gen::t_Message_Any.validate_ref(msg));
  CHECK(block::tlb::t_Message.validate_ref(msg));
  if (process_one_new_message(block::NewOutMsg{lt, msg, Ref<vm::Cell>{}}, false, &in_msg) != 1) {
    return fatal_error("cannot generate special transaction for recovering "s + amount.to_str() + " to account " +
                       addr.to_hex());
  }
  CHECK(in_msg.not_null());
  return true;
}

bool Collator::create_special_transactions() {
  CHECK(is_masterchain());
  return create_special_transaction(value_flow_.recovered, config_->get_config_param(3, 1), recover_create_msg_) &&
         create_special_transaction(value_flow_.minted, config_->get_config_param(2, 0), mint_msg_);
}

bool Collator::create_ticktock_transaction(const ton::StdSmcAddress& smc_addr, ton::LogicalTime req_start_lt,
                                           int mask) {
  auto acc_res = make_account(smc_addr.cbits(), false);
  if (acc_res.is_error()) {
    return fatal_error(acc_res.move_as_error());
  }
  block::Account* acc = acc_res.move_as_ok();
  assert(acc);
  if (acc->status != block::Account::acc_active) {
    // account not active, skip tick-tock transaction
    return true;
  }
  req_start_lt = std::max(req_start_lt, start_lt + 1);
  if (acc->last_trans_end_lt_ >= start_lt && acc->transactions.empty()) {
    return fatal_error(td::Status::Error(-666, PSTRING()
                                                   << "last transaction time in the state of account " << workchain()
                                                   << ":" << smc_addr.to_hex() << " is too large"));
  }
  std::unique_ptr<block::Transaction> trans = std::make_unique<block::Transaction>(
      *acc, mask == 2 ? block::Transaction::tr_tick : block::Transaction::tr_tock, req_start_lt, now_);
  if (!trans->prepare_storage_phase(storage_phase_cfg_, true)) {
    return fatal_error(td::Status::Error(
        -666, std::string{"cannot create storage phase of a new transaction for smart contract "} + smc_addr.to_hex()));
  }
  if (!trans->prepare_compute_phase(compute_phase_cfg_)) {
    return fatal_error(td::Status::Error(
        -666, std::string{"cannot create compute phase of a new transaction for smart contract "} + smc_addr.to_hex()));
  }
  if (!trans->compute_phase->accepted && trans->compute_phase->skip_reason == block::ComputePhase::sk_none) {
    return fatal_error(td::Status::Error(-666, std::string{"new tick-tock transaction for smart contract "} +
                                                   smc_addr.to_hex() +
                                                   " has not been accepted by the smart contract (?)"));
  }
  if (trans->compute_phase->success && !trans->prepare_action_phase(action_phase_cfg_)) {
    return fatal_error(td::Status::Error(
        -666, std::string{"cannot create action phase of a new transaction for smart contract "} + smc_addr.to_hex()));
  }
  if (!trans->serialize()) {
    return fatal_error(td::Status::Error(
        -666, std::string{"cannot serialize new transaction for smart contract "} + smc_addr.to_hex()));
  }
  if (!trans->update_limits(*block_limit_status_)) {
    return fatal_error(-666, "cannot update block limit status to include the new transaction");
  }
  if (trans->commit(*acc).is_null()) {
    return fatal_error(
        td::Status::Error(-666, std::string{"cannot commit new transaction for smart contract "} + smc_addr.to_hex()));
  }
  update_max_lt(acc->last_trans_end_lt_);
  register_new_msgs(*trans);
  return true;
}

Ref<vm::Cell> Collator::create_ordinary_transaction(Ref<vm::Cell> msg_root) {
  ton::StdSmcAddress addr;
  auto cs = vm::load_cell_slice(msg_root);
  bool external;
  Ref<vm::CellSlice> src, dest;
  switch (block::gen::t_CommonMsgInfo.get_tag(cs)) {
    case block::gen::CommonMsgInfo::ext_in_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(cs, info)) {
        LOG(DEBUG) << "cannot unpack inbound external message";
        return {};
      }
      dest = std::move(info.dest);
      external = true;
      break;
    }
    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
        fatal_error("cannot unpack internal message to be processed by an ordinary transaction");
        return {};
      }
      src = std::move(info.src);
      dest = std::move(info.dest);
      external = false;
      break;
    }
    default:
      fatal_error("cannot unpack message to be processed by an ordinary transaction");
      return {};
  }
  ton::WorkchainId wc;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(dest, wc, addr) || wc != workchain()) {
    return {};
  }
  LOG(DEBUG) << "inbound message to our smart contract " << addr.to_hex();
  auto acc_res = make_account(addr.cbits(), true);
  if (acc_res.is_error()) {
    fatal_error(acc_res.move_as_error());
    return {};
  }
  block::Account* acc = acc_res.move_as_ok();
  assert(acc);


  auto res = impl_create_ordinary_transaction(msg_root, acc, now_, start_lt,
                                                    &storage_phase_cfg_, &compute_phase_cfg_,
                                                    &action_phase_cfg_,
                                                    external, last_proc_int_msg_.first
                                                   );
  if(res.is_error()) {
    auto error = res.move_as_error();
    if(error.code() == -701) {
      // ignorable errors
      LOG(DEBUG) << error.message();
      return {};
    }
    fatal_error(std::move(error));
    return {};
  }
  std::unique_ptr<block::Transaction> trans = res.move_as_ok();

  if (!trans->update_limits(*block_limit_status_)) {
    fatal_error("cannot update block limit status to include the new transaction");
    return {};
  }
  auto trans_root = trans->commit(*acc);
  if (trans_root.is_null()) {
    fatal_error("cannot commit new transaction for smart contract "s + addr.to_hex());
    return {};
  }

  register_new_msgs(*trans);
  update_max_lt(acc->last_trans_end_lt_);
  return trans_root;
}

// If td::status::error_code == 669 - Fatal Error block can not be produced
// if td::status::error_code == 701 - Transaction can not be included into block, but it's ok (external or too early internal)
td::Result<std::unique_ptr<block::Transaction>> Collator::impl_create_ordinary_transaction(Ref<vm::Cell> msg_root,
                                                         block::Account* acc,
                                                         UnixTime utime, LogicalTime lt,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg,
                                                         bool external, LogicalTime after_lt) {
  if (acc->last_trans_end_lt_ >= lt && acc->transactions.empty()) {
    return td::Status::Error(-669, PSTRING() << "last transaction time in the state of account " << acc->workchain << ":" << acc->addr.to_hex()
                          << " is too large");
  }
  auto trans_min_lt = lt;
  if (external) {
    // transactions processing external messages must have lt larger than all processed internal messages
    trans_min_lt = std::max(trans_min_lt, after_lt);
  }

  std::unique_ptr<block::Transaction> trans =
      std::make_unique<block::Transaction>(*acc, block::Transaction::tr_ord, trans_min_lt + 1, utime, msg_root);
  bool ihr_delivered = false;  // FIXME
  if (!trans->unpack_input_msg(ihr_delivered, action_phase_cfg)) {
    if (external) {
      // inbound external message was not accepted
      return td::Status::Error(-701,"inbound external message rejected by account "s + acc->addr.to_hex() +
                                                           " before smart-contract execution");
      }
    return td::Status::Error(-669,"cannot unpack input message for a new transaction");
  }
  if (trans->bounce_enabled) {
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
    if (!external && !trans->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
  } else {
    if (!external && !trans->prepare_credit_phase()) {
      return td::Status::Error(-669,"cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true, true)) {
      return td::Status::Error(-669,"cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
      }
  }
  if (!trans->prepare_compute_phase(*compute_phase_cfg)) {
    return td::Status::Error(-669,"cannot create compute phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (!trans->compute_phase->accepted) {
    if (external) {
      // inbound external message was not accepted
      auto const& cp = *trans->compute_phase;
      return td::Status::Error(
          -701,
          PSLICE() << "inbound external message rejected by transaction " << acc->addr.to_hex() << ":\n" <<
              "exitcode=" << cp.exit_code << ", steps=" << cp.vm_steps << ", gas_used=" << cp.gas_used <<
              (cp.vm_log.empty() ? "" : "\nVM Log (truncated):\n..." + cp.vm_log));
      } else if (trans->compute_phase->skip_reason == block::ComputePhase::sk_none) {
        return td::Status::Error(-669,"new ordinary transaction for smart contract "s + acc->addr.to_hex() +
                  " has not been accepted by the smart contract (?)");
      }
  }
  if (trans->compute_phase->success && !trans->prepare_action_phase(*action_phase_cfg)) {
    return td::Status::Error(-669,"cannot create action phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (trans->bounce_enabled && !trans->compute_phase->success && !trans->prepare_bounce_phase(*action_phase_cfg)) {
    return td::Status::Error(-669,"cannot create bounce phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (!trans->serialize()) {
    return td::Status::Error(-669,"cannot serialize new transaction for smart contract "s + acc->addr.to_hex());
  }
  return std::move(trans);
}

void Collator::update_max_lt(ton::LogicalTime lt) {
  CHECK(lt >= start_lt);
  if (lt > max_lt) {
    max_lt = lt;
  }
}

bool Collator::update_last_proc_int_msg(const std::pair<ton::LogicalTime, ton::Bits256>& new_lt_hash) {
  if (last_proc_int_msg_ < new_lt_hash) {
    last_proc_int_msg_ = new_lt_hash;
    CHECK(new_lt_hash.first > 0);
    LOG(DEBUG) << "last_proc_int_msg updated to (" << new_lt_hash.first << ", " << new_lt_hash.second.to_hex() << ")";
    return true;
  } else {
    LOG(ERROR) << "processed message (" << new_lt_hash.first << ", " << new_lt_hash.second.to_hex()
               << ") AFTER message (" << last_proc_int_msg_.first << ", " << last_proc_int_msg_.second.to_hex() << ")";
    last_proc_int_msg_.first = std::numeric_limits<td::uint64>::max();
    return fatal_error("internal message processing order violated!");
  }
}

bool Collator::create_ticktock_transactions(int mask) {
  ton::LogicalTime req_lt = max_lt;
  for (auto smc_addr : special_smcs) {
    auto found = lookup_account(smc_addr.cbits());
    int ticktock = (found ? found->tick * 2 + found->tock : config_->get_smc_tick_tock(smc_addr.cbits()));
    if (ticktock >= 0 && (ticktock & mask)) {
      if (!create_ticktock_transaction(smc_addr, req_lt, mask)) {
        return false;
      }
    }
  }
  return true;
}

bool Collator::is_our_address(Ref<vm::CellSlice> addr_ref) const {
  return is_our_address(block::tlb::t_MsgAddressInt.get_prefix(std::move(addr_ref)));
}

bool Collator::is_our_address(ton::AccountIdPrefixFull addr_pfx) const {
  return ton::shard_contains(shard_, addr_pfx);
}

bool Collator::is_our_address(const ton::StdSmcAddress& addr) const {
  return ton::shard_contains(get_shard(), addr);
}

// 1 = processed, 0 = enqueued, 3 = processed, all future messages must be enqueued
int Collator::process_one_new_message(block::NewOutMsg msg, bool enqueue_only, Ref<vm::Cell>* is_special) {
  Ref<vm::CellSlice> src, dest;
  bool enqueue, external;
  auto cs = load_cell_slice(msg.msg);
  td::RefInt256 fwd_fees;
  int tag = block::gen::t_CommonMsgInfo.get_tag(cs);
  switch (tag) {
    case block::gen::CommonMsgInfo::ext_out_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info;
      if (!tlb::unpack(cs, info)) {
        return -1;
      }
      CHECK(info.created_lt == msg.lt && info.created_at == now_);
      src = std::move(info.src);
      enqueue = external = true;
      break;
    }
    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
        return -1;
      }
      CHECK(info.created_lt == msg.lt && info.created_at == now_);
      src = std::move(info.src);
      dest = std::move(info.dest);
      fwd_fees = block::tlb::t_Grams.as_integer(info.fwd_fee);
      CHECK(fwd_fees.not_null());
      external = false;
      enqueue = (enqueue_only || !is_our_address(dest));
      break;
    }
    default:
      return -1;
  }
  CHECK(is_our_address(std::move(src)));
  if (external) {
    // 1. construct a msg_export_ext OutMsg
    vm::CellBuilder cb;
    CHECK(cb.store_long_bool(0, 3)           // msg_export_ext$000
          && cb.store_ref_bool(msg.msg)      // msg:^(Message Any)
          && cb.store_ref_bool(msg.trans));  // transaction:^Transaction
    // 2. insert OutMsg into OutMsgDescr
    CHECK(insert_out_msg(cb.finalize()));  // OutMsg -> OutMsgDescr
    // (if ever a structure in the block for listing all external outbound messages appears, insert this message there as well)
    return 0;
  }
  if (enqueue) {
    auto lt = msg.lt;
    bool ok = enqueue_message(std::move(msg), std::move(fwd_fees), lt);
    return ok ? 0 : -1;
  }
  // process message by a transaction in this block:
  // 0. update last_proc_int_msg
  if (!is_special &&
      !update_last_proc_int_msg(std::pair<ton::LogicalTime, ton::Bits256>(msg.lt, msg.msg->get_hash().bits()))) {
    fatal_error("processing a message AFTER a newer message has been processed");
    return -1;
  }
  // 1. create a Transaction processing this Message
  auto trans_root = create_ordinary_transaction(msg.msg);
  if (trans_root.is_null()) {
    fatal_error("cannot create transaction for re-processing output message");
    return -1;
  }
  // 2. create a MsgEnvelope enveloping this Message
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(0x46060, 20)                         // msg_envelope#4 cur_addr:.. next_addr:..
        && block::tlb::t_Grams.store_integer_ref(cb, fwd_fees)  // fwd_fee_remaining:t_Grams
        && cb.store_ref_bool(msg.msg));                         // msg:^(Message Any)
  Ref<vm::Cell> msg_env = cb.finalize();
  if (verbosity > 2) {
    std::cerr << "new (processed outbound) message envelope: ";
    block::gen::t_MsgEnvelope.print_ref(std::cerr, msg_env);
  }
  // 3. create InMsg, referring to this MsgEnvelope and this Transaction
  CHECK(cb.store_long_bool(3, 3)                                  // msg_import_imm$011
        && cb.store_ref_bool(msg_env)                             // in_msg:^MsgEnvelope
        && cb.store_ref_bool(trans_root)                          // transaction:^Transaction
        && block::tlb::t_Grams.store_integer_ref(cb, fwd_fees));  // fwd_fee:Grams
  // 4. insert InMsg into InMsgDescr
  Ref<vm::Cell> in_msg = cb.finalize();
  if (!insert_in_msg(in_msg)) {
    return -1;
  }
  // 4.1. for special messages, return here
  if (is_special) {
    *is_special = in_msg;
    return 1;
  }
  // 5. create OutMsg, referring to this MsgEnvelope and InMsg
  CHECK(cb.store_long_bool(2, 3)         // msg_export_imm$010
        && cb.store_ref_bool(msg_env)    // out_msg:^MsgEnvelope
        && cb.store_ref_bool(msg.trans)  // transaction:^Transaction
        && cb.store_ref_bool(in_msg));   // reimport:^InMsg
  // 6. insert OutMsg into OutMsgDescr
  if (!insert_out_msg(cb.finalize())) {
    return -1;
  }
  // 7. check whether the block is full now
  if (!block_limit_status_->fits(block::ParamLimits::cl_normal)) {
    block_full_ = true;
    return 3;
  }
  return 1;
}

// very similar to enqueue_message(), but for transit messages
bool Collator::enqueue_transit_message(Ref<vm::Cell> msg, Ref<vm::Cell> old_msg_env,
                                       ton::AccountIdPrefixFull prev_prefix, ton::AccountIdPrefixFull cur_prefix,
                                       ton::AccountIdPrefixFull dest_prefix, td::RefInt256 fwd_fee_remaining,
                                       ton::LogicalTime enqueued_lt) {
  LOG(DEBUG) << "enqueueing transit message " << msg->get_hash().bits().to_hex(256);
  bool requeue = is_our_address(prev_prefix);
  // 1. perform hypercube routing
  auto route_info = block::perform_hypercube_routing(cur_prefix, dest_prefix, shard_);
  if ((unsigned)route_info.first > 96 || (unsigned)route_info.second > 96) {
    return fatal_error("cannot perform hypercube routing for a transit message");
  }
  // 2. compute our part of transit fees
  td::RefInt256 transit_fee = action_phase_cfg_.fwd_std.get_next_part(fwd_fee_remaining);
  fwd_fee_remaining -= transit_fee;
  CHECK(td::sgn(transit_fee) >= 0 && td::sgn(fwd_fee_remaining) >= 0);
  // 3. create a new MsgEnvelope
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(4, 4)                                         // msg_envelope#4 cur_addr:.. next_addr:..
        && cb.store_long_bool(route_info.first, 8)                       // cur_addr:IntermediateAddress
        && cb.store_long_bool(route_info.second, 8)                      // next_addr:IntermediateAddress
        && block::tlb::t_Grams.store_integer_ref(cb, fwd_fee_remaining)  // fwd_fee_remaining:t_Grams
        && cb.store_ref_bool(msg));                                      // msg:^(Message Any)
  Ref<vm::Cell> msg_env = cb.finalize();
  // 4. create InMsg
  CHECK(cb.store_long_bool(5, 3)                                     // msg_import_tr$101
        && cb.store_ref_bool(old_msg_env)                            // in_msg:^MsgEnvelope
        && cb.store_ref_bool(msg_env)                                // out_msg:^MsgEnvelope
        && block::tlb::t_Grams.store_integer_ref(cb, transit_fee));  // transit_fee:Grams
  Ref<vm::Cell> in_msg = cb.finalize();
  // 5. create a new OutMsg
  CHECK(cb.store_long_bool(requeue ? 7 : 3, 3)  // msg_export_tr$011 or msg_export_tr_req$111
        && cb.store_ref_bool(msg_env)           // out_msg:^MsgEnvelope
        && cb.store_ref_bool(in_msg));          // imported:^InMsg
  Ref<vm::Cell> out_msg = cb.finalize();
  // 4.1. insert OutMsg into OutMsgDescr
  if (verbosity > 2) {
    std::cerr << "OutMsg for a transit message: ";
    block::gen::t_OutMsg.print_ref(std::cerr, out_msg);
  }
  if (!insert_out_msg(out_msg)) {
    return fatal_error("cannot insert a new OutMsg into OutMsgDescr");
  }
  // 4.2. insert InMsg into InMsgDescr
  if (verbosity > 2) {
    std::cerr << "InMsg for a transit message: ";
    block::gen::t_InMsg.print_ref(std::cerr, in_msg);
  }
  if (!insert_in_msg(in_msg)) {
    return fatal_error("cannot insert a new InMsg into InMsgDescr");
  }
  // 5. create EnqueuedMsg
  CHECK(cb.store_long_bool(start_lt)     // _ enqueued_lt:uint64
        && cb.store_ref_bool(msg_env));  // out_msg:^MsgEnvelope = EnqueuedMsg;
  // 6. insert EnqueuedMsg into OutMsgQueue
  // NB: we use here cur_prefix instead of src_prefix; should we check that route_info.first >= next_addr.use_dest_bits of the old envelope?
  auto next_hop = block::interpolate_addr(cur_prefix, dest_prefix, route_info.second);
  td::BitArray<32 + 64 + 256> key;
  key.bits().store_int(next_hop.workchain, 32);
  (key.bits() + 32).store_int(next_hop.account_id_prefix, 64);
  (key.bits() + 96).copy_from(msg->get_hash().bits(), 256);
  bool ok;
  try {
    LOG(DEBUG) << "inserting into outbound queue message with (lt,key)=(" << start_lt << "," << key.to_hex() << ")";
    ok = out_msg_queue_->set_builder(key.bits(), 352, cb, vm::Dictionary::SetMode::Add);
  } catch (vm::VmError) {
    ok = false;
  }
  if (!ok) {
    LOG(ERROR) << "cannot add an OutMsg into OutMsgQueue dictionary!";
    return false;
  }
  return register_out_msg_queue_op();
}

bool Collator::delete_out_msg_queue_msg(td::ConstBitPtr key) {
  Ref<vm::CellSlice> queue_rec;
  try {
    LOG(DEBUG) << "deleting from outbound queue message with key=" << key.to_hex(352);
    queue_rec = out_msg_queue_->lookup_delete(key, 352);
  } catch (vm::VmError err) {
    LOG(ERROR) << "error deleting from out_msg_queue dictionary: " << err.get_msg();
  }
  if (queue_rec.is_null()) {
    return fatal_error(std::string{"cannot dequeue re-processed old message from OutMsgQueue using key "} +
                       key.to_hex(352));
  }
  return register_out_msg_queue_op();
}

bool Collator::process_inbound_message(Ref<vm::CellSlice> enq_msg, ton::LogicalTime lt, td::ConstBitPtr key,
                                       const block::McShardDescr& src_nb) {
  ton::LogicalTime enqueued_lt = 0;
  if (enq_msg.is_null() || enq_msg->size_ext() != 0x10040 ||
      (enqueued_lt = enq_msg->prefetch_ulong(64)) < /* 0 */ 1 * lt) {  // DEBUG
    if (enq_msg.not_null()) {
      block::gen::t_EnqueuedMsg.print(std::cerr, *enq_msg);
    }
    LOG(ERROR) << "inbound internal message is not a valid EnqueuedMsg (created lt " << lt << ", enqueued "
               << enqueued_lt << ")";
    return false;
  }
  auto msg_env = enq_msg->prefetch_ref();
  CHECK(msg_env.not_null());
  // 0. check MsgEnvelope
  if (msg_env->get_level() != 0) {
    LOG(ERROR) << "cannot import a message with non-zero level!";
    return false;
  }
  if (!block::gen::t_MsgEnvelope.validate_ref(msg_env)) {
    LOG(ERROR) << "inbound internal MsgEnvelope is invalid according to automated checks";
    return false;
  }
  if (!block::tlb::t_MsgEnvelope.validate_ref(msg_env)) {
    LOG(ERROR) << "inbound internal MsgEnvelope is invalid according to automated checks";
    return false;
  }
  // 1. unpack MsgEnvelope
  block::tlb::MsgEnvelope::Record_std env;
  if (!tlb::unpack_cell(msg_env, env)) {
    LOG(ERROR) << "cannot unpack MsgEnvelope of an inbound internal message";
    return false;
  }
  // 2. unpack CommonMsgInfo of the message
  vm::CellSlice cs{vm::NoVmOrd{}, env.msg};
  if (block::gen::t_CommonMsgInfo.get_tag(cs) != block::gen::CommonMsgInfo::int_msg_info) {
    LOG(ERROR) << "inbound internal message is not in fact internal!";
    return false;
  }
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  if (!tlb::unpack(cs, info)) {
    LOG(ERROR) << "cannot unpack CommonMsgInfo of an inbound internal message";
    return false;
  }
  if (info.created_lt != lt) {
    LOG(ERROR) << "inbound internal message has an augmentation value in source OutMsgQueue distinct from the one in "
                  "its contents";
    return false;
  }
  // 2.0. update last_proc_int_msg
  if (!update_last_proc_int_msg(std::pair<ton::LogicalTime, ton::Bits256>(lt, env.msg->get_hash().bits()))) {
    return fatal_error("processing a message AFTER a newer message has been processed");
  }
  // 2.1. check fwd_fee and fwd_fee_remaining
  td::RefInt256 orig_fwd_fee = block::tlb::t_Grams.as_integer(info.fwd_fee);
  if (env.fwd_fee_remaining > orig_fwd_fee) {
    LOG(ERROR) << "inbound internal message has fwd_fee_remaining=" << td::dec_string(env.fwd_fee_remaining)
               << " larger than original fwd_fee=" << td::dec_string(orig_fwd_fee);
    return false;
  }
  // 3. extract source and destination shards
  auto src_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.src);
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  if (!(src_prefix.is_valid() && dest_prefix.is_valid())) {
    LOG(ERROR) << "inbound internal message has invalid source or destination address";
    return false;
  }
  // 4. extrapolate current and next hop shards
  auto cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
  auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
  if (!(cur_prefix.is_valid() && next_prefix.is_valid())) {
    LOG(ERROR) << "inbound internal message has invalid source or destination address";
    return false;
  }
  // 5.1. cur_prefix must belong to the originating neighbor
  if (!ton::shard_contains(src_nb.shard(), cur_prefix)) {
    LOG(ERROR) << "inbound internal message does not have current address in the originating neighbor shard";
    return false;
  }
  // 5.2. next_prefix must belong to our shard
  if (!ton::shard_contains(shard_, next_prefix)) {
    LOG(ERROR) << "inbound internal message does not have next hop address in our shard";
    return false;
  }
  // 5.3. check the key -- it must consist of next_prefix + hash(msg)
  if (key.get_int(32) != next_prefix.workchain || (key + 32).get_uint(64) != next_prefix.account_id_prefix) {
    LOG(ERROR)
        << "inbound internal message has invalid key in OutMsgQueue : its first 96 bits differ from next_hop_addr";
    return false;
  }
  if (td::bitstring::bits_memcmp(key + 96, env.msg->get_hash().bits(), 256)) {
    LOG(ERROR)
        << "inbound internal message has invalid key in OutMsgQueue : its last 256 bits differ from the message hash";
    return false;
  }
  // 5.4. next_addr must be nearer to the destination than cur_addr
  if (env.cur_addr >= env.next_addr && env.next_addr < 96) {
    LOG(ERROR) << "inbound internal message has next hop address further from destination that current address";
    return false;
  }
  // 6. check whether we have already processed this message before using ProcessedUpTo (processed_upto)
  //    (then silently ignore this message; NB: it can be ours after merge)
  bool our = ton::shard_contains(shard_, cur_prefix);
  bool to_us = ton::shard_contains(shard_, dest_prefix);

  block::EnqueuedMsgDescr enq_msg_descr{cur_prefix, next_prefix, info.created_lt, enqueued_lt,
                                        env.msg->get_hash().bits()};
  if (processed_upto_->already_processed(enq_msg_descr)) {
    LOG(DEBUG) << "inbound internal message with lt=" << enq_msg_descr.lt_ << " hash=" << enq_msg_descr.hash_.to_hex()
               << " enqueued_lt=" << enq_msg_descr.enqueued_lt_ << " has been already processed by us before, skipping";
    // should we dequeue the message if it is ours (after a merge?)
    // (it should have been dequeued by out_msg_queue_cleanup() before)
    return true;
  }
  // 6.1. check whether we have already processed this message by IHR
  //      (then create a msg_discard_fin InMsg and remove record from IhrPendingInfo)
  // .. TODO ..
  // 7. decide what to do with the message
  if (!to_us) {
    // destination is outside our shard, relay transit message
    // (very similar to enqueue_message())
    if (!enqueue_transit_message(std::move(env.msg), std::move(msg_env), cur_prefix, next_prefix, dest_prefix,
                                 std::move(env.fwd_fee_remaining), max_lt)) {
      return fatal_error("cannot enqueue transit internal message with key "s + key.to_hex(352));
    }
    return !our || delete_out_msg_queue_msg(key);
  }
  // destination is in our shard
  // process the message by an ordinary transaction similarly to process_one_new_message()
  //
  // 8. create a Transaction processing this Message
  auto trans_root = create_ordinary_transaction(env.msg);
  if (trans_root.is_null()) {
    return fatal_error("cannot create transaction for processing inbound message");
  }
  // 9. create InMsg, referring to this MsgEnvelope and this Transaction
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(4, 3)                                               // msg_import_fin$100
        && cb.store_ref_bool(msg_env)                                          // in_msg:^MsgEnvelope
        && cb.store_ref_bool(trans_root)                                       // transaction:^Transaction
        && block::tlb::t_Grams.store_integer_ref(cb, env.fwd_fee_remaining));  // fwd_fee:Grams
  Ref<vm::Cell> in_msg = cb.finalize();
  if (our) {
    // if the message originates from the output queue of current shard, create a msg_export_deq_imm record
    // 10. create OutMsg with msg_export_deq_imm for dequeueing this message
    CHECK(cb.store_long_bool(4, 3)        // msg_export_deq_imm$100
          && cb.store_ref_bool(msg_env)   // out_msg:^MsgEnvelope
          && cb.store_ref_bool(in_msg));  // reimport:^InMsg
    // 11. insert OutMsg into OutMsgDescr
    if (!insert_out_msg(cb.finalize())) {
      return fatal_error("cannot insert a dequeueing OutMsg with msg_export_deq_imm constructor into OutMsgDescr");
    }
    // 12. delete message from OutMsgQueue
    if (!delete_out_msg_queue_msg(key)) {
      return fatal_error("cannot delete message from our own outbound queue after re-import");
    }
  }
  // 13. insert InMsg into InMsgDescr
  if (!insert_in_msg(std::move(in_msg))) {
    return fatal_error("cannot insert InMsg into InMsgDescr");
  }
  return true;
}

bool Collator::process_inbound_internal_messages() {
  while (!block_full_ && !nb_out_msgs_->is_eof()) {
    block_full_ = !block_limit_status_->fits(block::ParamLimits::cl_normal);
    if (block_full_) {
      LOG(INFO) << "BLOCK FULL, stop processing inbound internal messages";
      break;
    }
    auto kv = nb_out_msgs_->extract_cur();
    CHECK(kv && kv->msg.not_null());
    LOG(DEBUG) << "processing inbound message with (lt,hash)=(" << kv->lt << "," << kv->key.to_hex()
               << ") from neighbor #" << kv->source;
    if (verbosity > 2) {
      std::cerr << "inbound message: lt=" << kv->lt << " from=" << kv->source << " key=" << kv->key.to_hex() << " msg=";
      block::gen::t_EnqueuedMsg.print(std::cerr, *(kv->msg));
    }
    if (!process_inbound_message(kv->msg, kv->lt, kv->key.cbits(), neighbors_.at(kv->source))) {
      if (verbosity > 1) {
        std::cerr << "invalid inbound message: lt=" << kv->lt << " from=" << kv->source << " key=" << kv->key.to_hex()
                  << " msg=";
        block::gen::t_EnqueuedMsg.print(std::cerr, *(kv->msg));
      }
      return fatal_error("error processing inbound internal message");
    }
    nb_out_msgs_->next();
  }
  inbound_queues_empty_ = nb_out_msgs_->is_eof();
  return true;
}

bool Collator::process_inbound_external_messages() {
  if (skip_extmsg_) {
    LOG(INFO) << "skipping processing of inbound external messages";
    return true;
  }
  bool full = !block_limit_status_->fits(block::ParamLimits::cl_soft);
  for (auto& ext_msg_pair : ext_msg_list_) {
    if (full) {
      LOG(INFO) << "BLOCK FULL, stop processing external messages";
      break;
    }
    auto ext_msg = ext_msg_pair.first;
    ton::Bits256 hash{ext_msg->get_hash().bits()};
    int r = process_external_message(std::move(ext_msg));
    if (r < 0) {
      bad_ext_msgs_.emplace_back(ext_msg_pair.second);
      return false;
    }
    if (!r) {
      delay_ext_msgs_.emplace_back(ext_msg_pair.second);
    }
    if (r > 0) {
      full = !block_limit_status_->fits(block::ParamLimits::cl_soft);
    }
    auto it = ext_msg_map.find(hash);
    CHECK(it != ext_msg_map.end());
    it->second = (r >= 1 ? 3 : -2);  // processed or skipped
    if (r >= 3) {
      break;
    }
  }
  return true;
}

// 1 = processed, 0 = skipped, 3 = processed, all future messages must be skipped (block overflown)
int Collator::process_external_message(Ref<vm::Cell> msg) {
  auto cs = load_cell_slice(msg);
  td::RefInt256 fwd_fees;
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  if (!tlb::unpack(cs, info)) {
    return -1;
  }
  if (!is_our_address(info.dest)) {
    return 0;
  }
  // process message by a transaction in this block:
  // 1. create a Transaction processing this Message
  auto trans_root = create_ordinary_transaction(msg);
  if (trans_root.is_null()) {
    if (busy_) {
      // transaction rejected by account
      LOG(DEBUG) << "external message rejected by account, skipping";
      return 0;
    } else {
      fatal_error("cannot create transaction for processing inbound external message");
      return -1;
    }
  }
  // 2. create InMsg, referring to this Message and this Transaction
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(0, 3)            // msg_import_ext$000
        && cb.store_ref_bool(msg)           // in_msg:^(Message Any)
        && cb.store_ref_bool(trans_root));  // transaction:^Transaction
  Ref<vm::Cell> in_msg = cb.finalize();
  // 3. insert InMsg into InMsgDescr
  if (!insert_in_msg(std::move(in_msg))) {
    return -1;
  }
  return 1;
}

// inserts an InMsg into InMsgDescr
bool Collator::insert_in_msg(Ref<vm::Cell> in_msg) {
  if (verbosity > 2) {
    std::cerr << "InMsg being inserted into InMsgDescr: ";
    block::gen::t_InMsg.print_ref(std::cerr, in_msg);
  }
  auto cs = load_cell_slice(in_msg);
  if (!cs.size_refs()) {
    return false;
  }
  Ref<vm::Cell> msg = cs.prefetch_ref();
  int tag = (int)cs.prefetch_ulong(3);
  if (!(tag == 0 || tag == 2)) {  // msg_import_ext$000 or msg_import_ihr$010 contain (Message Any) directly
    // extract Message Any from MsgEnvelope to compute correct key
    auto cs2 = load_cell_slice(std::move(msg));
    if (!cs2.size_refs()) {
      return false;
    }
    msg = cs2.prefetch_ref();  // use hash of (Message Any)
  }
  bool ok;
  try {
    ok = in_msg_dict->set(msg->get_hash().bits(), 256, cs, vm::Dictionary::SetMode::Add);
  } catch (vm::VmError) {
    LOG(ERROR) << "cannot add an InMsg into InMsgDescr dictionary!";
    ok = false;
  }
  if (!ok) {
    return fatal_error("cannot add an InMsg into InMsgDescr dictionary");
  }
  ++in_descr_cnt_;
  return block_limit_status_->add_cell(std::move(in_msg)) &&
         ((in_descr_cnt_ & 63) || block_limit_status_->add_cell(in_msg_dict->get_root_cell()));
}

// inserts an OutMsg into OutMsgDescr
bool Collator::insert_out_msg(Ref<vm::Cell> out_msg) {
  if (verbosity > 2) {
    std::cerr << "OutMsg being inserted into OutMsgDescr: ";
    block::gen::t_OutMsg.print_ref(std::cerr, out_msg);
  }
  auto cs = load_cell_slice(out_msg);
  if (!cs.size_refs()) {
    return false;
  }
  Ref<vm::Cell> msg = cs.prefetch_ref();
  int tag = (int)cs.prefetch_ulong(3);
  if (!(tag == 0)) {  // msg_export_ext$000 contains (Message Any) directly
    // extract Message Any from MsgEnvelope to compute correct key
    auto cs2 = load_cell_slice(std::move(msg));
    if (!cs2.size_refs()) {
      return false;
    }
    msg = cs2.prefetch_ref();  // use hash of (Message Any)
  }
  return insert_out_msg(std::move(out_msg), msg->get_hash().bits());
}

bool Collator::insert_out_msg(Ref<vm::Cell> out_msg, td::ConstBitPtr msg_hash) {
  bool ok;
  try {
    ok = out_msg_dict->set(msg_hash, 256, load_cell_slice(std::move(out_msg)), vm::Dictionary::SetMode::Add);
  } catch (vm::VmError&) {
    ok = false;
  }
  if (!ok) {
    LOG(ERROR) << "cannot add an OutMsg into OutMsgDescr dictionary!";
    return false;
  }
  ++out_descr_cnt_;
  return block_limit_status_->add_cell(std::move(out_msg)) &&
         ((out_descr_cnt_ & 63) || block_limit_status_->add_cell(out_msg_dict->get_root_cell()));
}

// enqueues a new Message into OutMsgDescr and OutMsgQueue
bool Collator::enqueue_message(block::NewOutMsg msg, td::RefInt256 fwd_fees_remaining, ton::LogicalTime enqueued_lt) {
  // 0. unpack src_addr and dest_addr
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  if (!tlb::unpack_cell_inexact(msg.msg, info)) {
    return fatal_error("cannot enqueue a new message because it cannot be unpacked");
  }
  auto src_prefix = block::tlb::t_MsgAddressInt.get_prefix(std::move(info.src));
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(std::move(info.dest));
  if (!is_our_address(src_prefix)) {
    return fatal_error("cannot enqueue a new message because its source address does not belong to this shard");
  }
  if (!dest_prefix.is_valid()) {
    return fatal_error("cannot enqueue a new message because its destination shard is invalid");
  }
  // 1. perform hypercube routing
  auto route_info = block::perform_hypercube_routing(src_prefix, dest_prefix, shard_);
  if ((unsigned)route_info.first > 96 || (unsigned)route_info.second > 96) {
    return fatal_error("cannot perform hypercube routing for a new outbound message");
  }
  // 2. create a new MsgEnvelope
  vm::CellBuilder cb;
  CHECK(cb.store_long_bool(4, 4)                                          // msg_envelope#4 cur_addr:.. next_addr:..
        && cb.store_long_bool(route_info.first, 8)                        // cur_addr:IntermediateAddress
        && cb.store_long_bool(route_info.second, 8)                       // next_addr:IntermediateAddress
        && block::tlb::t_Grams.store_integer_ref(cb, fwd_fees_remaining)  // fwd_fee_remaining:t_Grams
        && cb.store_ref_bool(msg.msg));                                   // msg:^(Message Any)
  Ref<vm::Cell> msg_env = cb.finalize();
  // 3. create a new OutMsg
  CHECK(cb.store_long_bool(1, 3)           // msg_export_new$001
        && cb.store_ref_bool(msg_env)      // out_msg:^MsgEnvelope
        && cb.store_ref_bool(msg.trans));  // transaction:^Transaction
  Ref<vm::Cell> out_msg = cb.finalize();
  // 4. insert OutMsg into OutMsgDescr
  if (verbosity > 2) {
    std::cerr << "OutMsg for a newly-generated message: ";
    block::gen::t_OutMsg.print_ref(std::cerr, out_msg);
  }
  if (!insert_out_msg(out_msg)) {
    return fatal_error("cannot insert a new OutMsg into OutMsgDescr");
  }
  // 5. create EnqueuedMsg
  CHECK(cb.store_long_bool(enqueued_lt)  // _ enqueued_lt:uint64
        && cb.store_ref_bool(msg_env));  // out_msg:^MsgEnvelope = EnqueuedMsg;
  // 6. insert EnqueuedMsg into OutMsgQueue
  auto next_hop = block::interpolate_addr(src_prefix, dest_prefix, route_info.second);
  td::BitArray<32 + 64 + 256> key;
  key.bits().store_int(next_hop.workchain, 32);
  (key.bits() + 32).store_int(next_hop.account_id_prefix, 64);
  (key.bits() + 96).copy_from(msg.msg->get_hash().bits(), 256);
  bool ok;
  try {
    LOG(DEBUG) << "inserting into outbound queue a new message with (lt,key)=(" << start_lt << "," << key.to_hex()
               << ")";
    ok = out_msg_queue_->set_builder(key.bits(), 352, cb, vm::Dictionary::SetMode::Add);
  } catch (vm::VmError) {
    ok = false;
  }
  if (!ok) {
    LOG(ERROR) << "cannot add an OutMsg into OutMsgQueue dictionary!";
    return false;
  }
  return register_out_msg_queue_op();
}

bool Collator::process_new_messages(bool enqueue_only) {
  while (!new_msgs.empty()) {
    block::NewOutMsg msg = new_msgs.top();
    new_msgs.pop();
    if (block_full_ && !enqueue_only) {
      LOG(INFO) << "BLOCK FULL, enqueue all remaining new messages";
      enqueue_only = true;
    }
    LOG(DEBUG) << "have message with lt=" << msg.lt;
    int res = process_one_new_message(std::move(msg), enqueue_only);
    if (res < 0) {
      return fatal_error("error processing newly-generated outbound messages");
    } else if (res == 3) {
      LOG(INFO) << "All remaining new messages must be enqueued (BLOCK FULL)";
      enqueue_only = true;
    }
  }
  return true;
}

void Collator::register_new_msg(block::NewOutMsg new_msg) {
  if (new_msg.lt < min_new_msg_lt) {
    min_new_msg_lt = new_msg.lt;
  }
  new_msgs.push(std::move(new_msg));
}

void Collator::register_new_msgs(block::Transaction& trans) {
  CHECK(trans.root.not_null());
  for (unsigned i = 0; i < trans.out_msgs.size(); i++) {
    register_new_msg(trans.extract_out_msg_ext(i));
  }
}

/*
 *
 *  Generate (parts of) new state and block
 *
 */

bool store_ext_blk_ref_to(vm::CellBuilder& cb, const ton::BlockIdExt& id_ext, ton::LogicalTime end_lt) {
  return cb.store_long_bool(end_lt, 64)             // end_lt:uint64
         && cb.store_long_bool(id_ext.seqno(), 32)  // seq_no:uint32
         && cb.store_bits_bool(id_ext.root_hash)    // root_hash:bits256
         && cb.store_bits_bool(id_ext.file_hash);   // file_hash:bits256
}

bool store_ext_blk_ref_to(vm::CellBuilder& cb, const ton::BlockIdExt& id_ext, Ref<vm::Cell> blk_root) {
  block::gen::Block::Record rec;
  block::gen::BlockInfo::Record info;
  block::ShardId shard_id;
  return blk_root.not_null() &&
         !td::bitstring::bits_memcmp(id_ext.root_hash.bits(), blk_root->get_hash().bits(), 256) &&
         tlb::unpack_cell(blk_root, rec)                    // -> Block
         && tlb::unpack_cell(rec.info, info)                // -> info:BlockInfo
         && shard_id.deserialize(info.shard.write())        // -> shard:ShardId
         && (unsigned)info.seq_no == id_ext.seqno()         // seqno must match
         && shard_id == block::ShardId{id_ext.id}           // workchain and shard must match
         && store_ext_blk_ref_to(cb, id_ext, info.end_lt);  // store
}

static int update_one_shard(block::McShardHash& info, const block::McShardHash* sibling,
                            const block::WorkchainInfo* wc_info, ton::UnixTime now,
                            const block::CatchainValidatorsConfig& ccvc, bool update_cc) {
  bool changed = false;
  bool old_before_merge = info.before_merge_;
  info.before_merge_ = false;
  if (!info.is_fsm_none() && (now >= info.fsm_utime_end() || info.before_split_)) {
    info.clear_fsm();
    changed = true;
  } else if (info.is_fsm_merge() && (!sibling || sibling->before_split_)) {
    info.clear_fsm();
    changed = true;
  }
  if (wc_info && !info.before_split_) {
    // workchain present in configuration?
    unsigned depth = ton::shard_prefix_length(info.shard());
    if (info.is_fsm_none() && (info.want_split_ || depth < wc_info->min_split) && depth < wc_info->max_split &&
        depth < 60) {
      // prepare split
      info.set_fsm_split(now + ton::split_merge_delay, ton::split_merge_interval);
      changed = true;
      LOG(INFO) << "preparing to split shard " << info.shard().to_str() << " during " << info.fsm_utime() << " .. "
                << info.fsm_utime_end();
    } else if (info.is_fsm_none() && depth > wc_info->min_split && (info.want_merge_ || depth > wc_info->max_split) &&
               sibling && !sibling->before_split_ && sibling->is_fsm_none() &&
               (sibling->want_merge_ || depth > wc_info->max_split)) {
      // prepare merge
      info.set_fsm_merge(now + ton::split_merge_delay, ton::split_merge_interval);
      changed = true;
      LOG(INFO) << "preparing to merge shard " << info.shard().to_str() << " with " << sibling->shard().to_str()
                << " during " << info.fsm_utime() << " .. " << info.fsm_utime_end();
    } else if (info.is_fsm_merge() && depth > wc_info->min_split && sibling && !sibling->before_split_ &&
               sibling->is_fsm_merge() && now >= info.fsm_utime() && now >= sibling->fsm_utime() &&
               (depth > wc_info->max_split || (info.want_merge_ && sibling->want_merge_))) {
      // force merge
      info.before_merge_ = true;
      changed = true;
      LOG(INFO) << "force immediate merging of shard " << info.shard().to_str() << " with "
                << sibling->shard().to_str();
    }
  }
  if (info.before_merge_ != old_before_merge) {
    update_cc |= old_before_merge;
    changed = true;
  }
  if (update_cc) {
    info.next_catchain_seqno_++;
    changed = true;
  }
  return changed;
}

bool Collator::update_shard_config(const block::WorkchainSet& wc_set, const block::CatchainValidatorsConfig& ccvc,
                                   bool update_cc) {
  LOG(DEBUG) << "updating shard configuration (update_cc=" << update_cc << ")";
  WorkchainId wc_id{ton::workchainInvalid};
  Ref<block::WorkchainInfo> wc_info;
  ton::BlockSeqno& min_seqno = min_ref_mc_seqno_;
  return shard_conf_->process_sibling_shard_hashes(
      [&wc_set, &wc_id, &wc_info, &ccvc, &min_seqno, now = now_, update_cc](block::McShardHash& cur,
                                                                            const block::McShardHash* sibling) {
        if (!cur.is_valid()) {
          return -2;
        }
        if (wc_id != cur.workchain()) {
          wc_id = cur.workchain();
          auto it = wc_set.find(wc_id);
          if (it == wc_set.end()) {
            wc_info.clear();
          } else {
            wc_info = it->second;
          }
        }
        min_seqno = std::min(min_seqno, cur.min_ref_mc_seqno_);
        return update_one_shard(cur, sibling, wc_info.get(), now, ccvc, update_cc);
      });
}

bool Collator::create_mc_state_extra() {
  if (!is_masterchain()) {
    CHECK(mc_state_extra_.is_null());
    return true;
  }
  // should update mc_state_extra with a new McStateExtra
  block::gen::McStateExtra::Record state_extra;
  if (!tlb::unpack_cell(mc_state_extra_, state_extra)) {
    return fatal_error("cannot unpack previous McStateExtra");
  }
  // 1. update config:ConfigParams
  ton::StdSmcAddress config_addr;
  if (state_extra.config->size_ext() != 0x10100 || !state_extra.config->prefetch_bits_to(config_addr)) {
    return fatal_error("previous McStateExtra has invalid ConfigParams");
  }
  auto cfg_res = block::get_config_data_from_smc(account_dict->lookup(config_addr));
  if (cfg_res.is_error()) {
    return fatal_error("cannot obtain configuration from current configuration smart contract"s + config_addr.to_hex() +
                       " : " + cfg_res.move_as_error().to_string());
  }
  auto cfg_smc_config = cfg_res.move_as_ok();
  CHECK(cfg_smc_config.not_null());
  vm::Dictionary cfg_dict{cfg_smc_config, 32};
  bool ignore_cfg_changes = false;
  Ref<vm::Cell> cfg0;
  if (!block::valid_config_data(cfg_smc_config, config_addr, true, true, old_mparams_)) {
    block::gen::t_Hashmap_32_Ref_Cell.print_ref(std::cerr, cfg_smc_config);
    LOG(ERROR) << "configuration smart contract "s + config_addr.to_hex() +
                      " contains an invalid configuration in its data, IGNORING CHANGES";
    ignore_cfg_changes = true;
  } else {
    cfg0 = cfg_dict.lookup_ref(td::BitArray<32>{(long long) 0});
  }
  bool changed_cfg = false;
  if (cfg0.not_null()) {
    ton::StdSmcAddress new_config_addr;
    Ref<vm::Cell> new_cfg_smc_config;
    if (vm::load_cell_slice(cfg0).prefetch_bits_to(new_config_addr) && new_config_addr != config_addr &&
        try_fetch_new_config(new_config_addr, new_cfg_smc_config)) {
      LOG(WARNING) << "installing new configuration smart contract " << new_config_addr.to_hex();
      config_addr = new_config_addr;
      cfg_smc_config = new_cfg_smc_config;
      changed_cfg = true;
    }
  }
  if (ignore_cfg_changes) {
    LOG(ERROR) << "configuration changes ignored";
    return fatal_error("attempting to install invalid new configuration");
  } else if (block::important_config_parameters_changed(cfg_smc_config, state_extra.config->prefetch_ref()) ||
             changed_cfg) {
    LOG(WARNING) << "global configuration changed, updating";
    vm::CellBuilder cb;
    CHECK(cb.store_bits_bool(config_addr) && cb.store_ref_bool(cfg_smc_config));
    state_extra.config = vm::load_cell_slice_ref(cb.finalize());
    LOG(WARNING) << "marking new block as a key block";
    is_key_block_ = true;
  }
  new_config_params_ = state_extra.config;
  vm::Dictionary cfg_dict_new{new_config_params_->prefetch_ref(), 32};
  // 2. update shard_hashes and shard_fees
  auto ccvc = block::Config::unpack_catchain_validators_config(cfg_dict_new.lookup_ref(td::BitArray<32>{28}));
  auto wset_res = block::Config::unpack_workchain_list(cfg_dict_new.lookup_ref(td::BitArray<32>{12}));
  if (wset_res.is_error()) {
    return fatal_error(wset_res.move_as_error());
  }
  bool update_shard_cc = is_key_block_ || (now_ / ccvc.shard_cc_lifetime > prev_now_ / ccvc.shard_cc_lifetime);
  // temp debug
  if (verbosity >= 3 * 1) {
    auto csr = shard_conf_->get_root_csr();
    LOG(INFO) << "new shard configuration before post-processing is";
    std::ostringstream os;
    csr->print_rec(os);
    block::gen::t_ShardHashes.print(os, csr.write());
    LOG(INFO) << os.str();
  }
  // end (temp debug)
  if (!update_shard_config(wset_res.move_as_ok(), ccvc, update_shard_cc)) {
    auto csr = shard_conf_->get_root_csr();
    if (csr.is_null()) {
      LOG(WARNING) << "new shard configuration is null (!)";
    } else {
      LOG(WARNING) << "invalid new shard configuration is";
      std::ostringstream os;
      csr->print_rec(os);
      block::gen::t_ShardHashes.print(os, csr.write());
      LOG(WARNING) << os.str();
    }
    return fatal_error("cannot post-process shard configuration");
  }
  // 3. save new shard_hashes
  state_extra.shard_hashes = shard_conf_->get_root_csr();
  if (verbosity >= 3 * 0) {  // DEBUG
    std::cerr << "updated shard configuration to ";
    block::gen::t_ShardHashes.print(std::cerr, *state_extra.shard_hashes);
  }
  if (!block::gen::t_ShardHashes.validate_upto(10000, *state_extra.shard_hashes)) {
    return fatal_error("new ShardHashes is invalid");
  }
  // 4. check extension flags
  if (state_extra.r1.flags & ~1) {
    return fatal_error(PSTRING() << "previous McStateExtra has unknown extension flags set (" << state_extra.r1.flags
                                 << "), cannot handle these extensions");
  }
  // 5. update validator_info
  // (this algorithm should match one in MasterchainStateQ::get_next_validator_set()
  block::gen::ValidatorInfo::Record val_info;
  if (!tlb::csr_unpack(state_extra.r1.validator_info, val_info)) {
    return fatal_error("cannot unpack ValidatorInfo from previous state");
  }
  auto cur_vset_cell = cfg_dict_new.lookup_ref(td::BitArray<32>{35});
  if (cur_vset_cell.is_null()) {
    cur_vset_cell = cfg_dict_new.lookup_ref(td::BitArray<32>{34});
  }
  auto res = block::Config::unpack_validator_set(std::move(cur_vset_cell));
  if (res.is_error()) {
    auto err = res.move_as_error();
    LOG(ERROR) << "cannot unpack current validator set: " << err.to_string();
    return fatal_error(std::move(err));
  }
  auto cur_validators = res.move_as_ok();
  LOG_CHECK(cur_validators) << "unpacked current validator set is empty";

  unsigned lifetime = ccvc.mc_cc_lifetime;
  bool cc_updated = false;
  if (is_key_block_ || now_ / lifetime > prev_now_ / lifetime) {
    val_info.catchain_seqno++;
    cc_updated = true;
    LOG(INFO) << "increased masterchain catchain seqno to " << val_info.catchain_seqno;
  }
  auto nodes = block::Config::do_compute_validator_set(ccvc, shard_, *cur_validators, now_, val_info.catchain_seqno);
  LOG_CHECK(!nodes.empty()) << "validator node list in unpacked validator set is empty";

  auto vlist_hash = block::compute_validator_set_hash(/* val_info.catchain_seqno */ 0, shard_, std::move(nodes));
  LOG(INFO) << "masterchain validator set hash changed from " << val_info.validator_list_hash_short << " to "
            << vlist_hash;
  val_info.nx_cc_updated = cc_updated & update_shard_cc;
  // cc_updated |= (val_info.validator_list_hash_short != vlist_hash);
  val_info.validator_list_hash_short = vlist_hash;

  if (!tlb::csr_pack(state_extra.r1.validator_info, val_info)) {
    LOG(ERROR) << "cannot pack new ValidatorInfo";
    return false;
  }
  // ...
  // 6. update prev_blocks
  CHECK(new_block_seqno > 0 && new_block_seqno == last_block_seqno + 1);
  vm::AugmentedDictionary dict{state_extra.r1.prev_blocks, 32, block::tlb::aug_OldMcBlocksInfo};
  vm::CellBuilder cb;
  LOG(DEBUG) << "previous state is a key state: " << config_->is_key_state();
  CHECK(cb.store_bool_bool(config_->is_key_state()) && store_prev_blk_ref(cb, false) &&
        dict.set_builder(td::BitArray<32>(last_block_seqno), cb, vm::Dictionary::SetMode::Add));
  state_extra.r1.prev_blocks = std::move(dict).extract_root();
  cb.reset();
  // 7. update after_key_block:Bool and last_key_block:(Maybe ExtBlkRef)
  state_extra.r1.after_key_block = is_key_block_;
  if (prev_key_block_exists_) {
    // have non-trivial previous key block
    LOG(DEBUG) << "previous key block is " << prev_key_block_.to_str() << " lt " << prev_key_block_lt_;
    CHECK(cb.store_bool_bool(true) && store_ext_blk_ref_to(cb, prev_key_block_, prev_key_block_lt_));
  } else if (config_->is_key_state()) {
    LOG(DEBUG) << "setting previous key block to the previous block " << prev_blocks.at(0).to_str() << " lt "
               << config_->lt;
    CHECK(cb.store_bool_bool(true) && store_ext_blk_ref_to(cb, prev_blocks.at(0), config_->lt));
  } else {
    LOG(DEBUG) << "have no previous key block";
    CHECK(cb.store_bool_bool(false));
    if (state_extra.r1.last_key_block->size() > 1) {
      return fatal_error("cannot have no last key block after a state with last key block");
    }
  }
  state_extra.r1.last_key_block = vm::load_cell_slice_ref(cb.finalize());
  // 8. update global balance
  global_balance_ = old_global_balance_;
  global_balance_ += value_flow_.created;
  global_balance_ += value_flow_.minted;
  global_balance_ += import_created_;
  LOG(INFO) << "Global balance is " << global_balance_.to_str();
  if (!global_balance_.pack_to(state_extra.global_balance)) {
    return fatal_error("cannot store global_balance");
  }
  // 9. update block creator stats
  if (!update_block_creator_stats()) {
    return fatal_error("cannot update BlockCreateStats in new masterchain state");
  }
  state_extra.r1.flags = (state_extra.r1.flags & ~1) | create_stats_enabled_;
  if (state_extra.r1.flags & 1) {
    vm::CellBuilder cb;
    // block_create_stats#17 counters:(HashmapE 256 CreatorStats) = BlockCreateStats;
    CHECK(cb.store_long_bool(0x17, 8) && cb.append_cellslice_bool(block_create_stats_->get_root()));
    auto cs = vm::load_cell_slice_ref(cb.finalize());
    state_extra.r1.block_create_stats = cs;
    if (verify >= 2) {
      LOG(INFO) << "verifying new BlockCreateStats";
      if (!block::gen::t_BlockCreateStats.validate_csr(100000, cs)) {
        cs->print_rec(std::cerr);
        block::gen::t_BlockCreateStats.print(std::cerr, *cs);
        return fatal_error("BlockCreateStats in the new masterchain state failed to pass automated validity checks");
      }
    }
    if (verbosity >= 4 * 1) {
      block::gen::t_BlockCreateStats.print(std::cerr, *cs);
    }
  } else {
    state_extra.r1.block_create_stats.clear();
  }
  // 10. pack new McStateExtra
  if (!tlb::pack(cb, state_extra) || !cb.finalize_to(mc_state_extra_)) {
    return fatal_error("cannot pack new McStateExtra");
  }
  if (verify >= 2) {
    LOG(INFO) << "verifying new McStateExtra";
    CHECK(block::gen::t_McStateExtra.validate_ref(1000000, mc_state_extra_));
    CHECK(block::tlb::t_McStateExtra.validate_ref(1000000, mc_state_extra_));
  }
  LOG(INFO) << "McStateExtra created";
  return true;
}

bool Collator::update_block_creator_count(td::ConstBitPtr key, unsigned shard_incr, unsigned mc_incr) {
  LOG(DEBUG) << "increasing CreatorStats for " << key.to_hex(256) << " by (" << mc_incr << ", " << shard_incr << ")";
  block::DiscountedCounter mc_cnt, shard_cnt;
  auto cs = block_create_stats_->lookup(key, 256);
  if (!block::unpack_CreatorStats(std::move(cs), mc_cnt, shard_cnt)) {
    return fatal_error("cannot unpack CreatorStats for "s + key.to_hex(256) + " from previous masterchain state");
  }
  // std::cerr << mc_cnt.to_str() << " " << shard_cnt.to_str() << std::endl;
  if (mc_incr && !mc_cnt.increase_by(mc_incr, now_)) {
    return fatal_error(PSTRING() << "cannot increase masterchain block counter in CreatorStats for " << key.to_hex(256)
                                 << " by " << mc_incr << " (old value is " << mc_cnt.to_str() << ")");
  }
  if (shard_incr && !shard_cnt.increase_by(shard_incr, now_)) {
    return fatal_error(PSTRING() << "cannot increase shardchain block counter in CreatorStats for " << key.to_hex(256)
                                 << " by " << shard_incr << " (old value is " << shard_cnt.to_str() << ")");
  }
  vm::CellBuilder cb;
  if (!block::store_CreatorStats(cb, mc_cnt, shard_cnt)) {
    return fatal_error("cannot serialize new CreatorStats for "s + key.to_hex(256));
  }
  if (!block_create_stats_->set_builder(key, 256, cb)) {
    return fatal_error("cannot store new CreatorStats for "s + key.to_hex(256) + " into dictionary");
  }
  return true;
}

int Collator::creator_count_outdated(td::ConstBitPtr key, vm::CellSlice& cs) {
  block::DiscountedCounter mc_cnt, shard_cnt;
  if (!(block::fetch_CreatorStats(cs, mc_cnt, shard_cnt) && cs.empty_ext())) {
    fatal_error("cannot unpack CreatorStats for "s + key.to_hex(256) + " from previous masterchain state");
    return -1;
  }
  if (!(mc_cnt.increase_by(0, now_) && shard_cnt.increase_by(0, now_))) {
    fatal_error("cannot amortize counters in CreatorStats for "s + key.to_hex(256));
    return -1;
  }
  if (!(mc_cnt.cnt65536 | shard_cnt.cnt65536)) {
    LOG(DEBUG) << "removing stale CreatorStats for " << key.to_hex(256);
    return 0;
  } else {
    return 1;
  }
}

bool Collator::update_block_creator_stats() {
  if (!create_stats_enabled_) {
    return true;
  }
  LOG(INFO) << "updating block creator statistics";
  CHECK(block_create_stats_);
  for (const auto& p : block_create_count_) {
    if (!update_block_creator_count(p.first.bits(), p.second, 0)) {
      return fatal_error("cannot update CreatorStats for "s + p.first.to_hex());
    }
  }
  auto has_creator = !created_by_.is_zero();
  if (has_creator && !update_block_creator_count(created_by_.as_bits256().bits(), 0, 1)) {
    return fatal_error("cannot update CreatorStats for "s + created_by_.as_bits256().to_hex());
  }
  if ((has_creator || block_create_total_) &&
      !update_block_creator_count(td::Bits256::zero().bits(), block_create_total_, has_creator)) {
    return fatal_error("cannot update CreatorStats with zero index (representing the sum of other CreatorStats)");
  }
  // -> DEBUG
  LOG(INFO) << "scanning for outdated CreatorStats entries";
  /*
  int cnt = block_create_stats_->filter([this](vm::CellSlice& cs, td::ConstBitPtr key, int key_len) {
    CHECK(key_len == 256);
    return creator_count_outdated(key, cs);
  });
  */
  // alternative version with partial scan
  td::Bits256 key;
  prng::rand_gen().rand_bytes(key.data(), 32);
  int scanned, cnt = 0;
  for (scanned = 0; scanned < 100; scanned++) {
    auto cs = block_create_stats_->lookup_nearest_key(key.bits(), 256, true);
    if (cs.is_null()) {
      break;
    }
    auto res = creator_count_outdated(key.bits(), cs.write());
    if (!res) {
      LOG(DEBUG) << "prunning CreatorStats for " << key.to_hex();
      block_create_stats_->lookup_delete(key);
      ++cnt;
    } else if (res < 0) {
      return fatal_error("error scanning stale CreatorStats entries");
    }
  }
  // -> DEBUG
  LOG(INFO) << "removed " << cnt << " stale CreatorStats entries out of " << scanned << " scanned";
  return cnt >= 0;
}

td::Result<Ref<vm::Cell>> Collator::get_config_data_from_smc(const ton::StdSmcAddress& cfg_addr) {
  return block::get_config_data_from_smc(account_dict->lookup_ref(cfg_addr));
}

bool Collator::try_fetch_new_config(const ton::StdSmcAddress& cfg_addr, Ref<vm::Cell>& new_config) {
  auto cfg_res = get_config_data_from_smc(cfg_addr);
  if (cfg_res.is_error()) {
    LOG(ERROR) << "cannot extract new configuration from configuration smart contract " << cfg_addr.to_hex() << " : "
               << cfg_res.move_as_error().to_string();
    return false;
  }
  auto cfg = cfg_res.move_as_ok();
  if (!block::valid_config_data(cfg, cfg_addr, true, false, old_mparams_)) {
    LOG(ERROR) << "new configuration smart contract " << cfg_addr.to_hex()
               << " contains a new configuration which is invalid, ignoring";
    return false;
  }
  new_config = cfg;
  return true;
}

static int history_weight(td::uint64 history) {
  return td::count_bits64(history & 0xffff) * 3 + td::count_bits64(history & 0xffff0000) * 2 +
         td::count_bits64(history & 0xffff00000000) - (3 + 2 + 1) * 16 * 2 / 3;
}

bool Collator::check_block_overload() {
  overload_history_ <<= 1;
  underload_history_ <<= 1;
  block_size_estimate_ = block_limit_status_->estimate_block_size();
  LOG(INFO) << "block load statistics: gas=" << block_limit_status_->gas_used
            << " lt_delta=" << block_limit_status_->cur_lt - block_limit_status_->limits.start_lt
            << " size_estimate=" << block_size_estimate_;
  auto cl = block_limit_status_->classify();
  if (cl <= block::ParamLimits::cl_underload) {
    underload_history_ |= 1;
    LOG(INFO) << "block is underloaded";
  } else if (cl >= block::ParamLimits::cl_soft) {
    overload_history_ |= 1;
    LOG(INFO) << "block is overloaded (category " << cl << ")";
  } else {
    LOG(INFO) << "block is loaded normally";
  }
  if (collator_settings & 1) {
    LOG(INFO) << "want_split manually set";
    want_split_ = true;
    return true;
  } else if (collator_settings & 2) {
    LOG(INFO) << "want_merge manually set";
    want_merge_ = true;
    return true;
  }
  char buffer[17];
  if (history_weight(overload_history_) >= 0) {
    sprintf(buffer, "%016llx", (unsigned long long)overload_history_);
    LOG(INFO) << "want_split set because of overload history " << buffer;
    want_split_ = true;
  } else if (history_weight(underload_history_) >= 0) {
    sprintf(buffer, "%016llx", (unsigned long long)underload_history_);
    LOG(INFO) << "want_merge set because of underload history " << buffer;
    want_merge_ = true;
  }
  return true;
}

bool Collator::remove_public_library(td::ConstBitPtr key, td::ConstBitPtr addr) {
  LOG(INFO) << "Removing public library " << key.to_hex(256) << " of account " << addr.to_hex(256);
  auto val = shard_libraries_->lookup(key, 256);
  if (val.is_null()) {
    return fatal_error("cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because this public library did not exist");
  }
  block::gen::LibDescr::Record rec;
  if (!tlb::csr_unpack(std::move(val), rec)) {
    return fatal_error("cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because this public library LibDescr record is invalid");
  }
  if (rec.lib->get_hash().bits().compare(key, 256)) {
    return fatal_error(
        "cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
        " because this public library LibDescr record does not contain a library root cell with required hash");
  }
  vm::Dictionary publishers{vm::DictNonEmpty(), std::move(rec.publishers), 256};
  auto found = publishers.lookup_delete(addr, 256);
  if (found.is_null() || found->size_ext()) {
    return fatal_error("cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because this public library LibDescr record does not list this account as one of publishers");
  }
  if (publishers.is_empty()) {
    LOG(INFO) << "library " << key.to_hex(256) << " has no publishers left, removing altogether";
    auto val2 = shard_libraries_->lookup_delete(key, 256);
    CHECK(val2.not_null());
    return true;
  }
  rec.publishers = vm::load_cell_slice_ref(std::move(publishers).extract_root_cell());
  vm::CellBuilder cb;
  if (!tlb::pack(cb, rec)) {
    return fatal_error("cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because the new LibDescr cannot be serialized");
  }
  if (!shard_libraries_->set_builder(key, 256, cb, vm::Dictionary::SetMode::Replace)) {
    return fatal_error("cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because the LibDescr cannot be modified inside the shard library dictionary");
  }
  libraries_changed_ = true;
  return true;
}

bool Collator::add_public_library(td::ConstBitPtr key, td::ConstBitPtr addr, Ref<vm::Cell> library) {
  LOG(INFO) << "Adding public library " << key.to_hex(256) << " of account " << addr.to_hex(256);
  CHECK(library.not_null() && !library->get_hash().bits().compare(key, 256));
  block::gen::LibDescr::Record rec;
  std::unique_ptr<vm::Dictionary> publishers;
  auto val = shard_libraries_->lookup(key, 256);
  if (val.is_null()) {
    rec.lib = std::move(library);
    publishers = std::make_unique<vm::Dictionary>(256);
  } else if (!tlb::csr_unpack(std::move(val), rec)) {
    return fatal_error("cannot add public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because this public library LibDescr record is invalid");
  } else if (rec.lib->get_hash().bits().compare(key, 256)) {
    return fatal_error(
        "cannot add public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
        " because existing LibDescr record for this library does not contain a library root cell with required hash");
  } else {
    publishers = std::make_unique<vm::Dictionary>(vm::DictNonEmpty(), std::move(rec.publishers), 256);
  }
  vm::CellBuilder cb;
  if (!publishers->set_builder(addr, 256, cb, vm::Dictionary::SetMode::Add)) {
    return fatal_error("cannot add public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because this public library LibDescr record already lists this account as a publisher");
  }
  rec.publishers = vm::load_cell_slice_ref(std::move(*publishers).extract_root_cell());
  cb.reset();
  if (!tlb::pack(cb, rec)) {
    return fatal_error("cannot add public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because the new LibDescr cannot be serialized");
  }
  if (!shard_libraries_->set_builder(key, 256, cb, vm::Dictionary::SetMode::Set)) {
    return fatal_error("cannot remove public library "s + key.to_hex(256) + " of account " + addr.to_hex(256) +
                       " because the LibDescr cannot be added to the shard library dictionary");
  }
  libraries_changed_ = true;
  return true;
}

bool Collator::update_account_public_libraries(Ref<vm::Cell> orig_libs, Ref<vm::Cell> final_libs,
                                               const td::Bits256& addr) {
  vm::Dictionary dict1{std::move(orig_libs), 256}, dict2{std::move(final_libs), 256};
  return dict1.scan_diff(
      dict2, [this, &addr](td::ConstBitPtr key, int n, Ref<vm::CellSlice> val1, Ref<vm::CellSlice> val2) -> bool {
        CHECK(n == 256);
        bool f = block::is_public_library(key, std::move(val1));
        bool g = block::is_public_library(key, val2);
        if (f && !g) {
          return remove_public_library(key, addr.bits());
        } else if (!f && g) {
          return add_public_library(key, addr.bits(), val2->prefetch_ref());
        }
        return true;
      });
}

bool Collator::update_public_libraries() {
  CHECK(is_masterchain());
  for (auto& z : accounts) {
    block::Account& acc = *(z.second);
    CHECK(acc.addr == z.first);
    if (acc.libraries_changed()) {
      LOG(DEBUG) << "libraries of " << acc.addr.to_hex() << " changed, rescanning";
      CHECK(!acc.transactions.empty());
      if (!update_account_public_libraries(acc.orig_library, acc.library, acc.addr)) {
        return fatal_error("error scanning public libraries of account "s + acc.addr.to_hex());
      }
    }
  }
  if (libraries_changed_ && verbosity >= 2 * 0) {
    std::cerr << "New public libraries: ";
    block::gen::t_HashmapE_256_LibDescr.print(std::cerr, shard_libraries_->get_root());
    shard_libraries_->get_root()->print_rec(std::cerr);
  }
  return true;
}

bool Collator::update_min_mc_seqno(ton::BlockSeqno some_mc_seqno) {
  min_ref_mc_seqno_ = std::min(min_ref_mc_seqno_, some_mc_seqno);
  return true;
}

bool Collator::register_out_msg_queue_op(bool force) {
  ++out_msg_queue_ops_;
  if (force || !(out_msg_queue_ops_ & 63)) {
    return block_limit_status_->add_proof(out_msg_queue_->get_root_cell());
  } else {
    return true;
  }
}

bool Collator::create_shard_state() {
  Ref<vm::Cell> msg_q_info;
  vm::CellBuilder cb, cb2;
  if (!(cb.store_long_bool(0x9023afe2, 32)          // shard_state#9023afe2
        && cb.store_long_bool(global_id_, 32)       // global_id:int32
        && global_id_                               // { global_id != 0 }
        && block::ShardId{shard_}.serialize(cb)     // shard_id:ShardIdent
        && cb.store_long_bool(new_block_seqno, 32)  // seq_no:uint32
        && cb.store_long_bool(vert_seqno_, 32)      // vert_seq_no:#
        && cb.store_long_bool(now_, 32)             // gen_utime:uint32
        && cb.store_long_bool(max_lt, 64)           // gen_lt:uint64
        && update_processed_upto()                  // insert new ProcessedUpto
        && update_min_mc_seqno(processed_upto_->min_mc_seqno()) &&
        cb.store_long_bool(min_ref_mc_seqno_, 32)       // min_ref_mc_seqno:uint32
        && compute_out_msg_queue_info(msg_q_info)       // -> out_msg_queue_info
        && cb.store_ref_bool(msg_q_info)                // out_msg_queue_info:^OutMsgQueueInfo
        && cb.store_long_bool(before_split_, 1)         // before_split:Bool
        && account_dict->append_dict_to_bool(cb2)       // accounts:^ShardAccounts
        && cb.store_ref_bool(cb2.finalize())            // ...
        && cb2.store_long_bool(overload_history_, 64)   // ^[ overload_history:uint64
        && cb2.store_long_bool(underload_history_, 64)  //    underload_history:uint64
        && compute_total_balance()                      //    -> total_balance, total_validator_fees
        && total_balance_.store(cb2)                    //  total_balance:CurrencyCollection
        && total_validator_fees_.store(cb2)             //  total_validator_fees:CurrencyCollection
        && shard_libraries_->append_dict_to_bool(cb2)   //    libraries:(HashmapE 256 LibDescr)
        && cb2.store_bool_bool(!is_masterchain()) &&
        (is_masterchain() || store_master_ref(cb2))  // master_ref:(Maybe BlkMasterInfo)
        && cb.store_ref_bool(cb2.finalize())         // ]
        && cb.store_maybe_ref(mc_state_extra_)       // custom:(Maybe ^McStateExtra)
        && cb.finalize_to(state_root))) {
    return fatal_error("cannot create new ShardState");
  }
  LOG(DEBUG) << "min_ref_mc_seqno is " << min_ref_mc_seqno_;
  if (verbosity > 2) {
    std::cerr << "new ShardState: ";
    block::gen::t_ShardState.print_ref(std::cerr, state_root);
    vm::load_cell_slice(state_root).print_rec(std::cerr);
  }
  if (verify >= 2) {
    LOG(INFO) << "verifying new ShardState";
    CHECK(block::gen::t_ShardState.validate_ref(1000000, state_root));
    CHECK(block::tlb::t_ShardState.validate_ref(1000000, state_root));
  }
  LOG(INFO) << "creating Merkle update for the ShardState";
  state_update = vm::MerkleUpdate::generate(prev_state_root_, state_root, state_usage_tree_.get());
  if (verbosity > 2) {
    std::cerr << "Merkle Update for ShardState: ";
    vm::CellSlice cs{vm::NoVm{}, state_update};
    cs.print_rec(std::cerr);
  }
  LOG(INFO) << "updating block profile statistics";
  block_limit_status_->add_proof(state_root);
  LOG(INFO) << "new ShardState and corresponding Merkle update created";
  return true;
}

// stores BlkMasterInfo (for non-masterchain blocks)
bool Collator::store_master_ref(vm::CellBuilder& cb) {
  return mc_block_root.not_null() && store_ext_blk_ref_to(cb, mc_block_id_, mc_block_root);
}

bool Collator::update_processed_upto() {
  auto ref_mc_seqno = is_masterchain() ? new_block_seqno : prev_mc_block_seqno;
  update_min_mc_seqno(ref_mc_seqno);
  if (last_proc_int_msg_.first) {
    if (!processed_upto_->insert(ref_mc_seqno, last_proc_int_msg_.first, last_proc_int_msg_.second.cbits())) {
      return fatal_error("cannot update our ProcessedUpto to reflect processed inbound message");
    }
  } else if (inbound_queues_empty_ && config_->lt > 0 &&
             !processed_upto_->insert_infty(ref_mc_seqno, config_->lt - 1)) {
    return fatal_error("cannot update our ProcessedUpto to reflect that all original inbound queues are empty");
  }
  return processed_upto_->compactify();
}

bool Collator::compute_out_msg_queue_info(Ref<vm::Cell>& out_msg_queue_info) {
  if (verbosity >= 2) {
    auto rt = out_msg_queue_->get_root();
    std::cerr << "resulting out_msg_queue is ";
    block::gen::t_OutMsgQueue.print(std::cerr, *rt);
    rt->print_rec(std::cerr);
  }
  vm::CellBuilder cb;
  return register_out_msg_queue_op(true) && out_msg_queue_->append_dict_to_bool(cb)  // _ out_queue:OutMsgQueue
         && processed_upto_->pack(cb)                                                // proc_info:ProcessedInfo
         && ihr_pending->append_dict_to_bool(cb)                                     // ihr_pending:IhrPendingInfo
         && cb.finalize_to(out_msg_queue_info);
}

bool Collator::compute_total_balance() {
  // 1. compute total_balance_ from the augmentation value of ShardAccounts
  auto accounts_extra = account_dict->get_root_extra();
  if (!(accounts_extra.write().advance(5) && total_balance_.validate_unpack(accounts_extra))) {
    LOG(ERROR) << "cannot unpack CurrencyCollection from the root of accounts dictionary";
    return false;
  }
  value_flow_.to_next_blk = total_balance_;
  // 2. compute new_validator_fees
  block::CurrencyCollection new_transaction_fees;
  vm::AugmentedDictionary acc_blocks_dict{vm::load_cell_slice_ref(shard_account_blocks_), 256,
                                          block::tlb::aug_ShardAccountBlocks};
  if (!new_transaction_fees.validate_unpack(acc_blocks_dict.get_root_extra())) {
    return fatal_error("cannot extract new_transaction_fees from the root of ShardAccountBlocks");
  }
  vm::CellSlice cs{*(in_msg_dict->get_root_extra())};
  if (verbosity > 2) {
    block::gen::t_ImportFees.print(std::cerr, vm::CellSlice{*(in_msg_dict->get_root_extra())});
    cs.print_rec(std::cerr);
  }
  auto new_import_fees = block::tlb::t_Grams.as_integer_skip(cs);
  if (new_import_fees.is_null()) {
    LOG(ERROR) << "new_import_fees is null (?)";
    return false;
  }
  if (!value_flow_.imported.fetch_exact(cs)) {
    LOG(ERROR) << "cannot unpack ImportFees from the root of InMsgDescr";
    return false;
  }
  if (!value_flow_.exported.validate_unpack(out_msg_dict->get_root_extra())) {
    LOG(ERROR) << "cannot unpack CurrencyCollection from the root of OutMsgDescr";
    return false;
  }
  value_flow_.fees_collected += new_transaction_fees + new_import_fees;
  // 3. compute total_validator_fees
  total_validator_fees_ += value_flow_.fees_collected;
  total_validator_fees_ -= value_flow_.recovered;
  CHECK(total_validator_fees_.is_valid());
  return true;
}

bool Collator::create_block_info(Ref<vm::Cell>& block_info) {
  vm::CellBuilder cb, cb2;
  bool mc = is_masterchain();
  td::uint32 val_hash = is_hardfork_ ? 0 : validator_set_->get_validator_set_hash();
  CatchainSeqno cc_seqno = is_hardfork_ ? 0 : validator_set_->get_catchain_seqno();
  return cb.store_long_bool(0x9bc7a987, 32)                         // block_info#9bc7a987
         && cb.store_long_bool(0, 32)                               // version:uint32
         && cb.store_bool_bool(!mc)                                 // not_master:(## 1)
         && cb.store_bool_bool(after_merge_)                        // after_merge:(## 1)
         && cb.store_bool_bool(before_split_)                       // before_split:Bool
         && cb.store_bool_bool(after_split_)                        // after_split:Bool
         && cb.store_bool_bool(want_split_)                         // want_split:Bool
         && cb.store_bool_bool(want_merge_)                         // want_merge:Bool
         && cb.store_bool_bool(is_key_block_)                       // key_block:Bool
         && cb.store_bool_bool(is_hardfork_)                        // vert_seqno_incr:(## 1)
         && cb.store_long_bool((int)report_version_, 8)             // flags:(## 8)
         && cb.store_long_bool(new_block_seqno, 32)                 // seq_no:#
         && cb.store_long_bool(vert_seqno_, 32)                     // vert_seq_no:#
         && block::ShardId{shard_}.serialize(cb)                    // shard:ShardIdent
         && cb.store_long_bool(now_, 32)                            // gen_utime:uint32
         && cb.store_long_bool(start_lt, 64)                        // start_lt:uint64
         && cb.store_long_bool(max_lt, 64)                          // end_lt:uint64
         && cb.store_long_bool(val_hash, 32)                        // gen_validator_list_hash_short:uint32
         && cb.store_long_bool(cc_seqno, 32)                        // gen_catchain_seqno:uint32
         && cb.store_long_bool(min_ref_mc_seqno_, 32)               // min_ref_mc_seqno:uint32
         && cb.store_long_bool(prev_key_block_seqno_, 32)           // prev_key_block_seqno:uint32
         && (!report_version_ || store_version(cb))                 // gen_software:flags . 0?GlobalVersion
         && (mc || (store_master_ref(cb2)                           // master_ref:not_master?
                    && cb.store_builder_ref_bool(std::move(cb2))))  // .. ^BlkMasterInfo
         && store_prev_blk_ref(cb2, after_merge_)                   // prev_ref:..
         && cb.store_builder_ref_bool(std::move(cb2))               // .. ^(PrevBlkInfo after_merge)
         && (!is_hardfork_ ||                                       // prev_vert_ref:vert_seqno_incr?..
             (store_master_ref(cb2)                                 //
              && cb.store_builder_ref_bool(std::move(cb2))))        // .. ^(BlkPrevInfo 0)
         && cb.finalize_to(block_info);
}

bool Collator::store_version(vm::CellBuilder& cb) const {
  return block::gen::t_GlobalVersion.pack_capabilities(cb, supported_version(), supported_capabilities());
}

bool Collator::store_zero_state_ref(vm::CellBuilder& cb) {
  CHECK(prev_state_root_.not_null());
  RootHash root_hash = prev_state_root_->get_hash().bits();
  CHECK(prev_blocks.size() == 1);
  CHECK(!prev_blocks[0].seqno());
  CHECK(root_hash == prev_blocks[0].root_hash);
  return cb.store_long_bool(prev_state_lt_, 64)            // ext_blk_ref$_ end_lt:uint64
         && cb.store_long_bool(0, 32)                      // seq_no:uint32
         && cb.store_bits_bool(root_hash)                  // root_hash:bits256
         && cb.store_bits_bool(prev_blocks[0].file_hash);  // file_hash:bits256
}

bool Collator::store_prev_blk_ref(vm::CellBuilder& cb, bool is_after_merge) {
  if (is_after_merge) {
    auto root2 = prev_block_data.at(1)->root_cell();
    CHECK(prev_block_root.not_null());
    CHECK(root2.not_null());
    vm::CellBuilder cb2;
    return store_ext_blk_ref_to(cb2, prev_blocks.at(0), prev_block_root) && cb.store_ref_bool(cb2.finalize()) &&
           store_ext_blk_ref_to(cb2, prev_blocks.at(1), std::move(root2)) && cb.store_ref_bool(cb2.finalize());
  }
  if (!last_block_seqno) {
    return store_zero_state_ref(cb);
  } else {
    CHECK(prev_block_root.not_null());
    return store_ext_blk_ref_to(cb, prev_blocks.at(0), prev_block_root);
  }
}

bool Collator::check_value_flow() {
  if (!value_flow_.validate()) {
    LOG(ERROR) << "incorrect value flow in new block : " << value_flow_.to_str();
    return fatal_error("incorrect value flow for the newly-generated block: in != out");
  }
  LOG(INFO) << "Value flow: " << value_flow_.to_str();
  return true;
}

bool Collator::create_block_extra(Ref<vm::Cell>& block_extra) {
  bool mc = is_masterchain();
  Ref<vm::Cell> mc_block_extra;
  vm::CellBuilder cb, cb2;
  return cb.store_long_bool(0x4a33f6fdU, 32)                                             // block_extra
         && in_msg_dict->append_dict_to_bool(cb2) && cb.store_ref_bool(cb2.finalize())   // in_msg_descr:^InMsgDescr
         && out_msg_dict->append_dict_to_bool(cb2) && cb.store_ref_bool(cb2.finalize())  // out_msg_descr:^OutMsgDescr
         && cb.store_ref_bool(shard_account_blocks_)      // account_blocks:^ShardAccountBlocks
         && cb.store_bits_bool(rand_seed_)                // rand_seed:bits256
         && cb.store_bits_bool(created_by_.as_bits256())  // created_by:bits256
         && cb.store_bool_bool(mc)                        // custom:(Maybe
         && (!mc || (create_mc_block_extra(mc_block_extra) && cb.store_ref_bool(mc_block_extra)))  // .. ^McBlockExtra)
         && cb.finalize_to(block_extra);                                                           // = BlockExtra;
}

bool Collator::create_mc_block_extra(Ref<vm::Cell>& mc_block_extra) {
  if (!is_masterchain()) {
    return false;
  }
  vm::CellBuilder cb, cb2;
  return cb.store_long_bool(0xcca5, 16)                            // masterchain_block_extra#cca5
         && cb.store_bool_bool(is_key_block_)                      // key_block:(## 1)
         && cb.append_cellslice_bool(shard_conf_->get_root_csr())  // shard_hashes:ShardHashes
         && fees_import_dict_->append_dict_to_bool(cb)             // shard_fees:ShardFees
         && cb2.store_long_bool(0, 1)                 // ^[ TODO: prev_blk_signatures:(HashmapE 16 CryptoSignature)
         && cb2.store_maybe_ref(recover_create_msg_)  //   recover_create_msg:(Maybe ^InMsg)
         && cb2.store_maybe_ref(mint_msg_)            //   mint_msg:(Maybe ^InMsg)
         && cb.store_ref_bool(cb2.finalize())         // ]
         && (!is_key_block_ || cb.append_cellslice_bool(new_config_params_))  // config:key_block?ConfigParams
         && cb.finalize_to(mc_block_extra);                                   //   = McBlockExtra
}

bool Collator::create_block() {
  Ref<vm::Cell> block_info, extra;
  if (!create_block_info(block_info)) {
    return fatal_error("cannot create BlockInfo for the new block");
  }
  if (!check_value_flow()) {
    return fatal_error("cannot create ValueFlow for the new block");
  }
  if (!create_block_extra(extra)) {
    return fatal_error("cannot create BlockExtra for the new block");
  }
  vm::CellBuilder cb, cb2;
  if (!(cb.store_long_bool(0x11ef55aa, 32)     // block#11ef55aa
        && cb.store_long_bool(global_id_, 32)  // global_id:int32
        && global_id_                          // { global_id != 0 }
        && cb.store_ref_bool(block_info)       // info:^BlockInfo
        && value_flow_.store(cb2)              // value_flow:^ValueFlow
        && cb.store_ref_bool(cb2.finalize())   // ...
        && cb.store_ref_bool(state_update)     // state_update:^(MERKLE_UPDATE ShardState)
        && cb.store_ref_bool(extra)            // extra:^BlockExtra
        && cb.finalize_to(new_block))) {       // = Block
    return fatal_error("cannot create new Block");
  }
  if (verbosity >= 3 * 1) {
    std::cerr << "new Block: ";
    block::gen::t_Block.print_ref(std::cerr, new_block);
    vm::load_cell_slice(new_block).print_rec(std::cerr);
  }
  if (verify >= 1) {
    LOG(INFO) << "verifying new Block";
    if (!block::gen::t_Block.validate_ref(1000000, new_block)) {
      return fatal_error("new Block failed to pass automatic validity tests");
    }
  }
  LOG(INFO) << "new Block created";
  return true;
}

Ref<vm::Cell> Collator::collate_shard_block_descr_set() {
  vm::Dictionary dict{96};
  for (const auto& descr : used_shard_block_descr_) {
    auto shard = descr->shard();
    td::BitArray<96> key;
    key.bits().store_int(shard.workchain, 32);
    (key.bits() + 32).store_uint(shard.shard, 64);
    CHECK(dict.set_ref(key, descr->get_root(), vm::Dictionary::SetMode::Add));
  }
  block::gen::TopBlockDescrSet::Record rec;
  Ref<vm::Cell> cell;
  rec.collection = std::move(dict).extract_root();
  if (!tlb::pack_cell(cell, rec)) {
    fatal_error("cannot serialize TopBlockDescrSet for collated data");
    return {};
  }
  if (verbosity >= 4 * 1) {
    std::cerr << "serialized TopBlockDescrSet for collated data is: ";
    block::gen::t_TopBlockDescrSet.print_ref(std::cerr, cell);
    vm::load_cell_slice(cell).print_rec(std::cerr);
  }
  return cell;
}

bool Collator::create_collated_data() {
  // TODO: store something into collated_roots_
  // 1. store the set of used shard block descriptions
  if (!used_shard_block_descr_.empty()) {
    auto cell = collate_shard_block_descr_set();
    if (cell.is_null()) {
      return true;
      return fatal_error("cannot collate the collection of used shard block descriptions");
    }
    collated_roots_.push_back(std::move(cell));
  }
  // 2. ...
  return true;
}

bool Collator::create_block_candidate() {
  // 1. serialize block
  LOG(INFO) << "serializing new Block";
  vm::BagOfCells boc;
  boc.set_root(new_block);
  auto res = boc.import_cells();
  if (res.is_error()) {
    return fatal_error(res.move_as_error());
  }
  auto blk_res = boc.serialize_to_slice(31);
  if (blk_res.is_error()) {
    LOG(ERROR) << "cannot serialize block";
    return fatal_error(blk_res.move_as_error());
  }
  auto blk_slice = blk_res.move_as_ok();
  // 2. serialize collated data
  td::BufferSlice cdata_slice;
  if (collated_roots_.empty()) {
    cdata_slice = td::BufferSlice{0};
  } else {
    vm::BagOfCells boc_collated;
    boc_collated.set_roots(collated_roots_);
    res = boc_collated.import_cells();
    if (res.is_error()) {
      return fatal_error(res.move_as_error());
    }
    auto cdata_res = boc_collated.serialize_to_slice(31);
    if (cdata_res.is_error()) {
      LOG(ERROR) << "cannot serialize collated data";
      return fatal_error(cdata_res.move_as_error());
    }
    cdata_slice = cdata_res.move_as_ok();
  }
  LOG(INFO) << "serialized block size " << blk_slice.size() << " bytes (preliminary estimate was "
            << block_size_estimate_ << "), collated data " << cdata_slice.size() << " bytes";
  auto st = block_limit_status_->st_stat.get_total_stat();
  LOG(INFO) << "size regression stats: " << blk_slice.size() << " " << st.cells << " " << st.bits << " "
            << st.internal_refs << " " << st.external_refs << " " << block_limit_status_->accounts << " "
            << block_limit_status_->transactions;
  // 3. create a BlockCandidate
  block_candidate = std::make_unique<BlockCandidate>(
      created_by_,
      ton::BlockIdExt{ton::BlockId{shard_, new_block_seqno}, new_block->get_hash().bits(),
                      block::compute_file_hash(blk_slice.as_slice())},
      block::compute_file_hash(cdata_slice.as_slice()), blk_slice.clone(), cdata_slice.clone());
  // 4. save block candidate
  LOG(INFO) << "saving new BlockCandidate";
  td::actor::send_closure_later(manager, &ValidatorManager::set_block_candidate, block_candidate->id,
                                block_candidate->clone(), [self = get_self()](td::Result<td::Unit> saved) -> void {
                                  LOG(DEBUG) << "got answer to set_block_candidate";
                                  td::actor::send_closure_later(std::move(self), &Collator::return_block_candidate,
                                                                std::move(saved));
                                });
  // 5. communicate about bad and delayed external messages
  if (!bad_ext_msgs_.empty() || !delay_ext_msgs_.empty()) {
    LOG(INFO) << "sending complete_external_messages() to Manager";
    td::actor::send_closure_later(manager, &ValidatorManager::complete_external_messages, std::move(delay_ext_msgs_),
                                  std::move(bad_ext_msgs_));
  }
  return true;
}

void Collator::return_block_candidate(td::Result<td::Unit> saved) {
  // 6. return data to the original "caller"
  if (saved.is_error()) {
    auto err = saved.move_as_error();
    LOG(ERROR) << "cannot save block candidate: " << err.to_string();
    fatal_error(std::move(err));
  } else {
    CHECK(block_candidate);
    LOG(INFO) << "sending new BlockCandidate to Promise";
    main_promise(block_candidate->clone());
    busy_ = false;
    stop();
  }
}

/*
 *
 *  Collator register methods
 *
 */

td::Result<bool> Collator::register_external_message_cell(Ref<vm::Cell> ext_msg, const ExtMessage::Hash& ext_hash) {
  if (ext_msg->get_level() != 0) {
    return td::Status::Error("external message must have zero level");
  }
  vm::CellSlice cs{vm::NoVmOrd{}, ext_msg};
  if (cs.prefetch_ulong(2) != 2) {  // ext_in_msg_info$10
    return td::Status::Error("external message must begin with ext_in_msg_info$10");
  }
  ton::Bits256 hash{ext_msg->get_hash().bits()};
  auto it = ext_msg_map.find(hash);
  if (it != ext_msg_map.end()) {
    if (it->second > 0) {
      // message registered before
      return false;
    } else {
      return td::Status::Error("external message has been rejected before");
    }
  }
  if (!block::gen::t_Message_Any.validate_ref(256, ext_msg)) {
    return td::Status::Error("external message is not a (Message Any) according to automated checks");
  }
  if (!block::tlb::t_Message.validate_ref(256, ext_msg)) {
    return td::Status::Error("external message is not a (Message Any) according to hand-written checks");
  }
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  if (!tlb::unpack_cell_inexact(ext_msg, info)) {
    return td::Status::Error("cannot unpack external message header");
  }
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  if (!dest_prefix.is_valid()) {
    return td::Status::Error("destination of an inbound external message is an invalid blockchain address");
  }
  // NB: previous checks are quite general and can be done at an outer level before multiplexing to correct Collator
  if (!ton::shard_contains(shard_, dest_prefix)) {
    return td::Status::Error("inbound external message has destination address not in this shard");
  }
  if (verbosity > 2) {
    std::cerr << "registered external message: ";
    block::gen::t_Message_Any.print_ref(std::cerr, ext_msg);
  }
  ext_msg_map.emplace(hash, 1);
  ext_msg_list_.emplace_back(std::move(ext_msg), ext_hash);
  return true;
}

/*
td::Result<bool> Collator::register_external_message(td::Slice ext_msg_boc) {
  if (ext_msg_boc.size() > max_ext_msg_size) {
    return td::Status::Error("external message too large, rejecting");
  }
  vm::BagOfCells boc;
  auto res = boc.deserialize(ext_msg_boc);
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error("external message is not a valid bag of cells");  // not a valid bag-of-Cells
  }
  return register_external_message_cell(boc.get_root_cell(0));
}
*/

void Collator::after_get_external_messages(td::Result<std::vector<Ref<ExtMessage>>> res) {
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto vect = res.move_as_ok();
  for (auto&& ext_msg : vect) {
    Ref<vm::Cell> ext_msg_cell = ext_msg->root_cell();
    bool err = ext_msg_cell.is_null();
    if (!err) {
      auto reg_res = register_external_message_cell(std::move(ext_msg_cell), ext_msg->hash());
      if (reg_res.is_error() || !reg_res.move_as_ok()) {
        err = true;
      }
    }
    if (err) {
      bad_ext_msgs_.emplace_back(ext_msg->hash());
    }
  }
  check_pending();
}

td::Result<bool> Collator::register_ihr_message_cell(Ref<vm::Cell> ihr_msg) {
  return false;
}

td::Result<bool> Collator::register_ihr_message(td::Slice ihr_msg_boc) {
  if (ihr_msg_boc.size() > max_ihr_msg_size) {
    return td::Status::Error("IHR message too large, rejecting");
  }
  vm::BagOfCells boc;
  auto res = boc.deserialize(ihr_msg_boc);
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error("IHR message is not a valid bag of cells");  // not a valid bag-of-Cells
  }
  return register_ihr_message_cell(boc.get_root_cell(0));
}

td::Result<bool> Collator::register_shard_signatures_cell(Ref<vm::Cell> signatures) {
  return false;
}

td::Result<bool> Collator::register_shard_signatures(td::Slice signatures_boc) {
  if (signatures_boc.size() > max_blk_sign_size) {
    return td::Status::Error("Shardchain signatures block too large, rejecting");
  }
  vm::BagOfCells boc;
  auto res = boc.deserialize(signatures_boc);
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error("Shardchain signatures block is not a valid bag of cells");  // not a valid bag-of-Cells
  }
  return register_shard_signatures_cell(boc.get_root_cell(0));
}

}  // namespace validator

}  // namespace ton
