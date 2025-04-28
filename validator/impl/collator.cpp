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

// Don't increase MERGE_MAX_QUEUE_LIMIT too much: merging requires cleaning the whole queue in out_msg_queue_cleanup
static constexpr td::uint32 FORCE_SPLIT_QUEUE_SIZE = 4096;
static constexpr td::uint32 SPLIT_MAX_QUEUE_SIZE = 100000;
static constexpr td::uint32 MERGE_MAX_QUEUE_SIZE = 2047;
static constexpr td::uint32 SKIP_EXTERNALS_QUEUE_SIZE = 8000;
static constexpr int HIGH_PRIORITY_EXTERNAL = 10;  // don't skip high priority externals when queue is big

static constexpr int MAX_ATTEMPTS = 5;

/**
 * Constructs a Collator object.
 *
 * @param shard The shard of the new block.
 * @param is_hardfork A boolean indicating whether the new block is a hardfork.
 * @param min_masterchain_block_id The the minimum reference masterchain block.
 * @param prev A vector of BlockIdExt representing the previous blocks.
 * @param validator_set A reference to the ValidatorSet.
 * @param collator_id The public key of the block creator.
 * @param collator_opts A reference to CollatorOptions.
 * @param manager The ActorId of the ValidatorManager.
 * @param timeout The timeout for the collator.
 * @param promise The promise to return the result.
 * @param cancellation_token Token to cancel collation.
 * @param mode +1 - skip storing candidate to disk.
 * @param attempt_idx The index of the attempt, starting from 0. On later attempts collator decreases block limits and skips some steps.
 */
Collator::Collator(ShardIdFull shard, bool is_hardfork, BlockIdExt min_masterchain_block_id,
                   std::vector<BlockIdExt> prev, td::Ref<ValidatorSet> validator_set, Ed25519_PublicKey collator_id,
                   Ref<CollatorOptions> collator_opts, td::actor::ActorId<ValidatorManager> manager,
                   td::Timestamp timeout, td::Promise<BlockCandidate> promise, td::CancellationToken cancellation_token,
                   unsigned mode, int attempt_idx)
    : shard_(shard)
    , is_hardfork_(is_hardfork)
    , min_mc_block_id{min_masterchain_block_id}
    , prev_blocks(std::move(prev))
    , created_by_(collator_id)
    , collator_opts_(collator_opts)
    , validator_set_(std::move(validator_set))
    , manager(manager)
    , timeout(timeout)
    // default timeout is 10 seconds, declared in validator/validator-group.cpp:generate_block_candidate:run_collate_query
    , queue_cleanup_timeout_(td::Timestamp::at(timeout.at() - 5.0))
    , soft_timeout_(td::Timestamp::at(timeout.at() - 3.0))
    , medium_timeout_(td::Timestamp::at(timeout.at() - 1.5))
    , main_promise(std::move(promise))
    , mode_(mode)
    , attempt_idx_(attempt_idx)
    , perf_timer_("collate", 0.1,
                  [manager](double duration) {
                    send_closure(manager, &ValidatorManager::add_perf_timer_stat, "collate", duration);
                  })
    , cancellation_token_(std::move(cancellation_token)) {
}

/**
 * Starts the Collator.
 *
 * This function initializes the Collator by performing various checks and queries to the ValidatorManager.
 * It checks the validity of the shard, the previous blocks, and the workchain.
 * If all checks pass, it proceeds to query the ValidatorManager for the top masterchain state block, shard states, block data, external messages, and shard blocks.
 * The results of these queries are handled by corresponding callback functions.
 */
void Collator::start_up() {
  LOG(WARNING) << "Collator for shard " << shard_.to_str() << " started"
               << (attempt_idx_ ? PSTRING() << " (attempt #" << attempt_idx_ << ")" : "");
  if (!check_cancelled()) {
    return;
  }
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
        [self = get_self()](td::Result<std::vector<std::pair<Ref<ExtMessage>, int>>> res) -> void {
          LOG(DEBUG) << "got answer to get_external_messages() query";
          td::actor::send_closure_later(std::move(self), &Collator::after_get_external_messages, std::move(res));
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

/**
 * Raises an error when timeout is reached.
 */
void Collator::alarm() {
  fatal_error(ErrorCode::timeout, "timeout");
}

/**
 * Generates a string representation of a shard.
 *
 * @param workchain The workchain ID of the shard.
 * @param shard The shard ID.
 *
 * @returns A string representation of the shard.
 */
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

/**
 * Returns a string representation of the shard of the given block.
 *
 * @param blk_id The BlockId object.
 *
 * @returns A string representation of the shard.
 */
std::string show_shard(const ton::BlockId blk_id) {
  return show_shard(blk_id.workchain, blk_id.shard);
}

/**
 * Converts a `ShardIdFull` object to a string representation.
 *
 * @param blk_id The `ShardIdFull` object to convert.
 *
 * @returns The string representation of the `ShardIdFull` object.
 */
std::string show_shard(const ton::ShardIdFull blk_id) {
  return show_shard(blk_id.workchain, blk_id.shard);
}

/**
 * Handles a fatal error encountered during block candidate generation.
 *
 * @param error The error encountered.
 *
 * @returns False to indicate that a fatal error occurred.
 */
bool Collator::fatal_error(td::Status error) {
  error.ensure_error();
  LOG(ERROR) << "cannot generate block candidate for " << show_shard(shard_) << " : " << error.to_string();
  if (busy_) {
    if (allow_repeat_collation_ && error.code() != ErrorCode::cancelled && attempt_idx_ + 1 < MAX_ATTEMPTS &&
        !is_hardfork_ && !timeout.is_in_past()) {
      LOG(WARNING) << "Repeating collation (attempt #" << attempt_idx_ + 1 << ")";
      run_collate_query(shard_, min_mc_block_id, prev_blocks, created_by_, validator_set_, collator_opts_, manager,
                        td::Timestamp::in(10.0), std::move(main_promise), std::move(cancellation_token_), mode_,
                        attempt_idx_ + 1);
    } else {
      main_promise(std::move(error));
      td::actor::send_closure(manager, &ValidatorManager::record_collate_query_stats, BlockIdExt{new_id, RootHash::zero(), FileHash::zero()},
                              work_timer_.elapsed(), cpu_work_timer_.elapsed(), td::optional<CollationStats>{});
    }
    busy_ = false;
  }
  stop();
  return false;
}

/**
 * Handles a fatal error encountered during block candidate generation.
 *
 * @param err_code The error code.
 * @param err_msg The error message.
 *
 * @returns False to indicate that a fatal error occurred.
 */
bool Collator::fatal_error(int err_code, std::string err_msg) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

/**
 * Handles a fatal error encountered during block candidate generation.
 *
 * @param err_msg The error message.
 * @param err_code The error code.
 *
 * @returns False to indicate that a fatal error occurred.
 */
bool Collator::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

/**
 * Checks if there are any pending tasks.
 *
 * If there are no pending tasks, it continues collation.
 * If collation fails, it raises a fatal error.
 * If an exception is caught during collation, it raises a fatal error with the corresponding error message.
 *
 * @returns None
 */
void Collator::check_pending() {
  // LOG(DEBUG) << "pending = " << pending;
  if (!check_cancelled()) {
    return;
  }
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

/**
 * Registers a masterchain state.
 *
 * @param other_mc_state The masterchain state to register.
 *
 * @returns True if the registration is successful, false otherwise.
 */
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

/**
 * Requests the auxiliary masterchain state.
 *
 * @param seqno The seqno of the block.
 * @param state A reference to the auxiliary masterchain state.
 *
 * @returns True if the auxiliary masterchain state is successfully requested, false otherwise.
 */
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

/**
 * Retrieves the auxiliary masterchain state for a given block sequence number.
 *
 * @param seqno The sequence number of the block.
 *
 * @returns A reference to the auxiliary masterchain state if found, otherwise an empty reference.
 */
Ref<MasterchainStateQ> Collator::get_aux_mc_state(BlockSeqno seqno) const {
  auto it = aux_mc_states_.find(seqno);
  if (it != aux_mc_states_.end()) {
    return it->second;
  } else {
    return {};
  }
}

/**
 * Callback function called after retrieving the auxiliary shard state.
 * Handles the retrieved shard state and performs necessary checks and registrations.
 *
 * @param blkid The BlockIdExt of the shard state.
 * @param res The result of retrieving the shard state.
 */
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

/**
 * Preprocesses the previous masterchain state.
 *
 * This function performs several checks and operations on the previous masterchain state
 * to ensure its validity and prepare it for further processing.
 *
 * @returns True if the preprocessing is successful, false otherwise.
 */
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

/**
 * Callback function called after retrieving the Masterchain state.
 *
 * @param res The retrieved masterchain state.
 */
void Collator::after_get_mc_state(td::Result<std::pair<Ref<MasterchainState>, BlockIdExt>> res) {
  LOG(WARNING) << "in Collator::after_get_mc_state()";
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

/**
 * Callback function called after retrieving the shard state for a previous block.
 *
 * @param idx The index of the previous shard block (0 or 1).
 * @param res The retrieved shard state.
 */
void Collator::after_get_shard_state(int idx, td::Result<Ref<ShardState>> res) {
  LOG(WARNING) << "in Collator::after_get_shard_state(" << idx << ")";
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

/**
 * Callback function called after retrieving block data for a previous block.
 *
 * @param idx The index of the previous block (0 or 1).
 * @param res The retreved block data.
 */
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

/**
 * Callback function called after retrieving shard block descriptions for masterchain.
 *
 * @param res The retrieved shard block descriptions.
 */
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

/**
 * Unpacks the last masterchain state and initializes the Collator object with the extracted configuration.
 *
 * @returns True if the unpacking and initialization is successful, false otherwise.
 */
bool Collator::unpack_last_mc_state() {
  auto res = block::ConfigInfo::extract_config(
      mc_state_root,
      block::ConfigInfo::needShardHashes | block::ConfigInfo::needLibraries | block::ConfigInfo::needValidatorSet |
          block::ConfigInfo::needWorkchainInfo | block::ConfigInfo::needCapabilities |
          block::ConfigInfo::needPrevBlocks |
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
  store_out_msg_queue_size_ = config_->has_capability(ton::capStoreOutMsgQueueSize);
  msg_metadata_enabled_ = config_->has_capability(ton::capMsgMetadata);
  deferring_messages_enabled_ = config_->has_capability(ton::capDeferMessages);
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
  if (attempt_idx_ == 3) {
    LOG(INFO) << "Attempt #3: bytes, gas limits /= 2";
    block_limits_->bytes.multiply_by(0.5);
    block_limits_->gas.multiply_by(0.5);
  } else if (attempt_idx_ == 4) {
    LOG(INFO) << "Attempt #4: bytes, gas limits /= 4";
    block_limits_->bytes.multiply_by(0.25);
    block_limits_->gas.multiply_by(0.25);
  }
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
  return true;
}

/**
 * Checks that the current validator set is entitled to create blocks in this shard and has a correct catchain seqno.
 *
 * @returns True if the current validator set is valid, false otherwise.
 */
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

/**
 * Requests the message queues of neighboring shards.
 *
 * @returns True if the request for neighbor message queues was successful, false otherwise.
 */
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

/**
 * Requests the size of the outbound message queue from the previous state(s) if needed.
 *
* @returns True if the request was successful, false otherwise.
 */
bool Collator::request_out_msg_queue_size() {
  if (have_out_msg_queue_size_in_state_) {
    // if after_split then have_out_msg_queue_size_in_state_ is always true, since the size is calculated during split
    return true;
  }
  out_msg_queue_size_ = 0;
  for (size_t i = 0; i < prev_blocks.size(); ++i) {
    ++pending;
    send_closure_later(manager, &ValidatorManager::get_out_msg_queue_size, prev_blocks[i],
                       [self = get_self(), i](td::Result<td::uint64> res) {
                         td::actor::send_closure(std::move(self), &Collator::got_out_queue_size, i, std::move(res));
                       });
  }
  return true;
}

/**
 * Handles the result of obtaining the outbound queue for a neighbor.
 *
 * @param i The index of the neighbor.
 * @param res The obtained outbound queue.
 */
void Collator::got_neighbor_out_queue(int i, td::Result<Ref<MessageQueue>> res) {
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  Ref<MessageQueue> outq_descr = res.move_as_ok();
  block::McShardDescr& descr = neighbors_.at(i);
  LOG(WARNING) << "obtained outbound queue for neighbor #" << i << " : " << descr.shard().to_str();
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
    FLOG(INFO) {
      block::gen::t_ProcessedInfo.print(sb, qinfo.proc_info);
      qinfo.proc_info->print_rec(sb);
    };
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

/**
 * Handles the result of obtaining the size of the outbound message queue.
 *
 * If the block is after merge then the two sizes are added.
 *
 * @param i The index of the previous block (0 or 1).
 * @param res The result object containing the size of the queue.
 */
void Collator::got_out_queue_size(size_t i, td::Result<td::uint64> res) {
  --pending;
  if (res.is_error()) {
    fatal_error(
        res.move_as_error_prefix(PSTRING() << "failed to get message queue size from prev block #" << i << ": "));
    return;
  }
  td::uint64 size = res.move_as_ok();
  LOG(WARNING) << "got outbound queue size from prev block #" << i << ": " << size;
  out_msg_queue_size_ += size;
  check_pending();
}

/**
 * Unpacks and merges the states of two previous blocks.
 * Used if the block is after_merge.
 * 
 * @returns True if the unpacking and merging was successful, false otherwise.
 */
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

/**
 * Unpacks the state of the previous block.
 * Used if the block is not after_merge.
 *
 * @returns True if the unpacking is successful, false otherwise.
 */
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

/**
 * Unpacks the state of a previous block and performs necessary checks.
 *
 * @param ss The ShardState object to unpack the state into.
 * @param blkid The BlockIdExt of the previous block.
 * @param prev_state_root The root of the state.
 *
 * @returns True if the unpacking and checks are successful, false otherwise.
 */
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

/**
 * Splits the state of previous block.
 * Used if the block is after_split.
 *
 * @param ss The ShardState object representing the previous state. The result is stored here.
 *
 * @returns True if the split operation is successful, false otherwise.
 */
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

/**
 * Imports the shard state data into the Collator object.
 * 
 * SETS: account_dict = account_dict_estimator_, shard_libraries_, mc_state_extra
 *    total_balance_ = old_total_balance_, total_validator_fees_
 * SETS: overload_history_, underload_history_
 * SETS: prev_state_utime_, prev_state_lt_, prev_vert_seqno_
 * SETS: out_msg_queue, processed_upto_, ihr_pending
 *
 * @param ss The ShardState object containing the shard state data.
 *
 * @returns True if the import was successful, False otherwise.
 */
bool Collator::import_shard_state_data(block::ShardState& ss) {
  account_dict = std::move(ss.account_dict_);
  account_dict_estimator_ = std::make_unique<vm::AugmentedDictionary>(*account_dict);
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
  dispatch_queue_ = std::move(ss.dispatch_queue_);
  block_create_stats_ = std::move(ss.block_create_stats_);
  if (ss.out_msg_queue_size_) {
    have_out_msg_queue_size_in_state_ = true;
    out_msg_queue_size_ = ss.out_msg_queue_size_.value();
  }
  return true;
}

/**
 * Adds trivials neighbor after merging two shards.
 * Trivial neighbors are the two previous blocks.
 *
 * @returns True if the operation is successful, false otherwise.
 */
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

/**
 * Adds a trivial neighbor.
 * A trivial neighbor is the previous block.
 *
 * @returns True if the operation is successful, false otherwise.
 */
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

/**
 * Checks the previous block against the block registered in the masterchain.
 *
 * @param listed The BlockIdExt of the top block of this shard registered in the masterchain.
 * @param prev The BlockIdExt of the previous block.
 * @param chk_chain_len Flag indicating whether to check the chain length.
 *
 * @returns True if the previous block is valid, false otherwise.
 */
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

/**
 * Checks the previous block against the block registered in the masterchain.
 *
 * @param listed The BlockIdExt of the top block of this shard registered in the masterchain.
 * @param prev The BlockIdExt of the previous block.
 *
 * @returns True if the previous block is equal to the one registered in the masterchain, false otherwise.
 */
bool Collator::check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev) {
  if (listed != prev) {
    return fatal_error(PSTRING() << "cannot generate shardchain block for shard " << shard_.to_str()
                                 << " after previous block " << prev.to_str()
                                 << " because masterchain configuration expects another previous block "
                                 << listed.to_str() << " and we are immediately after a split/merge event");
  }
  return true;
}

/**
 * Checks the validity of the shard configuration of the current shard.
 * 
 * @returns True if the shard configuration is valid, false otherwise.
 */
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

/**
 * Initializes the block limits for the collator.
 *
 * @returns True if the block limits were successfully initialized, false otherwise.
 */
bool Collator::init_block_limits() {
  CHECK(block_limits_);
  CHECK(state_usage_tree_);
  if (now_ > prev_now_ + 15 && block_limits_->lt_delta.hard() > 200) {
    block_limits_->lt_delta = {20, 180, 200};
  }
  block_limits_->usage_tree = state_usage_tree_.get();
  block_limit_status_ = std::make_unique<block::BlockLimitStatus>(*block_limits_);
  return true;
}

/**
 * Performs pre-initialization steps for the Collator.
 *
 * @returns True if pre-initialization is successful, false otherwise.
 */
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
  if (!request_out_msg_queue_size()) {
    return false;
  }
  return true;
}

/**
 * Adjusts the shard configuration by adding new workchains to the shard configuration in the masterchain state.
 * Used in masterchain collator.
 *
 * @returns True if the shard configuration was successfully adjusted, false otherwise.
 */
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

/**
 * Compares two ShardTopBlockDescription references based on their block IDs.
 *
 * @param a The first ShardTopBlockDescription reference.
 * @param b The second ShardTopBlockDescription reference.
 *
 * @returns True if a is considered less than b, false otherwise.
 */
static bool cmp_shard_block_descr_ref(const Ref<ShardTopBlockDescription>& a, const Ref<ShardTopBlockDescription>& b) {
  BlockId x = a->block_id().id, y = b->block_id().id;
  return x.workchain < y.workchain ||
         (x.workchain == y.workchain && (x.shard < y.shard || (x.shard == y.shard && x.seqno > y.seqno)));
}

/**
 * Stores the fees imported from a shard blocks to `fees_import_dict_`.
 * Used in masterchain collator.
 *
 * @param shard The shard identifier.
 * @param fees The fees imported from the block.
 * @param created The fee for creating shard blocks.
 *
 * @returns True if the fees were successfully stored, false otherwise.
 */
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

/**
 * Stores the fees imported from a shard blocks to `fees_import_dict_`.
 * Used in masterchain collator.
 *
 * @param descr A reference to the McShardHash object containing the shard information.
 *
 * @returns True if the shard fees and funds created were successfully stored, false otherwise.
 */
bool Collator::store_shard_fees(Ref<block::McShardHash> descr) {
  CHECK(descr.not_null());
  CHECK(descr->fees_collected_.is_valid());
  CHECK(descr->funds_created_.is_valid());
  CHECK(store_shard_fees(descr->shard(), descr->fees_collected_, descr->funds_created_));
  return true;
}

/**
 * Imports new top shard blocks and updates the shard configuration.
 *
 * @returns True if the import was successful, false otherwise.
 */
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
    FLOG(INFO) {
      sb << "updated shard block configuration to ";
      auto csr = shard_conf_->get_root_csr();
      block::gen::t_ShardHashes.print(sb, csr);
    };
  }
  block::gen::ShardFeeCreated::Record fc;
  if (!(tlb::csr_unpack(fees_import_dict_->get_root_extra(),
                        fc)  // _ fees:CurrencyCollection create:CurrencyCollection = ShardFeeCreated;
        && value_flow_.fees_imported.validate_unpack(fc.fees) && import_created_.validate_unpack(fc.create))) {
    return fatal_error("cannot read the total imported fees from the augmentation of the root of ShardFees");
  }
  LOG(INFO) << "total fees_imported = " << value_flow_.fees_imported.to_str()
            << " ; out of them, total fees_created = " << import_created_.to_str();
  block::CurrencyCollection burned =
      config_->get_burning_config().calculate_burned_fees(value_flow_.fees_imported - import_created_);
  if (!burned.is_valid()) {
    return fatal_error("cannot calculate amount of burned imported fees");
  }
  value_flow_.burned += burned;
  value_flow_.fees_collected += value_flow_.fees_imported - burned;
  return true;
}

/**
 * Registers the shard block creators to block_create_count_
 *
 * @param creator_list A vector of Bits256 representing the shard block creators.
 *
 * @returns True if the registration was successful, False otherwise.
 */
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

/**
 * Performs pre-initialization and collates the new block.
 *
 * @returns True if collation is successful, false otherwise.
 */
bool Collator::try_collate() {
  work_timer_.resume();
  cpu_work_timer_.resume();
  SCOPE_EXIT {
    work_timer_.pause();
    cpu_work_timer_.pause();
  };
  if (!preinit_complete) {
    LOG(WARNING) << "running do_preinit()";
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
  old_out_msg_queue_size_ = out_msg_queue_size_;
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

/**
 * Adjusts one entry from the processed up to information using the masterchain state that is referenced in the entry.
 *
 * @param proc The MsgProcessedUpto object.
 * @param owner The shard that the MsgProcessesUpto information is taken from.
 *
 * @returns True if the processed up to information was successfully adjusted, false otherwise.
 */
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

/**
 * Adjusts the processed up to collection using the using the auxilliary masterchain states.
 *
 * @param upto The MsgProcessedUptoCollection to be adjusted.
 *
 * @returns True if all entries were successfully adjusted, False otherwise.
 */
bool Collator::fix_processed_upto(block::MsgProcessedUptoCollection& upto) {
  for (auto& entry : upto.list) {
    if (!fix_one_processed_upto(entry, upto.owner)) {
      return false;
    }
  }
  return true;
}

/**
 * Initializes the unix time for the new block.
 * 
 * Unix time is set based on the current time, and the timestamps of the previous blocks.
 * If the previous block has a timestamp too far in the past then skipping importing external messages and new shard blocks is allowed.
 *
 * @returns True if the initialization is successful, false otherwise.
 */
bool Collator::init_utime() {
  CHECK(config_);
  // consider unixtime and lt from previous block(s) of the same shardchain
  prev_now_ = prev_state_utime_;
  // Extend collator timeout if previous block is too old
  td::Timestamp new_timeout = td::Timestamp::in(std::min(30.0, (td::Clocks::system() - (double)prev_now_) / 2));
  if (timeout < new_timeout) {
    timeout = new_timeout;
    alarm_timestamp() = timeout;
  }

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

/**
 * Initializes the logical time of the new block.
 */
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

/**
 * Fetches and initializes the configuration parameters using the masterchain configuration.
 *
 * @returns True if the configuration parameters were successfully fetched and initialized, false otherwise.
 */
bool Collator::fetch_config_params() {
  auto res = block::FetchConfigParams::fetch_config_params(
      *config_, &old_mparams_, &storage_prices_, &storage_phase_cfg_, &rand_seed_, &compute_phase_cfg_,
      &action_phase_cfg_, &serialize_cfg_, &masterchain_create_fee_, &basechain_create_fee_, workchain(), now_);
  if (res.is_error()) {
    return fatal_error(res.move_as_error());
  }
  compute_phase_cfg_.libraries = std::make_unique<vm::Dictionary>(config_->get_libraries_root(), 256);
  defer_out_queue_size_limit_ = std::max<td::uint64>(collator_opts_->defer_out_queue_size_limit,
                                                     compute_phase_cfg_.size_limits.defer_out_queue_size_limit);
  // This one is checked in validate-query
  hard_defer_out_queue_size_limit_ = compute_phase_cfg_.size_limits.defer_out_queue_size_limit;
  return true;
}

/**
 * Computes the amount of extra currencies to be minted.
 *
 * @param to_mint A reference to the CurrencyCollection object to store the minted amount.
 *
 * @returns True if the computation is successful, false otherwise.
 */
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

/**
 * Initializes value_flow_ and computes fees for creating the new block.
 *
 * @returns True if the initialization is successful, false otherwise.
 */
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

/**
 * Performs the collation of the new block.
 */
bool Collator::do_collate() {
  // After do_collate started it will not be interrupted by timeout
  alarm_timestamp() = td::Timestamp::never();

  LOG(WARNING) << "do_collate() : start";
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
  allow_repeat_collation_ = true;
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
  // 2-. take messages from dispatch queue
  LOG(INFO) << "process dispatch queue";
  if (!process_dispatch_queue()) {
    return fatal_error("cannot process dispatch queue");
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

/**
 * Dequeues an outbound message from the message queue of this shard.
 *
 * @param msg_envelope The message envelope to dequeue.
 * @param delivered_lt The logical time at which the message was delivered.
 *
 * @returns True if the message was successfully dequeued, false otherwise.
 */
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

/**
 * Cleans up the outbound message queue by removing messages that have already been imported by neighbors.
 * 
 * Cleanup may be interrupted early if it takes too long.
 *
 * @returns True if the cleanup operation was successful, false otherwise.
 */
bool Collator::out_msg_queue_cleanup() {
  LOG(INFO) << "cleaning outbound queue from messages already imported by neighbors";
  if (verbosity >= 2) {
    FLOG(INFO) {
      auto rt = out_msg_queue_->get_root();
      sb << "old out_msg_queue is ";
      block::gen::t_OutMsgQueue.print(sb, rt);
      rt->print_rec(sb);
    };
  }

  if (after_merge_) {
    // We need to clean the whole queue after merge
    // Queue is not too big, see const MERGE_MAX_QUEUE_SIZE
    for (const auto& nb : neighbors_) {
      if (!nb.is_disabled() && (!nb.processed_upto || !nb.processed_upto->can_check_processed())) {
        return fatal_error(-667, PSTRING() << "internal error: no info for checking processed messages from neighbor "
                                           << nb.blk_.to_str());
      }
    }
    td::uint32 deleted = 0;
    auto res = out_msg_queue_->filter([&](vm::CellSlice& cs, td::ConstBitPtr key, int n) -> int {
      assert(n == 352);
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
        ++deleted;
        CHECK(out_msg_queue_size_ > 0);
        --out_msg_queue_size_;
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
          block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
        }
      }
      return !delivered;
    });
    LOG(WARNING) << "deleted " << deleted << " messages from out_msg_queue after merge, remaining queue size is "
                 << out_msg_queue_size_;
    if (res < 0) {
      return fatal_error("error scanning/updating OutMsgQueue");
    }
  } else {
    std::vector<std::pair<block::OutputQueueMerger, const block::McShardDescr*>> queue_parts;

    block::OutputQueueMerger::Neighbor this_queue{BlockIdExt{new_id} /* block id is only used for logs */,
                                                  out_msg_queue_->get_root_cell()};
    for (const auto& nb : neighbors_) {
      if (nb.is_disabled()) {
        continue;
      }
      if (!nb.processed_upto || !nb.processed_upto->can_check_processed()) {
        return fatal_error(-667, PSTRING() << "internal error: no info for checking processed messages from neighbor "
                                           << nb.blk_.to_str());
      }
      queue_parts.emplace_back(block::OutputQueueMerger{nb.shard(), {this_queue}}, &nb);
    }

    size_t i = 0;
    td::uint32 deleted = 0;
    while (!queue_parts.empty()) {
      if (block_full_) {
        LOG(WARNING) << "BLOCK FULL while cleaning up outbound queue, cleanup completed only partially";
        break;
      }
      if (queue_cleanup_timeout_.is_in_past(td::Timestamp::now())) {
        LOG(WARNING) << "cleaning up outbound queue takes too long, ending";
        break;
      }
      if (!check_cancelled()) {
        return false;
      }
      if (i == queue_parts.size()) {
        i = 0;
      }
      auto& queue = queue_parts.at(i).first;
      auto nb = queue_parts.at(i).second;
      auto kv = queue.extract_cur();
      if (kv) {
        block::EnqueuedMsgDescr enq_msg_descr;
        if (!(enq_msg_descr.unpack(kv->msg.write())        // unpack EnqueuedMsg
              && enq_msg_descr.check_key(kv->key.cbits())  // check key
              )) {
          return fatal_error(PSTRING() << "error scanning/updating OutMsgQueue: cannot unpack EnqueuedMsg with key "
                                       << kv->key.to_hex());
        }
        if (nb->processed_upto->already_processed(enq_msg_descr)) {
          LOG(DEBUG) << "scanning outbound message with (lt,hash)=(" << enq_msg_descr.lt_ << ","
                     << enq_msg_descr.hash_.to_hex() << ") enqueued_lt=" << enq_msg_descr.enqueued_lt_
                     << ": message has been already delivered, dequeueing";
          ++deleted;
          CHECK(out_msg_queue_size_ > 0);
          --out_msg_queue_size_;
          out_msg_queue_->lookup_delete_with_extra(kv->key.cbits(), kv->key_len);
          if (!dequeue_message(std::move(enq_msg_descr.msg_env_), nb->end_lt())) {
            return fatal_error(PSTRING() << "cannot dequeue outbound message with (lt,hash)=(" << enq_msg_descr.lt_
                                         << "," << enq_msg_descr.hash_.to_hex()
                                         << ") by inserting a msg_export_deq record");
          }
          register_out_msg_queue_op();
          if (!block_limit_status_->fits(block::ParamLimits::cl_normal)) {
            block_full_ = true;
            block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
          }
          queue.next();
          ++i;
          continue;
        } else {
          LOG(DEBUG) << "scanning outbound message with (lt,hash)=(" << enq_msg_descr.lt_ << ","
                     << enq_msg_descr.hash_.to_hex() << ") enqueued_lt=" << enq_msg_descr.enqueued_lt_
                     << ": message has not been delivered";
        }
      }
      LOG(DEBUG) << "no more unprocessed messages to shard " << nb->shard().to_str();
      std::swap(queue_parts[i], queue_parts.back());
      queue_parts.pop_back();
    }
    LOG(WARNING) << "deleted " << deleted << " messages from out_msg_queue, remaining queue size is "
                 << out_msg_queue_size_;
  }
  if (verbosity >= 2) {
    FLOG(INFO) {
      auto rt = out_msg_queue_->get_root();
      sb << "new out_msg_queue is ";
      block::gen::t_OutMsgQueue.print(sb, rt);
      rt->print_rec(sb);
    };
  }
  return register_out_msg_queue_op(true);
}

/**
 * Creates a new Account object from the given address and serialized account data.
 *
 * @param addr A pointer to the 256-bit address of the account.
 * @param account A cell slice with an account serialized using ShardAccount TLB-scheme.
 * @param force_create A flag indicating whether to force the creation of a new account if `account` is null.
 *
 * @returns A unique pointer to the created Account object, or nullptr if the creation failed.
 */
std::unique_ptr<block::Account> Collator::make_account_from(td::ConstBitPtr addr, Ref<vm::CellSlice> account,
                                                            bool force_create) {
  if (account.is_null() && !force_create) {
    return nullptr;
  }
  auto ptr = std::make_unique<block::Account>(workchain(), addr);
  if (account.is_null()) {
    if (!ptr->init_new(now_)) {
      return nullptr;
    }
  } else if (!ptr->unpack(std::move(account), now_, is_masterchain() && config_->is_special_smartcontract(addr))) {
    return nullptr;
  }
  ptr->block_lt = start_lt;
  return ptr;
}

/**
 * Looks up an account in the Collator's account map.
 *
 * @param addr A pointer to the 256-bit address of the account to be looked up.
 *
 * @returns A pointer to the Account object if found, otherwise returns nullptr.
 */
block::Account* Collator::lookup_account(td::ConstBitPtr addr) const {
  auto found = accounts.find(addr);
  return found != accounts.end() ? found->second.get() : nullptr;
}

/**
 * Retreives an Account object from the data in the shard state.
 * Accounts are cached in the Collator's map.
 *
 * @param addr The 256-bit address of the account.
 * @param force_create Flag indicating whether to create a new account if it does not exist.
 *
 * @returns A Result object containing a pointer to the account if found or created successfully, or an error status.
 *          Returns nullptr if account does not exist and not force_create.
 */
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
  auto new_acc = make_account_from(addr, std::move(dict_entry.first), force_create);
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

/**
 * Combines account transactions and updates the ShardAccountBlocks and ShardAccounts.
 *
 * @returns True if the operation is successful, false otherwise.
 */
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
        FLOG(INFO) {
          sb << "new AccountBlock for " << z.first.to_hex() << ": ";
          block::gen::t_AccountBlock.print_ref(sb, cell);
          csr->print_rec(sb);
        };
      }
      if (!block::gen::t_AccountBlock.validate_ref(100000, cell)) {
        FLOG(WARNING) {
          sb << "AccountBlock failed to pass automatic validation tests: ";
          block::gen::t_AccountBlock.print_ref(sb, cell);
          csr->print_rec(sb);
        };
        return fatal_error(std::string{"new AccountBlock for "} + z.first.to_hex() +
                           " failed to pass automatic validation tests");
      }
      if (!block::tlb::t_AccountBlock.validate_ref(100000, cell)) {
        FLOG(WARNING) {
          sb << "AccountBlock failed to pass handwritten validation tests: ";
          block::gen::t_AccountBlock.print_ref(sb, cell);
          csr->print_rec(sb);
        };
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
            FLOG(INFO) {
              sb << "deleting account " << acc.addr.to_hex() << " with empty new value ";
              block::gen::t_Account.print_ref(sb, acc.total_state);
            };
          }
          if (account_dict->lookup_delete(acc.addr).is_null()) {
            return fatal_error(std::string{"cannot delete account "} + acc.addr.to_hex() + " from ShardAccounts");
          }
        } else {
          // existing account modified
          if (verbosity > 4) {
            FLOG(INFO) {
              sb << "modifying account " << acc.addr.to_hex() << " to ";
              block::gen::t_Account.print_ref(sb, acc.total_state);
            };
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
    FLOG(INFO) {
      sb << "new ShardAccountBlocks: ";
      block::gen::t_ShardAccountBlocks.print_ref(sb, shard_account_blocks_);
      vm::load_cell_slice(shard_account_blocks_).print_rec(sb);
    };
  }
  if (!block::gen::t_ShardAccountBlocks.validate_ref(100000, shard_account_blocks_)) {
    return fatal_error("new ShardAccountBlocks failed to pass automatic validity tests");
  }
  if (!block::tlb::t_ShardAccountBlocks.validate_ref(100000, shard_account_blocks_)) {
    return fatal_error("new ShardAccountBlocks failed to pass handwritten validity tests");
  }
  auto shard_accounts = account_dict->get_root();
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "new ShardAccounts: ";
      block::gen::t_ShardAccounts.print(sb, shard_accounts);
      shard_accounts->print_rec(sb);
    };
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

/**
 * Creates a special transaction to recover a specified amount of currency to a destination address.
 *
 * @param amount The amount of currency to recover.
 * @param dest_addr_cell The cell containing the destination address.
 * @param in_msg The reference to the input message.
 *
 * @returns True if the special transaction was created successfully, false otherwise.
 */
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
    FLOG(INFO) {
      block::gen::t_Message_Any.print_ref(sb, msg);
    };
  }
  CHECK(block::gen::t_Message_Any.validate_ref(msg));
  CHECK(block::tlb::t_Message.validate_ref(msg));
  if (process_one_new_message(block::NewOutMsg{lt, msg, Ref<vm::Cell>{}, 0}, false, &in_msg) != 1) {
    return fatal_error("cannot generate special transaction for recovering "s + amount.to_str() + " to account " +
                       addr.to_hex());
  }
  CHECK(in_msg.not_null());
  return true;
}

/**
 * Creates special transactions for retreiving fees and minted currencies.
 * Used in masterchain collator.
 *
 * @returns True if both special transactions were
 */
bool Collator::create_special_transactions() {
  CHECK(is_masterchain());
  return create_special_transaction(value_flow_.recovered, config_->get_config_param(3, 1), recover_create_msg_) &&
         create_special_transaction(value_flow_.minted, config_->get_config_param(2, 0), mint_msg_);
}

/**
 * Creates a tick-tock transaction for a given smart contract.
 *
 * @param smc_addr The address of the smart contract.
 * @param req_start_lt The requested start logical time for the transaction.
 * @param mask The value indicating whether the thansaction is tick (mask == 2) or tock (mask == 1).
 *
 * @returns True if the transaction was created successfully, false otherwise.
 */
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
  auto it = last_dispatch_queue_emitted_lt_.find(acc->addr);
  if (it != last_dispatch_queue_emitted_lt_.end()) {
    req_start_lt = std::max(req_start_lt, it->second + 1);
  }
  if (acc->last_trans_end_lt_ >= start_lt && acc->transactions.empty()) {
    return fatal_error(td::Status::Error(-666, PSTRING()
                                                   << "last transaction time in the state of account " << workchain()
                                                   << ":" << smc_addr.to_hex() << " is too large"));
  }
  std::unique_ptr<block::transaction::Transaction> trans = std::make_unique<block::transaction::Transaction>(
      *acc, mask == 2 ? block::transaction::Transaction::tr_tick : block::transaction::Transaction::tr_tock,
      req_start_lt, now_);
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
  if (!trans->serialize(serialize_cfg_)) {
    return fatal_error(td::Status::Error(
        -666, std::string{"cannot serialize new transaction for smart contract "} + smc_addr.to_hex()));
  }
  if (!trans->update_limits(*block_limit_status_, /* with_gas = */ false)) {
    return fatal_error(-666, "cannot update block limit status to include the new transaction");
  }
  if (trans->commit(*acc).is_null()) {
    return fatal_error(
        td::Status::Error(-666, std::string{"cannot commit new transaction for smart contract "} + smc_addr.to_hex()));
  }
  if (!update_account_dict_estimation(*trans)) {
    return fatal_error(-666, "cannot update account dict size estimation");
  }
  update_max_lt(acc->last_trans_end_lt_);
  block::MsgMetadata new_msg_metadata{0, acc->workchain, acc->addr, trans->start_lt};
  register_new_msgs(*trans, std::move(new_msg_metadata));
  return true;
}

/**
 * Creates an ordinary transaction using a given message.
 *
 * @param msg_root The root of the message to be processed serialized using Message TLB-scheme.
 * @param msg_metadata Metadata of the inbound message.
 * @param after_lt Transaction lt will be grater than after_lt. Used for deferred messages.
 * @param is_special_tx True if creating a special transaction (mint/recover), false otherwise.
 *
 * @returns The root of the serialized transaction, or an empty reference if the transaction creation fails.
 */
Ref<vm::Cell> Collator::create_ordinary_transaction(Ref<vm::Cell> msg_root,
                                                    td::optional<block::MsgMetadata> msg_metadata, LogicalTime after_lt,
                                                    bool is_special_tx) {
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

  if (external) {
    after_lt = std::max(after_lt, last_proc_int_msg_.first);
  }
  auto it = last_dispatch_queue_emitted_lt_.find(acc->addr);
  if (it != last_dispatch_queue_emitted_lt_.end()) {
    after_lt = std::max(after_lt, it->second);
  }
  auto res = impl_create_ordinary_transaction(msg_root, acc, now_, start_lt, &storage_phase_cfg_, &compute_phase_cfg_,
                                              &action_phase_cfg_, &serialize_cfg_, external, after_lt);
  if (res.is_error()) {
    auto error = res.move_as_error();
    if (error.code() == -701) {
      // ignorable errors
      LOG(DEBUG) << error.message();
      return {};
    }
    fatal_error(std::move(error));
    return {};
  }
  std::unique_ptr<block::transaction::Transaction> trans = res.move_as_ok();

  if (!trans->update_limits(*block_limit_status_,
                            /* with_gas = */ !(is_special_tx && compute_phase_cfg_.special_gas_full))) {
    fatal_error("cannot update block limit status to include the new transaction");
    return {};
  }
  auto trans_root = trans->commit(*acc);
  if (trans_root.is_null()) {
    fatal_error("cannot commit new transaction for smart contract "s + addr.to_hex());
    return {};
  }
  if (!update_account_dict_estimation(*trans)) {
    fatal_error("cannot update account dict size estimation");
    return {};
  }

  td::optional<block::MsgMetadata> new_msg_metadata;
  if (external || is_special_tx) {
    new_msg_metadata = block::MsgMetadata{0, acc->workchain, acc->addr, trans->start_lt};
  } else if (msg_metadata) {
    new_msg_metadata = std::move(msg_metadata);
    ++new_msg_metadata.value().depth;
  }
  register_new_msgs(*trans, std::move(new_msg_metadata));
  update_max_lt(acc->last_trans_end_lt_);
  value_flow_.burned += trans->blackhole_burned;
  return trans_root;
}

/**
 * Creates an ordinary transaction using given parameters.
 *
 * @param msg_root The root of the message to be processed serialized using Message TLB-scheme.
 * @param acc The account for which the transaction is being created.
 * @param utime The Unix time of the transaction.
 * @param lt The minimal logical time of the transaction.
 * @param storage_phase_cfg The configuration for the storage phase of the transaction.
 * @param compute_phase_cfg The configuration for the compute phase of the transaction.
 * @param action_phase_cfg The configuration for the action phase of the transaction.
 * @param serialize_cfg The configuration for the serialization of the transaction.
 * @param external Flag indicating if the message is external.
 * @param after_lt The logical time after which the transaction should occur. Used only for external messages.
 *
 * @returns A Result object containing the created transaction.
 *          Returns error_code == 669 if the error is fatal and the block can not be produced.
 *          Returns error_code == 701 if the transaction can not be included into block, but it's ok (external or too early internal).
 */
td::Result<std::unique_ptr<block::transaction::Transaction>> Collator::impl_create_ordinary_transaction(Ref<vm::Cell> msg_root,
                                                         block::Account* acc,
                                                         UnixTime utime, LogicalTime lt,
                                                         block::StoragePhaseConfig* storage_phase_cfg,
                                                         block::ComputePhaseConfig* compute_phase_cfg,
                                                         block::ActionPhaseConfig* action_phase_cfg,
                                                         block::SerializeConfig* serialize_cfg,
                                                         bool external, LogicalTime after_lt) {
  if (acc->last_trans_end_lt_ >= lt && acc->transactions.empty()) {
    return td::Status::Error(-669, PSTRING() << "last transaction time in the state of account " << acc->workchain
                                             << ":" << acc->addr.to_hex() << " is too large");
  }
  auto trans_min_lt = lt;
  // transactions processing external messages must have lt larger than all processed internal messages
  // if account has deferred message processed in this block, the next transaction should have lt > emitted_lt
  trans_min_lt = std::max(trans_min_lt, after_lt);

  std::unique_ptr<block::transaction::Transaction> trans = std::make_unique<block::transaction::Transaction>(
      *acc, block::transaction::Transaction::tr_ord, trans_min_lt + 1, utime, msg_root);
  bool ihr_delivered = false;  // FIXME
  if (!trans->unpack_input_msg(ihr_delivered, action_phase_cfg)) {
    if (external) {
      // inbound external message was not accepted
      return td::Status::Error(-701, "inbound external message rejected by account "s + acc->addr.to_hex() +
                                         " before smart-contract execution");
    }
    return td::Status::Error(-669, "cannot unpack input message for a new transaction");
  }
  if (trans->bounce_enabled) {
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true)) {
      return td::Status::Error(
          -669, "cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
    if (!external && !trans->prepare_credit_phase()) {
      return td::Status::Error(
          -669, "cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
  } else {
    if (!external && !trans->prepare_credit_phase()) {
      return td::Status::Error(
          -669, "cannot create credit phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
    if (!trans->prepare_storage_phase(*storage_phase_cfg, true, true)) {
      return td::Status::Error(
          -669, "cannot create storage phase of a new transaction for smart contract "s + acc->addr.to_hex());
    }
  }
  if (!trans->prepare_compute_phase(*compute_phase_cfg)) {
    return td::Status::Error(
        -669, "cannot create compute phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (!trans->compute_phase->accepted) {
    if (external) {
      // inbound external message was not accepted
      auto const& cp = *trans->compute_phase;
      return td::Status::Error(
          -701, PSLICE() << "inbound external message rejected by transaction " << acc->addr.to_hex() << ":\n"
                         << "exitcode=" << cp.exit_code << ", steps=" << cp.vm_steps << ", gas_used=" << cp.gas_used
                         << (cp.vm_log.empty() ? "" : "\nVM Log (truncated):\n..." + cp.vm_log));
    } else if (trans->compute_phase->skip_reason == block::ComputePhase::sk_none) {
      return td::Status::Error(-669, "new ordinary transaction for smart contract "s + acc->addr.to_hex() +
                                         " has not been accepted by the smart contract (?)");
    }
  }
  if (trans->compute_phase->success && !trans->prepare_action_phase(*action_phase_cfg)) {
    return td::Status::Error(
        -669, "cannot create action phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (trans->bounce_enabled &&
      (!trans->compute_phase->success || trans->action_phase->state_exceeds_limits || trans->action_phase->bounce) &&
      !trans->prepare_bounce_phase(*action_phase_cfg)) {
    return td::Status::Error(
        -669, "cannot create bounce phase of a new transaction for smart contract "s + acc->addr.to_hex());
  }
  if (!trans->serialize(*serialize_cfg)) {
    return td::Status::Error(-669, "cannot serialize new transaction for smart contract "s + acc->addr.to_hex());
  }
  return std::move(trans);
}

/**
 * Updates the maximum logical time if the given logical time is greater than the current maximum logical time.
 *
 * @param lt The logical time to be compared.
 */
void Collator::update_max_lt(ton::LogicalTime lt) {
  CHECK(lt >= start_lt);
  if (lt > max_lt) {
    max_lt = lt;
  }
}

/**
 * Updates information on the last processed internal message with a new logical time and hash.
 *
 * @param new_lt_hash The new logical time and hash pair.
 *
 * @returns True if the last processed internal message was successfully updated, false otherwise.
 */
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

/**
 * Creates ticktock transactions for special accounts.
 * Used in masterchain collator.
 *
 * @param mask The value indicating whether the thansactions are tick (mask == 2) or tock (mask == 1).
 *
 * @returns True if all ticktock transactions were successfully created, false otherwise.
 */
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

/**
 * Checks if the given address belongs to the current shard.
 *
 * @param addr_ref A reference to a vm::CellSlice object representing the address.
 *
 * @returns True if the address belongs to the current shard, False otherwise.
 */
bool Collator::is_our_address(Ref<vm::CellSlice> addr_ref) const {
  return is_our_address(block::tlb::t_MsgAddressInt.get_prefix(std::move(addr_ref)));
}

/**
 * Checks if the given account ID prefix belongs to the current shard.
 *
 * @param addr_pfx The account ID prefix to check.
 *
 * @returns True if the account ID prefix belongs to the current shard, False otherwise.
 */
bool Collator::is_our_address(ton::AccountIdPrefixFull addr_pfx) const {
  return ton::shard_contains(shard_, addr_pfx);
}

/**
 * Checks if the given address belongs to the current shard.
 *
 * @param addr The address to check.
 *
 * @returns True if the address belongs to the current shard, False otherwise.
 */
bool Collator::is_our_address(const ton::StdSmcAddress& addr) const {
  return ton::shard_contains(get_shard(), addr);
}

/**
 * Processes a message generated in this block or a message from DispatchQueue.
 *
 * @param msg The new message to be processed.
 * @param enqueue_only Flag indicating whether the message should only be enqueued.
 * @param is_special New message if creating a special transaction, nullptr otherwise.
 *
 * @returns Returns:
 *          0 - message was enqueued.
 *          1 - message was processed.
 *          3 - message was processed, all future messages must be enqueued.
 *          -1 - error occured.
 */
int Collator::process_one_new_message(block::NewOutMsg msg, bool enqueue_only, Ref<vm::Cell>* is_special) {
  bool from_dispatch_queue = msg.msg_env_from_dispatch_queue.not_null();
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
      CHECK(info.created_lt == msg.lt && info.created_at == now_ && !from_dispatch_queue);
      src = std::move(info.src);
      enqueue = external = true;
      break;
    }
    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
        return -1;
      }
      CHECK(from_dispatch_queue || (info.created_lt == msg.lt && info.created_at == now_));
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
  CHECK(is_our_address(src));
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

  WorkchainId src_wc;
  StdSmcAddress src_addr;
  CHECK(block::tlb::t_MsgAddressInt.extract_std_address(src, src_wc, src_addr));
  CHECK(src_wc == workchain());
  bool is_special_account = is_masterchain() && config_->is_special_smartcontract(src_addr);
  bool defer = false;
  if (!from_dispatch_queue) {
    if (deferring_messages_enabled_ && collator_opts_->deferring_enabled && !is_special && !is_special_account &&
        !collator_opts_->whitelist.count({src_wc, src_addr}) && msg.msg_idx != 0) {
      if (++sender_generated_messages_count_[src_addr] >= collator_opts_->defer_messages_after ||
          out_msg_queue_size_ > defer_out_queue_size_limit_) {
        defer = true;
      }
    }
    if (dispatch_queue_->lookup(src_addr).not_null() || unprocessed_deferred_messages_.count(src_addr)) {
      defer = true;
    }
  } else {
    auto &x = unprocessed_deferred_messages_[src_addr];
    CHECK(x > 0);
    if (--x == 0) {
      unprocessed_deferred_messages_.erase(src_addr);
    }
  }

  if (enqueue || defer) {
    bool ok;
    if (from_dispatch_queue) {
      auto msg_env = msg.msg_env_from_dispatch_queue;
      block::tlb::MsgEnvelope::Record_std env;
      CHECK(block::tlb::unpack_cell(msg_env, env));
      auto src_prefix = block::tlb::MsgAddressInt::get_prefix(src);
      auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(dest);
      CHECK(env.emitted_lt && env.emitted_lt.value() == msg.lt);
      ok = enqueue_transit_message(std::move(msg.msg), std::move(msg_env), src_prefix, src_prefix, dest_prefix,
                                   std::move(env.fwd_fee_remaining), std::move(env.metadata), msg.lt);
    } else {
      ok = enqueue_message(std::move(msg), std::move(fwd_fees), src_addr, defer);
    }
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
  auto trans_root = create_ordinary_transaction(msg.msg, msg.metadata, msg.lt, is_special != nullptr);
  if (trans_root.is_null()) {
    fatal_error("cannot create transaction for re-processing output message");
    return -1;
  }
  // 2. create a MsgEnvelope enveloping this Message
  block::tlb::MsgEnvelope::Record_std msg_env_rec{0x60, 0x60, fwd_fees, msg.msg, {}, msg.metadata};
  Ref<vm::Cell> msg_env;
  CHECK(block::tlb::pack_cell(msg_env, msg_env_rec));
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "new (processed outbound) message envelope: ";
      block::gen::t_MsgEnvelope.print_ref(sb, msg_env);
    };
  }
  // 3. create InMsg, referring to this MsgEnvelope and this Transaction
  vm::CellBuilder cb;
  if (from_dispatch_queue) {
    auto msg_env = msg.msg_env_from_dispatch_queue;
    block::tlb::MsgEnvelope::Record_std env;
    CHECK(block::tlb::unpack_cell(msg_env, env));
    CHECK(env.emitted_lt && env.emitted_lt.value() == msg.lt);
    CHECK(cb.store_long_bool(0b00100, 5)                                         // msg_import_deferred_fin$00100
          && cb.store_ref_bool(msg_env)                                          // in_msg:^MsgEnvelope
          && cb.store_ref_bool(trans_root)                                       // transaction:^Transaction
          && block::tlb::t_Grams.store_integer_ref(cb, env.fwd_fee_remaining));  // fwd_fee:Grams
  } else {
    CHECK(cb.store_long_bool(3, 3)                                  // msg_import_imm$011
          && cb.store_ref_bool(msg_env)                             // in_msg:^MsgEnvelope
          && cb.store_ref_bool(trans_root)                          // transaction:^Transaction
          && block::tlb::t_Grams.store_integer_ref(cb, fwd_fees));  // fwd_fee:Grams
  }
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
  if (!from_dispatch_queue) {
    // 5. create OutMsg, referring to this MsgEnvelope and InMsg
    CHECK(cb.store_long_bool(2, 3)         // msg_export_imm$010
          && cb.store_ref_bool(msg_env)    // out_msg:^MsgEnvelope
          && cb.store_ref_bool(msg.trans)  // transaction:^Transaction
          && cb.store_ref_bool(in_msg));   // reimport:^InMsg
    // 6. insert OutMsg into OutMsgDescr
    if (!insert_out_msg(cb.finalize())) {
      return -1;
    }
  }
  // 7. check whether the block is full now
  if (!block_limit_status_->fits(block::ParamLimits::cl_normal)) {
    block_full_ = true;
    block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
    return 3;
  }
  if (soft_timeout_.is_in_past(td::Timestamp::now())) {
    LOG(WARNING) << "soft timeout reached, stop processing new messages";
    block_full_ = true;
    return 3;
  }
  return 1;
}

/**
 * Enqueues a transit message.
 * Very similar to enqueue_message(), but for transit messages.
 *
 * @param msg The message to be enqueued.
 * @param old_msg_env The previous message envelope.
 * @param prev_prefix The account ID prefix for this shard.
 * @param cur_prefix The account ID prefix for the next hop.
 * @param dest_prefix The prefix of the destination account ID.
 * @param fwd_fee_remaining The remaining forward fee.
 * @param msg_metadata Metadata of the message.
 * @param emitted_lt If present - the message was taken from DispatchQueue, and msg_env will have this emitted_lt.
 *
 * @returns True if the transit message is successfully enqueued, false otherwise.
 */
bool Collator::enqueue_transit_message(Ref<vm::Cell> msg, Ref<vm::Cell> old_msg_env,
                                       ton::AccountIdPrefixFull prev_prefix, ton::AccountIdPrefixFull cur_prefix,
                                       ton::AccountIdPrefixFull dest_prefix, td::RefInt256 fwd_fee_remaining,
                                       td::optional<block::MsgMetadata> msg_metadata,
                                       td::optional<LogicalTime> emitted_lt) {
  bool from_dispatch_queue = (bool)emitted_lt;
  if (from_dispatch_queue) {
    LOG(DEBUG) << "enqueueing message from dispatch queue " << msg->get_hash().bits().to_hex(256)
               << ", emitted_lt=" << emitted_lt.value();
  } else {
    LOG(DEBUG) << "enqueueing transit message " << msg->get_hash().bits().to_hex(256);
  }
  bool requeue = !from_dispatch_queue && is_our_address(prev_prefix) && !from_dispatch_queue;
  // 1. perform hypercube routing
  auto route_info = block::perform_hypercube_routing(cur_prefix, dest_prefix, shard_);
  if ((unsigned)route_info.first > 96 || (unsigned)route_info.second > 96) {
    return fatal_error("cannot perform hypercube routing for a transit message");
  }
  // 2. compute our part of transit fees
  td::RefInt256 transit_fee =
      from_dispatch_queue ? td::zero_refint() : action_phase_cfg_.fwd_std.get_next_part(fwd_fee_remaining);
  fwd_fee_remaining -= transit_fee;
  CHECK(td::sgn(transit_fee) >= 0 && td::sgn(fwd_fee_remaining) >= 0);
  // 3. create a new MsgEnvelope
  block::tlb::MsgEnvelope::Record_std msg_env_rec{route_info.first, route_info.second,      fwd_fee_remaining, msg,
                                                  emitted_lt,       std::move(msg_metadata)};
  Ref<vm::Cell> msg_env;
  CHECK(block::tlb::t_MsgEnvelope.pack_cell(msg_env, msg_env_rec));
  // 4. create InMsg
  vm::CellBuilder cb;
  if (from_dispatch_queue) {
    CHECK(cb.store_long_bool(0b00101, 5)     // msg_import_deferred_tr$00101
          && cb.store_ref_bool(old_msg_env)  // in_msg:^MsgEnvelope
          && cb.store_ref_bool(msg_env));    // out_msg:^MsgEnvelope
  } else {
    CHECK(cb.store_long_bool(5, 3)                                     // msg_import_tr$101
          && cb.store_ref_bool(old_msg_env)                            // in_msg:^MsgEnvelope
          && cb.store_ref_bool(msg_env)                                // out_msg:^MsgEnvelope
          && block::tlb::t_Grams.store_integer_ref(cb, transit_fee));  // transit_fee:Grams
  }
  Ref<vm::Cell> in_msg = cb.finalize();
  // 5. create a new OutMsg
  // msg_export_tr$011 / msg_export_tr_req$111 / msg_export_deferred_tr$10101
  if (from_dispatch_queue) {
    CHECK(cb.store_long_bool(0b10101, 5));
  } else {
    CHECK(cb.store_long_bool(requeue ? 7 : 3, 3));
  }
  CHECK(cb.store_ref_bool(msg_env)      // out_msg:^MsgEnvelope
        && cb.store_ref_bool(in_msg));  // imported:^InMsg
  Ref<vm::Cell> out_msg = cb.finalize();
  // 4.1. insert OutMsg into OutMsgDescr
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "OutMsg for a transit message: ";
      block::gen::t_OutMsg.print_ref(sb, out_msg);
    };
  }
  if (!insert_out_msg(out_msg)) {
    return fatal_error("cannot insert a new OutMsg into OutMsgDescr");
  }
  // 4.2. insert InMsg into InMsgDescr
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "InMsg for a transit message: ";
      block::gen::t_InMsg.print_ref(sb, in_msg);
    };
  }
  if (!insert_in_msg(in_msg)) {
    return fatal_error("cannot insert a new InMsg into InMsgDescr");
  }
  // 5. create EnqueuedMsg
  CHECK(cb.store_long_bool(from_dispatch_queue ? emitted_lt.value() : start_lt)  // _ enqueued_lt:uint64
        && cb.store_ref_bool(msg_env));                                          // out_msg:^MsgEnvelope = EnqueuedMsg;
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
    ++out_msg_queue_size_;
  } catch (vm::VmError) {
    ok = false;
  }
  if (!ok) {
    LOG(ERROR) << "cannot add an OutMsg into OutMsgQueue dictionary!";
    return false;
  }
  return register_out_msg_queue_op();
}

/**
 * Deletes a message from the outbound message queue.
 *
 * @param key The key of the message to be deleted.
 *
 * @returns True if the message was successfully deleted, false otherwise.
 */
bool Collator::delete_out_msg_queue_msg(td::ConstBitPtr key) {
  Ref<vm::CellSlice> queue_rec;
  try {
    LOG(DEBUG) << "deleting from outbound queue message with key=" << key.to_hex(352);
    queue_rec = out_msg_queue_->lookup_delete(key, 352);
    CHECK(out_msg_queue_size_ > 0);
    --out_msg_queue_size_;
  } catch (vm::VmError err) {
    LOG(ERROR) << "error deleting from out_msg_queue dictionary: " << err.get_msg();
  }
  if (queue_rec.is_null()) {
    return fatal_error(std::string{"cannot dequeue re-processed old message from OutMsgQueue using key "} +
                       key.to_hex(352));
  }
  return register_out_msg_queue_op();
}

/**
 * Processes an inbound message from a neighbor's outbound queue.
 * The message may create a transaction or be enqueued.
 *
 * @param enq_msg The inbound message serialized using EnqueuedMsg TLB-scheme.
 * @param lt The logical time of the message.
 * @param key The 32+64+256-bit key of the message.
 * @param src_nb The description of the source neighbor shard.
 *
 * @returns True if the message was processed successfully, false otherwise.
 */
bool Collator::process_inbound_message(Ref<vm::CellSlice> enq_msg, ton::LogicalTime lt, td::ConstBitPtr key,
                                       const block::McShardDescr& src_nb) {
  ton::LogicalTime enqueued_lt = 0;
  if (enq_msg.is_null() || enq_msg->size_ext() != 0x10040 ||
      (enqueued_lt = enq_msg->prefetch_ulong(64)) < /* 0 */ 1 * lt) {  // DEBUG
    if (enq_msg.not_null()) {
      FLOG(WARNING) {
        sb << "inbound internal message is not a valid EnqueuedMsg: ";
        block::gen::t_EnqueuedMsg.print(sb, enq_msg);
      };
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
    LOG(ERROR) << "inbound internal MsgEnvelope is invalid according to hand-written checks";
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
  if (!env.emitted_lt && info.created_lt != lt) {
    LOG(ERROR) << "inbound internal message has an augmentation value in source OutMsgQueue distinct from the one in "
                  "its contents (CommonMsgInfo)";
    return false;
  }
  if (env.emitted_lt && env.emitted_lt.value() != lt) {
    LOG(ERROR) << "inbound internal message has an augmentation value in source OutMsgQueue distinct from the one in "
                  "its contents (deferred_it in MsgEnvelope)";
    return false;
  }
  if (!block::tlb::validate_message_libs(env.msg)) {
    LOG(ERROR) << "inbound internal message has invalid StateInit";
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

  block::EnqueuedMsgDescr enq_msg_descr{cur_prefix, next_prefix,
                                        env.emitted_lt ? env.emitted_lt.value() : info.created_lt, enqueued_lt,
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
                                 std::move(env.fwd_fee_remaining), std::move(env.metadata))) {
      return fatal_error("cannot enqueue transit internal message with key "s + key.to_hex(352));
    }
    return !our || delete_out_msg_queue_msg(key);
  }
  // destination is in our shard
  // process the message by an ordinary transaction similarly to process_one_new_message()
  //
  // 8. create a Transaction processing this Message
  auto trans_root = create_ordinary_transaction(env.msg, env.metadata, 0);
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

/**
 * Creates a string that explains which limit is exceeded. Used for collator stats.
 *
 * @param block_limit_status Status of block limits.
 * @param cls Which limit class is exceeded.
 *
 * @returns String for collator stats.
 */
static std::string block_full_comment(const block::BlockLimitStatus& block_limit_status, unsigned cls) {
  auto bytes = block_limit_status.estimate_block_size();
  if (!block_limit_status.limits.bytes.fits(cls, bytes)) {
    return PSTRING() << "block_full bytes " << bytes;
  }
  if (!block_limit_status.limits.gas.fits(cls, block_limit_status.gas_used)) {
    return PSTRING() << "block_full gas " << block_limit_status.gas_used;
  }
  auto lt_delta = block_limit_status.cur_lt - block_limit_status.limits.start_lt;
  if (!block_limit_status.limits.lt_delta.fits(cls, lt_delta)) {
    return PSTRING() << "block_full lt_delta " << lt_delta;
  }
  return "";
}

/**
 * Processes inbound internal messages from message queues of the neighbors.
 * Messages are processed until the normal limit is reached, soft timeout is reached or there are no more messages.
 *
 * @returns True if the processing was successful, false otherwise.
 */
bool Collator::process_inbound_internal_messages() {
  if (have_unprocessed_account_dispatch_queue_) {
    return true;
  }
  while (!block_full_ && !nb_out_msgs_->is_eof()) {
    block_full_ = !block_limit_status_->fits(block::ParamLimits::cl_normal);
    if (block_full_) {
      LOG(INFO) << "BLOCK FULL, stop processing inbound internal messages";
      block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
      stats_.limits_log += PSTRING() << "INBOUND_INT_MESSAGES: "
                                     << block_full_comment(*block_limit_status_, block::ParamLimits::cl_normal) << "\n";
      break;
    }
    if (soft_timeout_.is_in_past(td::Timestamp::now())) {
      block_full_ = true;
      LOG(WARNING) << "soft timeout reached, stop processing inbound internal messages";
      stats_.limits_log += PSTRING() << "INBOUND_INT_MESSAGES: timeout\n";
      break;
    }
    if (!check_cancelled()) {
      return false;
    }
    auto kv = nb_out_msgs_->extract_cur();
    CHECK(kv && kv->msg.not_null());
    LOG(DEBUG) << "processing inbound message with (lt,hash)=(" << kv->lt << "," << kv->key.to_hex()
               << ") from neighbor #" << kv->source;
    if (verbosity > 2) {
      FLOG(INFO) {
        sb << "inbound message: lt=" << kv->lt << " from=" << kv->source << " key=" << kv->key.to_hex() << " msg=";
        block::gen::t_EnqueuedMsg.print(sb, kv->msg);
      };
    }
    if (!process_inbound_message(kv->msg, kv->lt, kv->key.cbits(), neighbors_.at(kv->source))) {
      if (verbosity > 1) {
        FLOG(INFO) {
          sb << "invalid inbound message: lt=" << kv->lt << " from=" << kv->source << " key=" << kv->key.to_hex()
                    << " msg=";
          block::gen::t_EnqueuedMsg.print(sb, kv->msg);
        };
      }
      return fatal_error("error processing inbound internal message");
    }
    nb_out_msgs_->next();
  }
  inbound_queues_empty_ = nb_out_msgs_->is_eof();
  return true;
}

/**
 * Processes inbound external messages.
 * Messages are processed until the soft limit is reached, medium timeout is reached or there are no more messages.
 * 
 * @returns True if the processing was successful, false otherwise.
 */
bool Collator::process_inbound_external_messages() {
  if (skip_extmsg_) {
    LOG(INFO) << "skipping processing of inbound external messages";
    return true;
  }
  if (attempt_idx_ >= 2) {
    LOG(INFO) << "Attempt #" << attempt_idx_ << ": skip external messages";
    return true;
  }
  if (out_msg_queue_size_ > SKIP_EXTERNALS_QUEUE_SIZE) {
    LOG(INFO) << "skipping processing of inbound external messages (except for high-priority) because out_msg_queue is "
                 "too big ("
              << out_msg_queue_size_ << " > " << SKIP_EXTERNALS_QUEUE_SIZE << ")";
  }
  bool full = !block_limit_status_->fits(block::ParamLimits::cl_soft);
  for (auto& ext_msg_struct : ext_msg_list_) {
    if (out_msg_queue_size_ > SKIP_EXTERNALS_QUEUE_SIZE && ext_msg_struct.priority < HIGH_PRIORITY_EXTERNAL) {
      continue;
    }
    if (full) {
      LOG(INFO) << "BLOCK FULL, stop processing external messages";
      stats_.limits_log += PSTRING() << "INBOUND_EXT_MESSAGES: "
                                     << block_full_comment(*block_limit_status_, block::ParamLimits::cl_soft) << "\n";
      break;
    }
    if (medium_timeout_.is_in_past(td::Timestamp::now())) {
      LOG(WARNING) << "medium timeout reached, stop processing inbound external messages";
      stats_.limits_log += PSTRING() << "INBOUND_EXT_MESSAGES: timeout\n";
      break;
    }
    if (!check_cancelled()) {
      return false;
    }
    auto ext_msg = ext_msg_struct.cell;
    ton::Bits256 hash{ext_msg->get_hash().bits()};
    int r = process_external_message(std::move(ext_msg));
    if (r > 0) {
      ++stats_.ext_msgs_accepted;
    } else {
      ++stats_.ext_msgs_rejected;
    }
    if (r < 0) {
      bad_ext_msgs_.emplace_back(ext_msg_struct.hash);
      return false;
    }
    if (!r) {
      delay_ext_msgs_.emplace_back(ext_msg_struct.hash);
    }
    if (r > 0) {
      full = !block_limit_status_->fits(block::ParamLimits::cl_soft);
      block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
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

/**
 * Processes an external message.
 *
 * @param msg The message to be processed serialized as Message TLB-scheme.
 *
 * @returns The result of processing the message:
 *          -1 if a fatal error occurred.
 *           0 if the message is rejected.
 *           1 if the message was processed.
 *           3 if the message was processed and all future messages must be skipped (block overflown).
 */
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
  auto trans_root = create_ordinary_transaction(msg, /* metadata = */ {}, 0);
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

/**
 * Processes messages from dispatch queue
 *
 * Messages from dispatch queue are taken in three steps:
 * 1. Take one message from each account (in the order of lt)
 * 2. Take up to 10 per account (including from p.1), up to 20 per initiator, up to 150 in total
 * 3. Take up to X messages per initiator, up to 150 in total. X depends on out msg queue size
 *
 * @returns True if the processing was successful, false otherwise.
 */
bool Collator::process_dispatch_queue() {
  if (out_msg_queue_size_ > defer_out_queue_size_limit_ && old_out_msg_queue_size_ > hard_defer_out_queue_size_limit_) {
    return true;
  }
  have_unprocessed_account_dispatch_queue_ = true;
  size_t max_total_count[3] = {1 << 30, collator_opts_->dispatch_phase_2_max_total,
                               collator_opts_->dispatch_phase_3_max_total};
  size_t max_per_initiator[3] = {1 << 30, collator_opts_->dispatch_phase_2_max_per_initiator, 0};
  if (collator_opts_->dispatch_phase_3_max_per_initiator) {
    max_per_initiator[2] = collator_opts_->dispatch_phase_3_max_per_initiator.value();
  } else if (out_msg_queue_size_ <= 256) {
    max_per_initiator[2] = 10;
  } else if (out_msg_queue_size_ <= 512) {
    max_per_initiator[2] = 2;
  } else if (out_msg_queue_size_ <= 1500) {
    max_per_initiator[2] = 1;
  }
  for (int iter = 0; iter < 3; ++iter) {
    if (max_per_initiator[iter] == 0 || max_total_count[iter] == 0) {
      continue;
    }
    if (iter > 0 && attempt_idx_ >= 1) {
      LOG(INFO) << "Attempt #" << attempt_idx_ << ": skip process_dispatch_queue";
      break;
    }
    vm::AugmentedDictionary cur_dispatch_queue{dispatch_queue_->get_root(), 256, block::tlb::aug_DispatchQueue};
    std::map<std::tuple<WorkchainId, StdSmcAddress, LogicalTime>, size_t> count_per_initiator;
    size_t total_count = 0;
    auto prioritylist = collator_opts_->prioritylist;
    auto prioritylist_iter = prioritylist.begin();
    while (!cur_dispatch_queue.is_empty()) {
      block_full_ = !block_limit_status_->fits(block::ParamLimits::cl_normal);
      if (block_full_) {
        LOG(INFO) << "BLOCK FULL, stop processing dispatch queue";
        block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
        stats_.limits_log += PSTRING() << "DISPATCH_QUEUE_STAGE_" << iter << ": "
                                       << block_full_comment(*block_limit_status_, block::ParamLimits::cl_normal)
                                       << "\n";
        return register_dispatch_queue_op(true);
      }
      if (soft_timeout_.is_in_past(td::Timestamp::now())) {
        block_full_ = true;
        LOG(WARNING) << "soft timeout reached, stop processing dispatch queue";
        stats_.limits_log += PSTRING() << "DISPATCH_QUEUE_STAGE_" << iter << ": timeout\n";
        return register_dispatch_queue_op(true);
      }
      StdSmcAddress src_addr;
      td::Ref<vm::CellSlice> account_dispatch_queue;
      while (!prioritylist.empty()) {
        if (prioritylist_iter == prioritylist.end()) {
          prioritylist_iter = prioritylist.begin();
        }
        auto priority_addr = *prioritylist_iter;
        if (priority_addr.first != workchain() || !is_our_address(priority_addr.second)) {
          prioritylist_iter = prioritylist.erase(prioritylist_iter);
          continue;
        }
        src_addr = priority_addr.second;
        account_dispatch_queue = cur_dispatch_queue.lookup(src_addr);
        if (account_dispatch_queue.is_null()) {
          prioritylist_iter = prioritylist.erase(prioritylist_iter);
        } else {
          ++prioritylist_iter;
          break;
        }
      }
      if (account_dispatch_queue.is_null()) {
        account_dispatch_queue = block::get_dispatch_queue_min_lt_account(cur_dispatch_queue, src_addr);
        if (account_dispatch_queue.is_null()) {
          return fatal_error("invalid dispatch queue in shard state");
        }
      }
      vm::Dictionary dict{64};
      td::uint64 dict_size;
      if (!block::unpack_account_dispatch_queue(account_dispatch_queue, dict, dict_size)) {
        return fatal_error(PSTRING() << "invalid account dispatch queue for account " << src_addr.to_hex());
      }
      td::BitArray<64> key;
      Ref<vm::CellSlice> enqueued_msg = dict.extract_minmax_key(key.bits(), 64, false, false);
      LogicalTime lt = key.to_ulong();

      td::optional<block::MsgMetadata> msg_metadata;
      if (!process_deferred_message(std::move(enqueued_msg), src_addr, lt, msg_metadata)) {
        return fatal_error(PSTRING() << "error processing internal message from dispatch queue: account="
                                     << src_addr.to_hex() << ", lt=" << lt);
      }

      // Remove message from DispatchQueue
      bool ok;
      if (iter == 0 ||
          (iter == 1 && sender_generated_messages_count_[src_addr] >= collator_opts_->defer_messages_after &&
           !collator_opts_->whitelist.count({workchain(), src_addr}))) {
        ok = cur_dispatch_queue.lookup_delete(src_addr).not_null();
      } else {
        dict.lookup_delete(key);
        --dict_size;
        account_dispatch_queue = block::pack_account_dispatch_queue(dict, dict_size);
        ok = account_dispatch_queue.not_null() ? cur_dispatch_queue.set(src_addr, account_dispatch_queue)
                                               : cur_dispatch_queue.lookup_delete(src_addr).not_null();
      }
      if (!ok) {
        return fatal_error(PSTRING() << "error processing internal message from dispatch queue: account="
                                     << src_addr.to_hex() << ", lt=" << lt);
      }
      if (msg_metadata) {
        auto initiator = std::make_tuple(msg_metadata.value().initiator_wc, msg_metadata.value().initiator_addr,
                                         msg_metadata.value().initiator_lt);
        size_t initiator_count = ++count_per_initiator[initiator];
        if (initiator_count >= max_per_initiator[iter]) {
          cur_dispatch_queue.lookup_delete(src_addr);
        }
      }
      ++total_count;
      if (total_count >= max_total_count[iter]) {
        dispatch_queue_total_limit_reached_ = true;
        stats_.limits_log += PSTRING() << "DISPATCH_QUEUE_STAGE_" << iter << ": total limit reached\n";
        break;
      }
    }
    if (iter == 0) {
      have_unprocessed_account_dispatch_queue_ = false;
    }
    register_dispatch_queue_op(true);
  }
  return true;
}

/**
 * Processes an internal message from DispatchQueue.
 * The message may create a transaction or be enqueued.
 *
 * Similar to Collator::process_inbound_message.
 *
 * @param enq_msg The internal message serialized using EnqueuedMsg TLB-scheme.
 * @param src_addr 256-bit address of the sender.
 * @param lt The logical time of the message.
 * @param msg_metadata Reference to store msg_metadata
 *
 * @returns True if the message was processed successfully, false otherwise.
 */
bool Collator::process_deferred_message(Ref<vm::CellSlice> enq_msg, StdSmcAddress src_addr, LogicalTime lt,
                                        td::optional<block::MsgMetadata>& msg_metadata) {
  if (!block::remove_dispatch_queue_entry(*dispatch_queue_, src_addr, lt)) {
    return fatal_error(PSTRING() << "failed to delete message from DispatchQueue: address=" << src_addr.to_hex()
                                 << ", lt=" << lt);
  }
  register_dispatch_queue_op();
  ++sender_generated_messages_count_[src_addr];

  LogicalTime enqueued_lt = 0;
  if (enq_msg.is_null() || enq_msg->size_ext() != 0x10040 || (enqueued_lt = enq_msg->prefetch_ulong(64)) != lt) {
    if (enq_msg.not_null()) {
      FLOG(WARNING) {
        sb << "internal message in DispatchQueue is not a valid EnqueuedMsg: ";
        block::gen::t_EnqueuedMsg.print(sb, enq_msg);
      };
    }
    LOG(ERROR) << "internal message in DispatchQueue is not a valid EnqueuedMsg (created lt " << lt << ", enqueued "
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
    LOG(ERROR) << "MsgEnvelope from DispatchQueue is invalid according to automated checks";
    return false;
  }
  if (!block::tlb::t_MsgEnvelope.validate_ref(msg_env)) {
    LOG(ERROR) << "MsgEnvelope from DispatchQueue is invalid according to hand-written checks";
    return false;
  }
  // 1. unpack MsgEnvelope
  block::tlb::MsgEnvelope::Record_std env;
  if (!tlb::unpack_cell(msg_env, env)) {
    LOG(ERROR) << "cannot unpack MsgEnvelope from DispatchQueue";
    return false;
  }
  // 2. unpack CommonMsgInfo of the message
  vm::CellSlice cs{vm::NoVmOrd{}, env.msg};
  if (block::gen::t_CommonMsgInfo.get_tag(cs) != block::gen::CommonMsgInfo::int_msg_info) {
    LOG(ERROR) << "internal message from DispatchQueue is not in fact internal!";
    return false;
  }
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  if (!tlb::unpack(cs, info)) {
    LOG(ERROR) << "cannot unpack CommonMsgInfo of an internal message from DispatchQueue";
    return false;
  }
  if (info.created_lt != lt) {
    LOG(ERROR) << "internal message has lt in DispatchQueue distinct from the one in "
                  "its contents";
    return false;
  }
  if (!block::tlb::validate_message_libs(env.msg)) {
    LOG(ERROR) << "internal message in DispatchQueue has invalid StateInit";
    return false;
  }
  // 2.1. check fwd_fee and fwd_fee_remaining
  td::RefInt256 orig_fwd_fee = block::tlb::t_Grams.as_integer(info.fwd_fee);
  if (env.fwd_fee_remaining > orig_fwd_fee) {
    LOG(ERROR) << "internal message if DispatchQueue has fwd_fee_remaining=" << td::dec_string(env.fwd_fee_remaining)
               << " larger than original fwd_fee=" << td::dec_string(orig_fwd_fee);
    return false;
  }
  // 3. extract source and destination shards
  auto src_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.src);
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  if (!(src_prefix.is_valid() && dest_prefix.is_valid())) {
    LOG(ERROR) << "internal message in DispatchQueue has invalid source or destination address";
    return false;
  }
  // 4. chech current and next hop shards
  if (env.cur_addr != 0 || env.next_addr != 0) {
    LOG(ERROR) << "internal message in DispatchQueue is expected to have zero cur_addr and next_addr";
    return false;
  }
  // 5. calculate emitted_lt
  LogicalTime emitted_lt = std::max(start_lt, last_dispatch_queue_emitted_lt_[src_addr]) + 1;
  auto it = accounts.find(src_addr);
  if (it != accounts.end()) {
    emitted_lt = std::max(emitted_lt, it->second->last_trans_end_lt_ + 1);
  }
  last_dispatch_queue_emitted_lt_[src_addr] = emitted_lt;
  update_max_lt(emitted_lt + 1);

  env.emitted_lt = emitted_lt;
  if (!block::tlb::pack_cell(msg_env, env)) {
    return fatal_error("cannot pack msg envelope");
  }

  // 6. create NewOutMsg
  block::NewOutMsg new_msg{emitted_lt, env.msg, {}, 0};
  new_msg.metadata = env.metadata;
  new_msg.msg_env_from_dispatch_queue = msg_env;
  ++unprocessed_deferred_messages_[src_addr];
  LOG(INFO) << "delivering deferred message from account " << src_addr.to_hex() << ", lt=" << lt
            << ", emitted_lt=" << emitted_lt;
  block_limit_status_->add_cell(msg_env);
  register_new_msg(std::move(new_msg));
  msg_metadata = std::move(env.metadata);
  return true;
}

/**
 * Inserts an InMsg into the block's InMsgDescr.
 *
 * @param in_msg The input message to be inserted.
 *
 * @returns True if the insertion is successful, false otherwise.
 */
bool Collator::insert_in_msg(Ref<vm::Cell> in_msg) {
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "InMsg being inserted into InMsgDescr: ";
      block::gen::t_InMsg.print_ref(sb, in_msg);
    };
  }
  auto cs = load_cell_slice(in_msg);
  if (!cs.size_refs()) {
    return false;
  }
  Ref<vm::Cell> msg = cs.prefetch_ref();
  int tag = block::gen::t_InMsg.get_tag(cs);
  // msg_import_ext$000 or msg_import_ihr$010 contain (Message Any) directly
  if (!(tag == block::gen::InMsg::msg_import_ext || tag == block::gen::InMsg::msg_import_ihr)) {
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

/**
 * Inserts an OutMsg into the block's OutMsgDescr.
 *
 * @param out_msg The outgoing message to be inserted.
 *
 * @returns True if the insertion was successful, false otherwise.
 */
bool Collator::insert_out_msg(Ref<vm::Cell> out_msg) {
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "OutMsg being inserted into OutMsgDescr: ";
      block::gen::t_OutMsg.print_ref(sb, out_msg);
    };
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

/**
 * Inserts an outgoing message into the block's OutMsgDescr dictionary.
 *
 * @param out_msg The outgoing message to be inserted.
 * @param msg_hash The 256-bit hash of the outgoing message.
 *
 * @returns True if the insertion was successful, false otherwise.
 */
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

/**
 * Enqueues a new message into the block's outbound message queue and OutMsgDescr.
 *
 * @param msg The new outbound message to enqueue.
 * @param fwd_fees_remaining The remaining forward fees for the message.
 * @param src_addr 256-bit address of the sender
 * @param defer Put the message to DispatchQueue
 *
 * @returns True if the message was successfully enqueued, false otherwise.
 */
bool Collator::enqueue_message(block::NewOutMsg msg, td::RefInt256 fwd_fees_remaining, StdSmcAddress src_addr,
                               bool defer) {
  LogicalTime enqueued_lt = msg.lt;
  CHECK(msg.msg_env_from_dispatch_queue.is_null());
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
  block::tlb::MsgEnvelope::Record_std msg_env_rec{
      defer ? 0 : route_info.first, defer ? 0 : route_info.second, fwd_fees_remaining, msg.msg, {}, msg.metadata};
  Ref<vm::Cell> msg_env;
  CHECK(block::tlb::pack_cell(msg_env, msg_env_rec));
  // 3. create a new OutMsg
  vm::CellBuilder cb;
  Ref<vm::Cell> out_msg;
  if (defer) {
    CHECK(cb.store_long_bool(0b10100, 5)     // msg_export_new_defer$10100
          && cb.store_ref_bool(msg_env)      // out_msg:^MsgEnvelope
          && cb.store_ref_bool(msg.trans));  // transaction:^Transaction
    out_msg = cb.finalize();
  } else {
    CHECK(cb.store_long_bool(1, 3)           // msg_export_new$001
          && cb.store_ref_bool(msg_env)      // out_msg:^MsgEnvelope
          && cb.store_ref_bool(msg.trans));  // transaction:^Transaction
    out_msg = cb.finalize();
  }
  // 4. insert OutMsg into OutMsgDescr
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "OutMsg for a newly-generated message: ";
      block::gen::t_OutMsg.print_ref(sb, out_msg);
    };
  }
  if (!insert_out_msg(out_msg)) {
    return fatal_error("cannot insert a new OutMsg into OutMsgDescr");
  }
  // 5. create EnqueuedMsg
  CHECK(cb.store_long_bool(enqueued_lt)  // _ enqueued_lt:uint64
        && cb.store_ref_bool(msg_env));  // out_msg:^MsgEnvelope = EnqueuedMsg;

  // 6. insert EnqueuedMsg into OutMsgQueue (or DispatchQueue)
  if (defer) {
    LOG(INFO) << "deferring new message from account " << workchain() << ":" << src_addr.to_hex() << ", lt=" << msg.lt;
    vm::Dictionary dispatch_dict{64};
    td::uint64 dispatch_dict_size;
    if (!block::unpack_account_dispatch_queue(dispatch_queue_->lookup(src_addr), dispatch_dict, dispatch_dict_size)) {
      return fatal_error(PSTRING() << "cannot unpack AccountDispatchQueue for account " << src_addr.to_hex());
    }
    td::BitArray<64> key;
    key.store_ulong(msg.lt);
    if (!dispatch_dict.set_builder(key, cb, vm::Dictionary::SetMode::Add)) {
      return fatal_error(PSTRING() << "cannot add message to AccountDispatchQueue for account " << src_addr.to_hex()
                                   << ", lt=" << msg.lt);
    }
    ++dispatch_dict_size;
    dispatch_queue_->set(src_addr, block::pack_account_dispatch_queue(dispatch_dict, dispatch_dict_size));
    return register_dispatch_queue_op();
  }

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
    ++out_msg_queue_size_;
  } catch (vm::VmError) {
    ok = false;
  }
  if (!ok) {
    LOG(ERROR) << "cannot add an OutMsg into OutMsgQueue dictionary!";
    return false;
  }
  return register_out_msg_queue_op();
}

/**
 * Processes new messages that were generated in this block.
 *
 * @param enqueue_only If true, only enqueue the new messages without creating transactions.
 *
 * @returns True if all new messages were processed successfully, false otherwise.
 */
bool Collator::process_new_messages(bool enqueue_only) {
  while (!new_msgs.empty()) {
    block::NewOutMsg msg = new_msgs.top();
    new_msgs.pop();
    block_limit_status_->extra_out_msgs--;
    if ((block_full_ || have_unprocessed_account_dispatch_queue_) && !enqueue_only) {
      LOG(INFO) << "BLOCK FULL, enqueue all remaining new messages";
      enqueue_only = true;
      stats_.limits_log += PSTRING() << "NEW_MESSAGES: "
                                     << block_full_comment(*block_limit_status_, block::ParamLimits::cl_normal) << "\n";
    }
    if (!check_cancelled()) {
      return false;
    }
    LOG(DEBUG) << "have message with lt=" << msg.lt;
    int res = process_one_new_message(std::move(msg), enqueue_only);
    if (res < 0) {
      return fatal_error("error processing newly-generated outbound messages");
    } else if (res == 3) {
      LOG(INFO) << "All remaining new messages must be enqueued (BLOCK FULL)";
      enqueue_only = true;
      stats_.limits_log += PSTRING() << "NEW_MESSAGES: "
                                     << block_full_comment(*block_limit_status_, block::ParamLimits::cl_normal) << "\n";
    }
  }
  return true;
}

/**
 * Registers a new output message.
 *
 * @param new_msg The new output message to be registered.
 */
void Collator::register_new_msg(block::NewOutMsg new_msg) {
  if (new_msg.lt < min_new_msg_lt) {
    min_new_msg_lt = new_msg.lt;
  }
  new_msgs.push(std::move(new_msg));
  block_limit_status_->extra_out_msgs++;
}

/**
 * Registers new messages that were created in the transaction.
 *
 * @param trans The transaction containing the messages.
 * @param msg_metadata Metadata of the new messages.
 */
void Collator::register_new_msgs(block::transaction::Transaction& trans,
                                 td::optional<block::MsgMetadata> msg_metadata) {
  CHECK(trans.root.not_null());
  for (unsigned i = 0; i < trans.out_msgs.size(); i++) {
    block::NewOutMsg msg = trans.extract_out_msg_ext(i);
    if (msg_metadata_enabled_) {
      msg.metadata = msg_metadata;
    }
    register_new_msg(std::move(msg));
  }
}

/*
 *
 *  Generate (parts of) new state and block
 *
 */

/**
 * Stores an external block reference to a CellBuilder object.
 *
 * @param cb The CellBuilder object to store the reference in.
 * @param id_ext The block ID.
 * @param end_lt The end logical time of the block.
 *
 * @returns True if the reference was successfully stored, false otherwise.
 */
bool store_ext_blk_ref_to(vm::CellBuilder& cb, const ton::BlockIdExt& id_ext, ton::LogicalTime end_lt) {
  return cb.store_long_bool(end_lt, 64)             // end_lt:uint64
         && cb.store_long_bool(id_ext.seqno(), 32)  // seq_no:uint32
         && cb.store_bits_bool(id_ext.root_hash)    // root_hash:bits256
         && cb.store_bits_bool(id_ext.file_hash);   // file_hash:bits256
}

/**
 * Stores an external block reference to a CellBuilder.
 *
 * @param cb The CellBuilder to store the reference in.
 * @param id_ext The block ID.
 * @param blk_root The root of the block.
 *
 * @returns True if the reference was successfully stored, false otherwise.
 */
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

/**
 * Updates one shard description in the masterchain shard configuration.
 * Used in masterchain collator.
 *
 * @param info The shard information to be updated.
 * @param sibling The sibling shard information.
 * @param wc_info The workchain information.
 * @param now The current Unix time.
 * @param ccvc The Catchain validators configuration.
 * @param update_cc Flag indicating whether to update the Catchain seqno.
 *
 * @returns A boolean value indicating whether the shard description has changed.
 */
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
      info.set_fsm_split(now + wc_info->split_merge_delay, wc_info->split_merge_interval);
      changed = true;
      LOG(INFO) << "preparing to split shard " << info.shard().to_str() << " during " << info.fsm_utime() << " .. "
                << info.fsm_utime_end();
    } else if (info.is_fsm_none() && depth > wc_info->min_split && (info.want_merge_ || depth > wc_info->max_split) &&
               sibling && !sibling->before_split_ && sibling->is_fsm_none() &&
               (sibling->want_merge_ || depth > wc_info->max_split)) {
      // prepare merge
      info.set_fsm_merge(now + wc_info->split_merge_delay, wc_info->split_merge_interval);
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

/**
 * Updates the shard configuration in the masterchain.
 * Used in masterchain collator.
 *
 * @param wc_set The set of workchains.
 * @param ccvc The Catchain validators configuration.
 * @param update_cc A boolean indicating whether to update the Catchain seqno.
 *
 * @returns True if the shard configuration was successfully updated, false otherwise.
 */
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

/**
 * Creates McStateExtra.
 * Used in masterchain collator.
 *
 * @returns True if the creation is successful, false otherwise.
 */
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
    LOG(ERROR) << "configuration smart contract "s + config_addr.to_hex() +
                      " contains an invalid configuration in its data, IGNORING CHANGES";
    FLOG(WARNING) {
      sb << "ignored configuration: ";
      block::gen::t_Hashmap_32_Ref_Cell.print_ref(sb, cfg_smc_config);
    };
    ignore_cfg_changes = true;
  } else {
    cfg0 = cfg_dict.lookup_ref(td::BitArray<32>{(long long)0});
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
  if (!update_shard_config(wset_res.move_as_ok(), ccvc, update_shard_cc)) {
    auto csr = shard_conf_->get_root_csr();
    if (csr.is_null()) {
      LOG(WARNING) << "new shard configuration is null (!)";
    } else {
      LOG(WARNING) << "invalid new shard configuration is";
      FLOG(WARNING) {
        csr->print_rec(sb);
        block::gen::t_ShardHashes.print(sb, csr);
      };
    }
    return fatal_error("cannot post-process shard configuration");
  }
  // 3. save new shard_hashes
  state_extra.shard_hashes = shard_conf_->get_root_csr();
  if (verbosity >= 3) {
    FLOG(INFO) {
      sb << "updated shard configuration to ";
      block::gen::t_ShardHashes.print(sb, state_extra.shard_hashes);
    };
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
  auto nodes = block::Config::do_compute_validator_set(ccvc, shard_, *cur_validators, val_info.catchain_seqno);
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
        FLOG(WARNING) {
          sb << "BlockCreateStats in the new masterchain state failed to pass automated validity checks: ";
          cs->print_rec(sb);
          block::gen::t_BlockCreateStats.print(sb, cs);
        };
        return fatal_error("BlockCreateStats in the new masterchain state failed to pass automated validity checks");
      }
    }
    if (verbosity >= 4 * 1) {
      FLOG(INFO) {
        block::gen::t_BlockCreateStats.print(sb, cs);
      };
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

/**
 * Updates the `block_creator_stats_` for a given key.
 * Used in masterchain collator.
 *
 * @param key The 256-bit key of the creator.
 * @param shard_incr The increment value for the shardchain block counter.
 * @param mc_incr The increment value for the masterchain block counter.
 *
 * @returns True if the block creator count was successfully updated, false otherwise.
 */
bool Collator::update_block_creator_count(td::ConstBitPtr key, unsigned shard_incr, unsigned mc_incr) {
  LOG(DEBUG) << "increasing CreatorStats for " << key.to_hex(256) << " by (" << mc_incr << ", " << shard_incr << ")";
  block::DiscountedCounter mc_cnt, shard_cnt;
  auto cs = block_create_stats_->lookup(key, 256);
  if (!block::unpack_CreatorStats(std::move(cs), mc_cnt, shard_cnt)) {
    return fatal_error("cannot unpack CreatorStats for "s + key.to_hex(256) + " from previous masterchain state");
  }
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

/**
 * Determines if the creator count is outdated for a given key.
 * Used in masterchain collator.
 *
 * @param key The key of the creator.
 * @param cs The CellSlice containing the CreatorStats.
 *
 * @returns -1 if there was a fatal error.
 *           0 if the CreatorStats should be removed as they are stale,
 *           1 if the CreatorStats are still valid.
 */
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

/**
 * Updates `block_create_stats_` using information about creators of all new blocks.
 *
 * @returns True if the update was successful, false otherwise.
 */
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

/**
 * Retrieves the global masterchain config from the config contract.
 *
 * @param cfg_addr The address of the configuration smart contract.
 *
 * @returns A Result object containing a reference to the configuration data.
 */
td::Result<Ref<vm::Cell>> Collator::get_config_data_from_smc(const ton::StdSmcAddress& cfg_addr) {
  return block::get_config_data_from_smc(account_dict->lookup_ref(cfg_addr));
}

/**
 * Fetches and validates a new configuration from the configuration smart contract.
 *
 * @param cfg_addr The address of the configuration smart contract.
 * @param new_config A reference to a vm::Cell object to store the new configuration.
 *
 * @returns True if the new configuration was successfully fetched, false otherwise.
 */
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

/**
 * Computes the weight of a given history of underloaded or overloaded blocks.
 *
 * @param history The history value.
 *
 * @returns The weight of the history.
 */
static int history_weight(td::uint64 history) {
  return td::count_bits64(history & 0xffff) * 3 + td::count_bits64(history & 0xffff0000) * 2 +
         td::count_bits64(history & 0xffff00000000) - (3 + 2 + 1) * 16 * 2 / 3;
}

/**
 * Checks if the current block is overloaded or underloaded based on the block load statistics.
 * Updates the overload and underload history, and sets the want_split or want_merge flags accordingly.
 *
 * @returns True if the check is successful.
 */
bool Collator::check_block_overload() {
  LOG(INFO) << "final out_msg_queue size is " << out_msg_queue_size_;
  overload_history_ <<= 1;
  underload_history_ <<= 1;
  block_size_estimate_ = block_limit_status_->estimate_block_size();
  LOG(INFO) << "block load statistics: gas=" << block_limit_status_->gas_used
            << " lt_delta=" << block_limit_status_->cur_lt - block_limit_status_->limits.start_lt
            << " size_estimate=" << block_size_estimate_;
  block_limit_class_ = std::max(block_limit_class_, block_limit_status_->classify());
  if (block_limit_class_ >= block::ParamLimits::cl_soft || dispatch_queue_total_limit_reached_) {
    std::string message = "block is overloaded ";
    if (block_limit_class_ >= block::ParamLimits::cl_soft) {
      message += PSTRING() << "(category " << block_limit_class_ << ")";
    } else {
      message += "(long dispatch queue processing)";
    }
    if (out_msg_queue_size_ > SPLIT_MAX_QUEUE_SIZE) {
      LOG(INFO) << message << ", but don't set overload history because out_msg_queue size is too big to split ("
                << out_msg_queue_size_ << " > " << SPLIT_MAX_QUEUE_SIZE << ")";
    } else {
      overload_history_ |= 1;
      LOG(INFO) << message;
    }
  } else if (block_limit_class_ <= block::ParamLimits::cl_underload) {
    if (out_msg_queue_size_ > MERGE_MAX_QUEUE_SIZE) {
      LOG(INFO)
          << "block is underloaded, but don't set underload history because out_msg_queue size is too big to merge ("
          << out_msg_queue_size_ << " > " << MERGE_MAX_QUEUE_SIZE << ")";
    } else {
      underload_history_ |= 1;
      LOG(INFO) << "block is underloaded";
    }
  } else {
    LOG(INFO) << "block is loaded normally";
  }
  if (!(overload_history_ & 1) && out_msg_queue_size_ >= FORCE_SPLIT_QUEUE_SIZE &&
      out_msg_queue_size_ <= SPLIT_MAX_QUEUE_SIZE) {
    overload_history_ |= 1;
    LOG(INFO) << "setting overload history because out_msg_queue reached force split limit (" << out_msg_queue_size_
              << " >= " << FORCE_SPLIT_QUEUE_SIZE << ")";
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
    snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)overload_history_);
    LOG(INFO) << "want_split set because of overload history " << buffer;
    want_split_ = true;
  } else if (history_weight(underload_history_) >= 0) {
    snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)underload_history_);
    LOG(INFO) << "want_merge set because of underload history " << buffer;
    want_merge_ = true;
  }
  return true;
}

/**
 * Processes removing a library from the collection of public libraries of an account.
 * Updates the global collection of public libraries.
 * Used in masterchain collator.
 *
 * @param key The 256-bit key of the public library to remove.
 * @param addr The 256-bit address of the account where the library is removed.
 *
 * @returns True if the public library was successfully removed, false otherwise.
 */
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

/**
 * Processes adding a library to the collection of public libraries of an account.
 * Updates the global collection of public libraries.
 * Used in masterchain collator.
 *
 * @param key The key of the public library.
 * @param addr The address of the account where the library is added.
 * @param library The root cell of the library.
 *
 * @returns True if the public library was successfully added, false otherwise.
 */
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

/**
 * Processes changes in libraries of an account.
 * Updates the global collection of public libraries.
 * Used in masterchain collator.
 *
 * @param orig_libs The original libraries of the account.
 * @param final_libs The final libraries of the account.
 * @param addr The address associated with the account.
 *
 * @returns True if the update was successful, false otherwise.
 */
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

/**
 * Processes changes in libraries of all accounts.
 * Updates the global collection of public libraries.
 * Used in masterchain collator.
 *
 * @returns True if the update was successful, false otherwise.
 */
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
  if (libraries_changed_ && verbosity >= 2) {
    FLOG(INFO) {
      sb << "New public libraries: ";
      block::gen::t_HashmapE_256_LibDescr.print(sb, shard_libraries_->get_root());
      shard_libraries_->get_root()->print_rec(sb);
    };
  }
  return true;
}

/**
 * Updates the minimum reference masterchain seqno.
 *
 * @param some_mc_seqno The masterchain seqno to compare with the current minimum.
 *
 * @returns True if the minimum reference masterchain sequence number was updated successfully, false otherwise.
 */
bool Collator::update_min_mc_seqno(ton::BlockSeqno some_mc_seqno) {
  min_ref_mc_seqno_ = std::min(min_ref_mc_seqno_, some_mc_seqno);
  return true;
}

/**
 * Registers an output message queue operation.
 * Adds the proof to the block limit status every 64 operations.
 *
 * @param force If true, the proof will always be added to the block limit status.
 *
 * @returns True if the operation was successfully registered, false otherwise.
 */
bool Collator::register_out_msg_queue_op(bool force) {
  ++out_msg_queue_ops_;
  if (force || !(out_msg_queue_ops_ & 63)) {
    return block_limit_status_->add_proof(out_msg_queue_->get_root_cell());
  } else {
    return true;
  }
}

/**
 * Registers a dispatch queue message queue operation.
 * Adds the proof to the block limit status every 64 operations.
 *
 * @param force If true, the proof will always be added to the block limit status.
 *
 * @returns True if the operation was successfully registered, false otherwise.
 */
bool Collator::register_dispatch_queue_op(bool force) {
  ++dispatch_queue_ops_;
  if (force || !(dispatch_queue_ops_ & 63)) {
    return block_limit_status_->add_proof(dispatch_queue_->get_root_cell());
  } else {
    return true;
  }
}

/**
 * Update size estimation for the account dictionary.
 * This is required to count the depth of the ShardAccounts dictionary in the block size estimation.
 * account_dict_estimator_ is used for block limits only.
 *
 * @param trans Newly-created transaction.
 *
 * @returns True on success, false otherwise.
 */
bool Collator::update_account_dict_estimation(const block::transaction::Transaction& trans) {
  const block::Account& acc = trans.account;
  if (acc.orig_total_state->get_hash() != acc.total_state->get_hash() &&
      account_dict_estimator_added_accounts_.insert(acc.addr).second) {
    // see combine_account_transactions
    if (acc.status == block::Account::acc_nonexist) {
      account_dict_estimator_->lookup_delete(acc.addr);
    } else {
      vm::CellBuilder cb;
      if (!(cb.store_ref_bool(acc.total_state)             // account_descr$_ account:^Account
            && cb.store_bits_bool(acc.last_trans_hash_)    // last_trans_hash:bits256
            && cb.store_long_bool(acc.last_trans_lt_, 64)  // last_trans_lt:uint64
            && account_dict_estimator_->set_builder(acc.addr, cb))) {
        return false;
      }
    }
  }
  ++account_dict_ops_;
  if (!(account_dict_ops_ & 15)) {
    return block_limit_status_->add_proof(account_dict_estimator_->get_root_cell());
  }
  return true;
}

/**
 * Creates a new shard state and the Merkle update.
 *
 * @returns True if the shard state and Merkle update were successfully created, false otherwise.
 */
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
    FLOG(INFO) {
      sb << "new ShardState: ";
      block::gen::t_ShardState.print_ref(sb, state_root);
      vm::load_cell_slice(state_root).print_rec(sb);
    };
  }
  if (verify >= 2) {
    LOG(INFO) << "verifying new ShardState";
    CHECK(block::gen::t_ShardState.validate_ref(1000000, state_root));
    CHECK(block::tlb::t_ShardState.validate_ref(1000000, state_root));
  }
  LOG(INFO) << "creating Merkle update for the ShardState";
  state_update = vm::MerkleUpdate::generate(prev_state_root_, state_root, state_usage_tree_.get());
  if (state_update.is_null()) {
    return fatal_error("cannot create Merkle update for ShardState");
  }
  if (verbosity > 2) {
    FLOG(INFO) {
      sb << "Merkle Update for ShardState: ";
      vm::CellSlice cs{vm::NoVm{}, state_update};
      cs.print_rec(sb);
    };
  }
  LOG(INFO) << "updating block profile statistics";
  block_limit_status_->add_proof(state_root);
  LOG(INFO) << "new ShardState and corresponding Merkle update created";
  return true;
}

/**
 * Stores BlkMasterInfo (for non-masterchain blocks) in the provided CellBuilder.
 *
 * @param cb The CellBuilder to store the reference in.
 *
 * @returns True if the reference is successfully stored, false otherwise.
 */
bool Collator::store_master_ref(vm::CellBuilder& cb) {
  return mc_block_root.not_null() && store_ext_blk_ref_to(cb, mc_block_id_, mc_block_root);
}

/**
 * Updates the processed_upto information for the new block based on the information on the last processed inbound message.
 */
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

/**
 * Computes the outbound message queue.
 *
 * @param out_msg_queue_info A reference to a vm::Cell object to store the computed queue.
 *
 * @returns True if the computation is successful, False otherwise.
 */
bool Collator::compute_out_msg_queue_info(Ref<vm::Cell>& out_msg_queue_info) {
  if (verbosity >= 2) {
    FLOG(INFO) {
      auto rt = out_msg_queue_->get_root();
      sb << "resulting out_msg_queue is ";
      block::gen::t_OutMsgQueue.print(sb, rt);
      rt->print_rec(sb);
    };
  }
  vm::CellBuilder cb;
  // out_msg_queue_extra#0 dispatch_queue:DispatchQueue out_queue_size:(Maybe uint48) = OutMsgQueueExtra;
  // ... extra:(Maybe OutMsgQueueExtra)
  if (!dispatch_queue_->is_empty() || store_out_msg_queue_size_) {
    if (!(cb.store_long_bool(1, 1) && cb.store_long_bool(0, 4) && dispatch_queue_->append_dict_to_bool(cb))) {
      return false;
    }
    if (!(cb.store_bool_bool(store_out_msg_queue_size_) &&
          (!store_out_msg_queue_size_ || cb.store_long_bool(out_msg_queue_size_, 48)))) {
      return false;
    }
  } else {
    if (!cb.store_long_bool(0, 1)) {
      return false;
    }
  }
  vm::CellSlice maybe_extra = cb.as_cellslice();
  cb.reset();

  return register_out_msg_queue_op(true) && register_dispatch_queue_op(true) &&
         out_msg_queue_->append_dict_to_bool(cb)   // _ out_queue:OutMsgQueue
         && processed_upto_->pack(cb)              // proc_info:ProcessedInfo
         && cb.append_cellslice_bool(maybe_extra)  // extra:(Maybe OutMsgQueueExtra)
         && cb.finalize_to(out_msg_queue_info);
}

/**
 * Computes the total balance of the shard state.
 *
 * @returns True if the total balance computation is successful, false otherwise.
 */
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
    FLOG(INFO) {
      block::gen::t_ImportFees.print(sb, in_msg_dict->get_root_extra());
      cs.print_rec(sb);
    };
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
  block::CurrencyCollection total_fees = new_transaction_fees + new_import_fees;
  value_flow_.fees_collected += total_fees;
  if (is_masterchain()) {
    block::CurrencyCollection burned = config_->get_burning_config().calculate_burned_fees(total_fees);
    if (!burned.is_valid()) {
      return fatal_error("cannot calculate amount of burned masterchain fees");
    }
    value_flow_.fees_collected -= burned;
    value_flow_.burned += burned;
  }
  // 3. compute total_validator_fees
  total_validator_fees_ += value_flow_.fees_collected;
  total_validator_fees_ -= value_flow_.recovered;
  CHECK(total_validator_fees_.is_valid());
  return true;
}

/**
 * Creates BlockInfo of the new block.
 *
 * @param block_info A reference to the cell to put the serialized info to.
 *
 * @returns True if the block info cell was successfully created, false otherwise.
 */
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

/**
 * Stores the version information in a CellBuilder.
 *
 * @param cb The CellBuilder object to store the version information.
 *
 * @returns True if the version information was successfully stored, false otherwise.
 */
bool Collator::store_version(vm::CellBuilder& cb) const {
  return block::gen::t_GlobalVersion.pack_capabilities(cb, supported_version(), supported_capabilities());
}

/**
 * Stores the zero state reference in the given CellBuilder.
 *
 * @param cb The CellBuilder to store the zero state reference in.
 *
 * @returns True if the zero state reference is successfully stored, false otherwise.
 */
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

/**
 * Stores the previous block references to the given CellBuilder.
 *
 * @param cb The CellBuilder object to store the references.
 * @param is_after_merge A boolean indicating whether the new block after a merge.
 *
 * @returns True if the references are successfully stored, false otherwise.
 */
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

/**
 * Validates the value flow of the block.
 *
 * @returns True if the value flow is correct, false otherwise.
 */
bool Collator::check_value_flow() {
  if (!value_flow_.validate()) {
    LOG(ERROR) << "incorrect value flow in new block : " << value_flow_.to_str();
    return fatal_error("incorrect value flow for the newly-generated block: in != out");
  }
  LOG(INFO) << "Value flow: " << value_flow_.to_str();
  return true;
}

/**
 * Creates the BlockExtra of the new block.
 *
 * @param block_extra A reference to the cell to put the serialized info to.
 *
 * @returns True if the block extra data was successfully created, false otherwise.
 */
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

/**
 * Creates the McBlockExtra of the new masterchain block.
 * Used in masterchain collator.
 *
 * @param mc_block_extra A reference to the cell to put the serialized info to.
 *
 * @returns True if the extra data was successfully created, false otherwise.
 */
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

/**
 * Serialized the new block.
 *
 * This function performs the following steps:
 * 1. Creates a BlockInfo for the new block.
 * 2. Checks the value flow for the new block.
 * 3. Creates a BlockExtra for the new block.
 * 4. Builds a new block using the created BlockInfo, value flow, state update, and BlockExtra.
 * 5. Verifies the new block if the verification is enabled.
 *
 * @returns True if the new block is successfully created, false otherwise.
 */
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
    FLOG(INFO) {
      sb << "new Block: ";
      block::gen::t_Block.print_ref(sb, new_block);
      vm::load_cell_slice(new_block).print_rec(sb);
    };
  }
  if (verify >= 1) {
    LOG(INFO) << "verifying new Block";
    if (!block::gen::t_Block.validate_ref(10000000, new_block)) {
      return fatal_error("new Block failed to pass automatic validity tests");
    }
  }
  LOG(INFO) << "new Block created";
  return true;
}

/**
 * Collates the shard block description set.
 * Used in masterchain collator.
 *
 * This function creates a dictionary and populates it with the shard block descriptions.
 *
 * @returns A `Ref<vm::Cell>` containing the serialized `TopBlockDescrSet` record.
 *          If serialization fails, an empty `Ref<vm::Cell>` is returned.
 */
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
    FLOG(INFO) {
      sb << "serialized TopBlockDescrSet for collated data is: ";
      block::gen::t_TopBlockDescrSet.print_ref(sb, cell);
      vm::load_cell_slice(cell).print_rec(sb);
    };
  }
  return cell;
}

/**
 * Creates collated data for the block.
 *
 * @returns True if the collated data was successfully created, false otherwise.
 */
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

/**
 * Creates a block candidate for the Collator.
 *
 * This function serializes the new block and collated data, and creates a BlockCandidate object
 * with the necessary information. It then checks if the size of the block candidate exceeds the
 * limits specified in the consensus configuration.
 *
 * Finally, the block candidate is saved to the disk.
 * If there are any bad external messages or delayed external messages, the ValidatorManager is called to handle them.
 *
 * @returns True if the block candidate was created successfully, false otherwise.
 */
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
  // 3.1 check block and collated data size
  auto consensus_config = config_->get_consensus_config();
  if (block_candidate->data.size() > consensus_config.max_block_size) {
    return fatal_error(PSTRING() << "block size (" << block_candidate->data.size()
                                 << ") exceeds the limit in consensus config (" << consensus_config.max_block_size
                                 << ")");
  }
  if (block_candidate->collated_data.size() > consensus_config.max_collated_data_size) {
    return fatal_error(PSTRING() << "collated data size (" << block_candidate->collated_data.size()
                                 << ") exceeds the limit in consensus config ("
                                 << consensus_config.max_collated_data_size << ")");
  }
  // 4. save block candidate
  if (mode_ & CollateMode::skip_store_candidate) {
    td::actor::send_closure_later(actor_id(this), &Collator::return_block_candidate, td::Unit());
  } else {
    LOG(INFO) << "saving new BlockCandidate";
    td::actor::send_closure_later(
        manager, &ValidatorManager::set_block_candidate, block_candidate->id, block_candidate->clone(),
        validator_set_->get_catchain_seqno(), validator_set_->get_validator_set_hash(),
        [self = get_self()](td::Result<td::Unit> saved) -> void {
          LOG(DEBUG) << "got answer to set_block_candidate";
          td::actor::send_closure_later(std::move(self), &Collator::return_block_candidate, std::move(saved));
        });
  }
  // 5. communicate about bad and delayed external messages
  if (!bad_ext_msgs_.empty() || !delay_ext_msgs_.empty()) {
    LOG(INFO) << "sending complete_external_messages() to Manager";
    td::actor::send_closure_later(manager, &ValidatorManager::complete_external_messages, std::move(delay_ext_msgs_),
                                  std::move(bad_ext_msgs_));
  }

  double work_time = work_timer_.elapsed();
  double cpu_work_time = cpu_work_timer_.elapsed();
  LOG(WARNING) << "Collate query work time = " << work_time << "s, cpu time = " << cpu_work_time << "s";
  stats_.bytes = block_limit_status_->estimate_block_size();
  stats_.gas = block_limit_status_->gas_used;
  stats_.lt_delta = block_limit_status_->cur_lt - block_limit_status_->limits.start_lt;
  stats_.cat_bytes = block_limit_status_->limits.classify_size(stats_.bytes);
  stats_.cat_gas = block_limit_status_->limits.classify_gas(stats_.gas);
  stats_.cat_lt_delta = block_limit_status_->limits.classify_lt(block_limit_status_->cur_lt);
  td::actor::send_closure(manager, &ValidatorManager::record_collate_query_stats, block_candidate->id, work_time,
                          cpu_work_time, std::move(stats_));
  return true;
}

/**
 * Returns a block candidate to the Promise.
 *
 * @param saved The result of saving the block candidate to the disk.
 */
void Collator::return_block_candidate(td::Result<td::Unit> saved) {
  // 6. return data to the original "caller"
  if (saved.is_error()) {
    auto err = saved.move_as_error();
    LOG(ERROR) << "cannot save block candidate: " << err.to_string();
    fatal_error(std::move(err));
  } else {
    CHECK(block_candidate);
    LOG(WARNING) << "sending new BlockCandidate to Promise";
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

/**
 * Registers an external message to the list of external messages in the Collator.
 *
 * @param ext_msg The reference to the external message cell.
 * @param ext_hash The hash of the external message.
 *
 * @returns Result indicating the success or failure of the registration.
 *          - If the external message is invalid, returns an error.
 *          - If the external message has been previously rejected, returns an error
 *          - If the external message has been previously registered and accepted, returns false.
 *          - Otherwise returns true.
 */
td::Result<bool> Collator::register_external_message_cell(Ref<vm::Cell> ext_msg, const ExtMessage::Hash& ext_hash,
                                                          int priority) {
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
  if (!block::tlb::validate_message_libs(ext_msg)) {
    return td::Status::Error("external message has invalid libs in StateInit");
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
    FLOG(INFO) {
      sb << "registered external message: ";
      block::gen::t_Message_Any.print_ref(sb, ext_msg);
    };
  }
  ext_msg_map.emplace(hash, 1);
  ext_msg_list_.push_back({std::move(ext_msg), ext_hash, priority});
  return true;
}

/**
 * Callback function called after retrieving external messages.
 *
 * @param res The result of the external message retrieval operation.
 */
void Collator::after_get_external_messages(td::Result<std::vector<std::pair<Ref<ExtMessage>, int>>> res) {
  // res: pair {ext msg, priority}
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  auto vect = res.move_as_ok();
  for (auto& p : vect) {
    ++stats_.ext_msgs_total;
    auto& ext_msg = p.first;
    int priority = p.second;
    Ref<vm::Cell> ext_msg_cell = ext_msg->root_cell();
    bool err = ext_msg_cell.is_null();
    if (!err) {
      auto reg_res = register_external_message_cell(std::move(ext_msg_cell), ext_msg->hash(), priority);
      if (reg_res.is_error() || !reg_res.move_as_ok()) {
        err = true;
      }
    }
    if (err) {
      ++stats_.ext_msgs_filtered;
      bad_ext_msgs_.emplace_back(ext_msg->hash());
    }
  }
  LOG(WARNING) << "got " << vect.size() << " external messages from mempool, " << bad_ext_msgs_.size()
               << " bad messages";
  check_pending();
}

/**
 * Checks if collation was cancelled via cancellation token
 *
 * @returns false if the collation was cancelled, true otherwise
 */
bool Collator::check_cancelled() {
  if (cancellation_token_) {
    return fatal_error(td::Status::Error(ErrorCode::cancelled, "cancelled"));
  }
  return true;
}

td::uint32 Collator::get_skip_externals_queue_size() {
  return SKIP_EXTERNALS_QUEUE_SIZE;
}

}  // namespace validator

}  // namespace ton
