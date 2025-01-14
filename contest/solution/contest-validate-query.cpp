#include "contest-validate-query.hpp"
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
#include "fabric.h"
#include <ctime>

namespace solution {

using namespace ton;
using namespace ton::validator;

using td::Ref;
using namespace std::literals::string_literals;

/**
 * Converts the error context to a string representation to show it in case of validation error.
 *
 * @returns The error context as a string.
 */
std::string ErrorCtx::as_string() const {
  std::string a;
  for (const auto& s : entries_) {
    a += s;
    a += " : ";
  }
  return a;
}

/**
 * Constructs a ContestValidateQuery object.
 *
 * @param block_id Id of the block
 * @param block_data Block data, but without state update
 * @param collated_data Collated data (proofs of shard states)
 * @param promise The Promise to return the serialized state update to
 */
ContestValidateQuery::ContestValidateQuery(BlockIdExt block_id, td::BufferSlice block_data,
                                           td::BufferSlice collated_data, td::Promise<td::BufferSlice> promise)
    : shard_(block_id.shard_full())
    , id_(block_id)
    , block_data(std::move(block_data))
    , collated_data(std::move(collated_data))
    , main_promise(std::move(promise))
    , shard_pfx_(shard_.shard)
    , shard_pfx_len_(ton::shard_prefix_length(shard_)) {
}

/**
 * Aborts the validation with the given error.
 *
 * @param error The error encountered.
 */
void ContestValidateQuery::abort_query(td::Status error) {
  (void)fatal_error(std::move(error));
}

/**
 * Rejects the validation and logs an error message.
 *
 * @param error The error message to be logged.
 * @param reason The reason for rejecting the validation.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::reject_query(std::string error, td::BufferSlice reason) {
  error = error_ctx() + error;
  LOG(WARNING) << "REJECT: aborting validation of block candidate for " << shard_.to_str() << " : " << error;
  if (main_promise) {
    main_promise.set_error(td::Status::Error(error));
  }
  stop();
  return false;
}

/**
 * Rejects the validation and logs an error message.
 *
 * @param err_msg The error message to be displayed.
 * @param error The error status.
 * @param reason The reason for rejecting the query.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::reject_query(std::string err_msg, td::Status error, td::BufferSlice reason) {
  error.ensure_error();
  return reject_query(err_msg + " : " + error.to_string(), std::move(reason));
}

/**
 * Rejects the validation and logs an error message.
 *
 * @param error The error message to be logged.
 * @param reason The reason for rejecting the validation.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::soft_reject_query(std::string error, td::BufferSlice reason) {
  error = error_ctx() + error;
  LOG(WARNING) << "SOFT REJECT: aborting validation of block candidate for " << shard_.to_str() << " : " << error;
  if (main_promise) {
    main_promise.set_error(td::Status::Error(std::move(error)));
  }
  stop();
  return false;
}

/**
 * Handles a fatal error during validation.
 *
 * @param error The error status.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::fatal_error(td::Status error) {
  error.ensure_error();
  LOG(WARNING) << "aborting validation of block candidate for " << shard_.to_str() << " : " << error.to_string();
  if (main_promise) {
    main_promise.set_error(std::move(error));
  }
  stop();
  return false;
}

/**
 * Handles a fatal error during validation.
 *
 * @param err_code Error code.
 * @param err_msg Error message.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::fatal_error(int err_code, std::string err_msg) {
  return fatal_error(td::Status::Error(err_code, error_ctx() + err_msg));
}

/**
 * Handles a fatal error during validation.
 *
 * @param err_code Error code.
 * @param err_msg Error message.
 * @param error Error status.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::fatal_error(int err_code, std::string err_msg, td::Status error) {
  error.ensure_error();
  return fatal_error(err_code, err_msg + " : " + error.to_string());
}

/**
 * Handles a fatal error during validation.
 *
 * @param err_msg Error message.
 * @param err_code Error code.
 *
 * @returns False indicating that the validation failed.
 */
bool ContestValidateQuery::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, error_ctx() + err_msg));
}

/**
 * Finishes the query and sends the result to the promise.
 */
void ContestValidateQuery::finish_query() {
  if (main_promise) {
    LOG(WARNING) << "validate query done";
    main_promise.set_result(std::move(result_state_update_));
  }
  stop();
}

/*
 *
 *   INITIAL PARSE & LOAD REQUIRED DATA
 *
 */

/**
 * Starts the validation process.
 *
 * This function performs various checks on the validation parameters and the block candidate.
 * Then the function also sends requests to the ValidatorManager to fetch blocks and shard stated.
 */
void ContestValidateQuery::start_up() {
  LOG(INFO) << "validate query for " << id_.to_str() << " started";
  rand_seed_.set_zero();

  if (ShardIdFull(id_) != shard_) {
    soft_reject_query(PSTRING() << "block candidate belongs to shard " << ShardIdFull(id_).to_str()
                                << " different from current shard " << shard_.to_str());
    return;
  }
  if (workchain() != ton::basechainId) {
    soft_reject_query("only basechain is supported");
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
  // 3. unpack block candidate (while necessary data is being loaded)
  if (!unpack_block_candidate()) {
    reject_query("error unpacking block candidate");
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
      if (!shard_is_parent(ShardIdFull(prev_blocks[0]), shard_)) {
        soft_reject_query("previous block does not belong to the shard we are generating a new block for");
        return;
      }
    }
    if (after_split_) {
      // soft_reject_query("splitting shards not implemented yet");
      // return;
    }
  }
  // 4. load state(s) corresponding to previous block(s)
  prev_states.resize(prev_blocks.size());
  for (int i = 0; (unsigned)i < prev_blocks.size(); i++) {
    // 4.1. load state
    LOG(DEBUG) << "sending wait_block_state() query #" << i << " for " << prev_blocks[i].to_str() << " to Manager";
    ++pending;
    td::actor::send_closure_later(actor_id(this), &ContestValidateQuery::after_get_shard_state, i,
                                  fetch_block_state(prev_blocks[i]));
  }
  // 5. request masterchain state referred to in the block
  ++pending;
  td::actor::send_closure_later(actor_id(this), &ContestValidateQuery::after_get_mc_state,
                                fetch_block_state(mc_blkid_));
  // ...
  CHECK(pending);
}

/**
 * Unpacks and validates a block candidate.
 *
 * This function unpacks the block candidate data and performs various validation checks to ensure its integrity.
 * It checks the file hash and root hash of the block candidate against the expected values.
 * It then parses the block header and checks its validity.
 * Finally, it deserializes the collated data and extracts the collated roots.
 *
 * @returns True if the block candidate was successfully unpacked, false otherwise.
 */
bool ContestValidateQuery::unpack_block_candidate() {
  vm::BagOfCells boc1, boc2;
  // 1. deserialize block itself
  auto res1 = boc1.deserialize(block_data);
  if (res1.is_error()) {
    return reject_query("cannot deserialize block", res1.move_as_error());
  }
  if (boc1.get_root_count() != 1) {
    return reject_query("block BoC must contain exactly one root");
  }
  block_root_ = boc1.get_root_cell();
  CHECK(block_root_.not_null());
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
  auto res2 = boc2.deserialize(collated_data);
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

/**
 * Initializes the validation by parsing and checking the block header.
 *
 * @returns True if the initialization is successful, false otherwise.
 */
bool ContestValidateQuery::init_parse() {
  CHECK(block_root_.not_null());
  std::vector<BlockIdExt> prev_blks;
  bool after_split;
  auto res = block::unpack_block_prev_blk_try(block_root_, id_, prev_blks, mc_blkid_, after_split, nullptr, true);
  if (res.is_error()) {
    return reject_query("cannot unpack block header : "s + res.to_string());
  }
  CHECK(mc_blkid_.id.is_masterchain_ext());
  mc_seqno_ = mc_blkid_.seqno();
  prev_blocks = prev_blks;
  after_merge_ = prev_blocks.size() == 2;
  after_split_ = !after_merge_ && prev_blocks[0].shard_full() != shard_;
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
  global_id_ = blk.global_id;
  vert_seqno_ = info.vert_seq_no;
  start_lt_ = info.start_lt;
  end_lt_ = info.end_lt;
  now_ = info.gen_utime;
  before_split_ = info.before_split;
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
  if (is_key_block_) {
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
  created_by_ = extra.created_by;
  if (extra.custom->size_refs()) {
    return reject_query("non-masterchain block cannot have McBlockExtra");
  }
  // ...
  return true;
}

/**
 * Extracts collated data from a cell.
 *
 * @param croot The root cell containing the collated data.
 * @param idx The index of the root.
 *
 * @returns True if the extraction is successful, false otherwise.
 */
bool ContestValidateQuery::extract_collated_data_from(Ref<vm::Cell> croot, int idx) {
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
  if (block::gen::t_ExtraCollatedData.has_valid_tag(cs)) {
    LOG(DEBUG) << "collated datum # " << idx << " is an ExtraCollatedData";
    if (!block::gen::unpack(cs, extra_collated_data_)) {
      return reject_query("invalid ExtraCollatedData");
    }
    have_extra_collated_data_ = true;
    return true;
  }
  LOG(INFO) << "collated datum # " << idx << " has unknown type (magic " << cs.prefetch_ulong(32) << "), ignoring";
  return true;
}

/**
 * Extracts collated data from a list of collated roots.
 *
 * @returns True if the extraction is successful, False otherwise.
 */
bool ContestValidateQuery::extract_collated_data() {
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
  if (!have_extra_collated_data_) {
    return reject_query("no extra collated data");
  }
  return true;
}

/**
 * Callback function called after retrieving the masterchain state referenced int the block.
 *
 * @param res The result of the masterchain state retrieval.
 */
void ContestValidateQuery::after_get_mc_state(td::Result<Ref<ShardState>> res) {
  LOG(INFO) << "in ContestValidateQuery::after_get_mc_state() for " << mc_blkid_.to_str();
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

/**
 * Callback function called after retrieving the shard state for a previous block.
 *
 * @param idx The index of the previous block (0 or 1).
 * @param res The result of the shard state retrieval.
 */
void ContestValidateQuery::after_get_shard_state(int idx, td::Result<Ref<ShardState>> res) {
  LOG(INFO) << "in ContestValidateQuery::after_get_shard_state(" << idx << ")";
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
  if (!pending) {
    if (!try_validate()) {
      fatal_error("cannot validate new block");
    }
  }
}

/**
 * Processes the retreived masterchain state.
 *
 * @param mc_state The reference to the masterchain state.
 *
 * @returns True if the masterchain state is successfully processed, false otherwise.
 */
bool ContestValidateQuery::process_mc_state(Ref<MasterchainState> mc_state) {
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

/**
 * Tries to unpack the masterchain state and perform necessary checks.
 *
 * @returns True if the unpacking and checks are successful, false otherwise.
 */
bool ContestValidateQuery::try_unpack_mc_state() {
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
        mc_state_root_, block::ConfigInfo::needShardHashes | block::ConfigInfo::needLibraries |
                            block::ConfigInfo::needValidatorSet | block::ConfigInfo::needWorkchainInfo |
                            block::ConfigInfo::needStateExtraRoot | block::ConfigInfo::needCapabilities |
                            block::ConfigInfo::needPrevBlocks);
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
      LOG(INFO) << "block generation capabilities " << config_->get_capabilities()
                   << " have been enabled in global configuration, but we support only " << supported_capabilities()
                   << " (upgrade validator software?)";
    }
    if (config_->get_global_version() > supported_version()) {
      LOG(INFO) << "block version " << config_->get_global_version()
                   << " have been enabled in global configuration, but we support only " << supported_version()
                   << " (upgrade validator software?)";
    }

    old_shard_conf_ = std::make_unique<block::ShardConfig>(*config_);
    new_shard_conf_ = std::make_unique<block::ShardConfig>(*config_);
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
    auto limits = config_->get_block_limits(false);
    if (limits.is_error()) {
      return fatal_error(limits.move_as_error());
    }
    block_limits_ = limits.move_as_ok();
    block_limits_->start_lt = start_lt_;
    block_limit_status_ = std::make_unique<block::BlockLimitStatus>(*block_limits_);
    if (!fetch_config_params()) {
      return false;
    }
    if (!check_this_shard_mc_info()) {
      return fatal_error("masterchain configuration does not admit creating block "s + id_.to_str());
    }
    store_out_msg_queue_size_ = config_->has_capability(ton::capStoreOutMsgQueueSize);
    msg_metadata_enabled_ = config_->has_capability(ton::capMsgMetadata);
    deferring_messages_enabled_ = config_->has_capability(ton::capDeferMessages);
  } catch (vm::VmError& err) {
    return fatal_error(-666, err.get_msg());
  } catch (vm::VmVirtError& err) {
    return fatal_error(-666, err.get_msg());
  }
  return true;
}

/**
 * Fetches and validates configuration parameters from the masterchain configuration.
 * Almost the same as in Collator.
 *
 * @returns True if all configuration parameters were successfully fetched and validated, false otherwise.
 */
bool ContestValidateQuery::fetch_config_params() {
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
  block::SizeLimitsConfig size_limits;
  {
    auto res = config_->get_size_limits_config();
    if (res.is_error()) {
      return fatal_error(res.move_as_error());
    }
    size_limits = res.move_as_ok();
  }
  {
    // compute compute_phase_cfg / storage_phase_cfg
    auto cell = config_->get_config_param(21);
    if (cell.is_null()) {
      return fatal_error("cannot fetch current gas prices and limits from masterchain configuration");
    }
    if (!compute_phase_cfg_.parse_GasLimitsPrices(std::move(cell), storage_phase_cfg_.freeze_due_limit,
                                                  storage_phase_cfg_.delete_due_limit)) {
      return fatal_error("cannot unpack current gas prices and limits from masterchain configuration");
    }
    auto mc_gas_prices = config_->get_gas_limits_prices(true);
    if (mc_gas_prices.is_error()) {
      return fatal_error(mc_gas_prices.move_as_error_prefix("cannot unpack masterchain gas prices and limits: "));
    }
    compute_phase_cfg_.mc_gas_prices = mc_gas_prices.move_as_ok();
    compute_phase_cfg_.special_gas_full = config_->get_global_version() >= 5;
    storage_phase_cfg_.enable_due_payment = config_->get_global_version() >= 4;
    storage_phase_cfg_.global_version = config_->get_global_version();
    compute_phase_cfg_.block_rand_seed = rand_seed_;
    compute_phase_cfg_.libraries = std::make_unique<vm::Dictionary>(config_->get_libraries_root(), 256);
    compute_phase_cfg_.max_vm_data_depth = size_limits.max_vm_data_depth;
    compute_phase_cfg_.global_config = config_->get_root_cell();
    compute_phase_cfg_.global_version = config_->get_global_version();
    if (compute_phase_cfg_.global_version >= 4) {
      auto prev_blocks_info = config_->get_prev_blocks_info();
      if (prev_blocks_info.is_error()) {
        return fatal_error(
            prev_blocks_info.move_as_error_prefix("cannot fetch prev blocks info from masterchain configuration: "));
      }
      compute_phase_cfg_.prev_blocks_info = prev_blocks_info.move_as_ok();
    }
    if (compute_phase_cfg_.global_version >= 6) {
      compute_phase_cfg_.unpacked_config_tuple = config_->get_unpacked_config_tuple(now_);
    }
    compute_phase_cfg_.suspended_addresses = config_->get_suspended_addresses(now_);
    compute_phase_cfg_.size_limits = size_limits;
    compute_phase_cfg_.precompiled_contracts = config_->get_precompiled_contracts_config();
    compute_phase_cfg_.allow_external_unfreeze = compute_phase_cfg_.global_version >= 8;
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
    action_phase_cfg_.size_limits = size_limits;
    action_phase_cfg_.action_fine_enabled = config_->get_global_version() >= 4;
    action_phase_cfg_.bounce_on_fail_enabled = config_->get_global_version() >= 4;
    action_phase_cfg_.message_skip_enabled = config_->get_global_version() >= 8;
    action_phase_cfg_.disable_custom_fess = config_->get_global_version() >= 8;
    action_phase_cfg_.mc_blackhole_addr = config_->get_burning_config().blackhole_addr;
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

/**
 * Checks the previous block against the block registered in the masterchain.
 * Almost the same as in Collator.
 *
 * @param listed The BlockIdExt of the top block of this shard registered in the masterchain.
 * @param prev The BlockIdExt of the previous block.
 * @param chk_chain_len Flag indicating whether to check the chain length.
 *
 * @returns True if the previous block is valid, false otherwise.
 */
bool ContestValidateQuery::check_prev_block(const BlockIdExt& listed, const BlockIdExt& prev, bool chk_chain_len) {
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

/**
 * Checks the previous block against the block registered in the masterchain.
 * Almost the same as in Collator
 *
 * @param listed The BlockIdExt of the top block of this shard registered in the masterchain.
 * @param prev The BlockIdExt of the previous block.
 *
 * @returns True if the previous block is equal to the one registered in the masterchain, false otherwise.
 */
bool ContestValidateQuery::check_prev_block_exact(const BlockIdExt& listed, const BlockIdExt& prev) {
  if (listed != prev) {
    return reject_query(PSTRING() << "cannot generate shardchain block for shard " << shard_.to_str()
                                  << " after previous block " << prev.to_str()
                                  << " because masterchain configuration expects another previous block "
                                  << listed.to_str() << " and we are immediately after a split/merge event");
  }
  return true;
}

/**
 * Checks the validity of the shard configuration of the current shard.
 * Almost the same as in Collator (main change: fatal_error -> reject_query).
 *
 * @returns True if the shard's configuration is valid, False otherwise.
 */
bool ContestValidateQuery::check_this_shard_mc_info() {
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

/**
 * Computes the previous shard state.
 *
 * @returns True if the previous state is computed successfully, false otherwise.
 */
bool ContestValidateQuery::compute_prev_state() {
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
  state_usage_tree_ = std::make_shared<vm::CellUsageTree>();
  prev_state_root_ = vm::UsageCell::create(prev_state_root_, state_usage_tree_->root_ptr());
  return true;
}

/**
 * Unpacks and merges the states of two previous blocks.
 * Used if the block is after_merge.
 * Similar to Collator::unpack_merge_last_state()
 *
 * @returns True if the unpacking and merging was successful, false otherwise.
 */
bool ContestValidateQuery::unpack_merge_prev_state() {
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

/**
 * Unpacks the state of the previous block.
 * Used if the block is not after_merge.
 * Similar to Collator::unpack_last_state()
 *
 * @returns True if the unpacking is successful, false otherwise.
 */
bool ContestValidateQuery::unpack_prev_state() {
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

/**
 * Unpacks the state of a previous block and performs necessary checks.
 * Similar to Collator::unpack_one_last_state()
 *
 * @param ss The ShardState object to unpack the state into.
 * @param blkid The BlockIdExt of the previous block.
 * @param prev_state_root The root of the state.
 *
 * @returns True if the unpacking and checks are successful, false otherwise.
 */
bool ContestValidateQuery::unpack_one_prev_state(block::ShardState& ss, BlockIdExt blkid,
                                                 Ref<vm::Cell> prev_state_root) {
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

/**
 * Splits the state of previous block.
 * Used if the block is after_split.
 * Similar to Collator::split_last_state()
 *
 * @param ss The ShardState object representing the previous state. The result is stored here.
 *
 * @returns True if the split operation is successful, false otherwise.
 */
bool ContestValidateQuery::split_prev_state(block::ShardState& ss) {
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

bool ContestValidateQuery::init_next_state() {
  ns_.id_ = id_;
  ns_.global_id_ = global_id_;
  ns_.utime_ = now_;
  ns_.lt_ = end_lt_;
  ns_.mc_blk_ref_ = mc_blkid_;
  ns_.vert_seqno_ = vert_seqno_;
  ns_.before_split_ = before_split_;
  ns_.processed_upto_ = block::MsgProcessedUptoCollection::unpack(id_.shard_full(), extra_collated_data_.proc_info);
  if (!ns_.processed_upto_) {
    return reject_query("failed top unpack processed upto");
  }
  return true;
}

/**
 * Requests the message queues of neighboring shards.
 * Almost the same as in Collator.
 *
 * @returns True if the request for neighbor message queues was successful, false otherwise.
 */
bool ContestValidateQuery::request_neighbor_queues() {
  CHECK(new_shard_conf_);
  auto neighbor_list = new_shard_conf_->get_neighbor_shard_hash_ids(shard_);
  LOG(DEBUG) << "got a preliminary list of " << neighbor_list.size() << " neighbors for " << shard_.to_str();
  for (ton::BlockId blk_id : neighbor_list) {
    if (blk_id.seqno == 0 && blk_id.shard_full() != shard_) {
      continue;
    }
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
  {
    for (block::McShardDescr& descr : neighbors_) {
      LOG(DEBUG) << "requesting outbound queue of neighbor #" << i << " : " << descr.blk_.to_str();
      ++pending;
      auto r_state = fetch_block_state(descr.blk_);
      if (r_state.is_error()) {
        return fatal_error(r_state.move_as_error());
      }
      td::actor::send_closure(actor_id(this), &ContestValidateQuery::got_neighbor_out_queue, i,
                              r_state.ok()->message_queue());
      ++i;
    }
  }
  return true;
}

/**
 * Handles the result of obtaining the outbound queue for a neighbor.
 * Almost the same as in Collator.
 *
 * @param i The index of the neighbor.
 * @param res The obtained outbound queue.
 */
void ContestValidateQuery::got_neighbor_out_queue(int i, td::Result<Ref<MessageQueue>> res) {
  --pending;
  if (res.is_error()) {
    fatal_error(res.move_as_error());
    return;
  }
  Ref<MessageQueue> outq_descr = res.move_as_ok();
  block::McShardDescr& descr = neighbors_.at(i);
  LOG(INFO) << "obtained outbound queue for neighbor #" << i << " : " << descr.shard().to_str();
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

/**
 * Registers a masterchain state.
 * Almost the same as in Collator.
 *
 * @param other_mc_state The masterchain state to register.
 *
 * @returns True if the registration is successful, false otherwise.
 */
bool ContestValidateQuery::register_mc_state(Ref<MasterchainStateQ> other_mc_state) {
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
 * Almost the same as in Collator
 *
 * @param seqno The seqno of the block.
 * @param state A reference to the auxiliary masterchain state.
 *
 * @returns True if the auxiliary masterchain state is successfully requested, false otherwise.
 */
bool ContestValidateQuery::request_aux_mc_state(BlockSeqno seqno, Ref<MasterchainStateQ>& state) {
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
  td::actor::send_closure_later(actor_id(this), &ContestValidateQuery::after_get_aux_shard_state, blkid,
                                fetch_block_state(blkid));
  state.clear();
  return true;
}

/**
 * Retrieves the auxiliary masterchain state for a given block sequence number.
 * Almost the same as in Collator.
 *
 * @param seqno The sequence number of the block.
 *
 * @returns A reference to the auxiliary masterchain state if found, otherwise an empty reference.
 */
Ref<MasterchainStateQ> ContestValidateQuery::get_aux_mc_state(BlockSeqno seqno) const {
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
 * Almost the same as in Collator.
 *
 * @param blkid The BlockIdExt of the shard state.
 * @param res The result of retrieving the shard state.
 */
void ContestValidateQuery::after_get_aux_shard_state(ton::BlockIdExt blkid, td::Result<Ref<ShardState>> res) {
  LOG(DEBUG) << "in ContestValidateQuery::after_get_aux_shard_state(" << blkid.to_str() << ")";
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

/**
 * Checks if the Unix time and logical time of the block are valid.
 *
 * @returns True if the utime and logical time pass checks, False otherwise.
 */
bool ContestValidateQuery::check_utime_lt() {
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
  if (end_lt_ - start_lt_ > block_limits_->lt_delta.hard()) {
    return reject_query(PSTRING() << "block increased logical time by " << end_lt_ - start_lt_
                                  << " which is larger than the hard limit " << block_limits_->lt_delta.hard());
  }
  return true;
}

/**
 * Reads the size of the outbound message queue from the previous state(s), or requests it if needed.
 *
 * @returns True if the request was successful, false otherwise.
 */
bool ContestValidateQuery::prepare_out_msg_queue_size() {
  if (ps_.out_msg_queue_size_) {
    // if after_split then out_msg_queue_size is always present, since it is calculated during split
    old_out_msg_queue_size_ = ps_.out_msg_queue_size_.value();
    out_msg_queue_size_known_ = true;
    have_out_msg_queue_size_in_state_ = true;
    return true;
  }
  if (ps_.out_msg_queue_->is_empty()) {
    old_out_msg_queue_size_ = 0;
    out_msg_queue_size_known_ = true;
    have_out_msg_queue_size_in_state_ = true;
    return true;
  }
  if (!store_out_msg_queue_size_) {  // Don't need it
    return true;
  }
  old_out_msg_queue_size_ = 0;
  out_msg_queue_size_known_ = true;
  return fatal_error("unknown queue sizes");
}

/**
 * Handles the result of obtaining the size of the outbound message queue.
 *
 * If the block is after merge then the two sizes are added.
 *
 * @param i The index of the previous block (0 or 1).
 * @param res The result object containing the size of the queue.
 */
void ContestValidateQuery::got_out_queue_size(size_t i, td::Result<td::uint64> res) {
  --pending;
  if (res.is_error()) {
    fatal_error(
        res.move_as_error_prefix(PSTRING() << "failed to get message queue size from prev block #" << i << ": "));
    return;
  }
  td::uint64 size = res.move_as_ok();
  LOG(DEBUG) << "got outbound queue size from prev block #" << i << ": " << size;
  old_out_msg_queue_size_ += size;
  try_validate();
}

/*
 *
 *  METHODS CALLED FROM try_validate() stage 1
 *
 */

/**
 * Adjusts one entry from the processed up to information using the masterchain state that is referenced in the entry.
 * Almost the same as in Collator (but it can take into account the new state of the masterchain).
 *
 * @param proc The MsgProcessedUpto object.
 * @param owner The shard that the MsgProcessesUpto information is taken from.
 * @param allow_cur Allow using the new state of the msaterchain.
 *
 * @returns True if the processed up to information was successfully adjusted, false otherwise.
 */
bool ContestValidateQuery::fix_one_processed_upto(block::MsgProcessedUpto& proc, ton::ShardIdFull owner,
                                                  bool allow_cur) {
  if (proc.compute_shard_end_lt) {
    return true;
  }
  auto seqno = std::min(proc.mc_seqno, mc_seqno_);
  {
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

/**
 * Adjusts the processed up to collection using the using the auxilliary masterchain states.
 * Almost the same as in Collator.
 *
 * @param upto The MsgProcessedUptoCollection to be adjusted.
 * @param allow_cur Allow using the new state of the msaterchain.
 *
 * @returns True if all entries were successfully adjusted, False otherwise.
 */
bool ContestValidateQuery::fix_processed_upto(block::MsgProcessedUptoCollection& upto, bool allow_cur) {
  for (auto& entry : upto.list) {
    if (!fix_one_processed_upto(entry, upto.owner, allow_cur)) {
      return false;
    }
  }
  return true;
}

/**
 * Adjusts the processed_upto values for all shard states, including neighbors.
 *
 * @returns True if all processed_upto values were successfully adjusted, false otherwise.
 */
bool ContestValidateQuery::fix_all_processed_upto() {
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

/**
 * Adds trivials neighbor after merging two shards.
 * Trivial neighbors are the two previous blocks.
 * Almost the same as in Collator.
 *
 * @returns True if the operation is successful, false otherwise.
 */
bool ContestValidateQuery::add_trivial_neighbor_after_merge() {
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

/**
 * Adds a trivial neighbor.
 * A trivial neighbor is the previous block.
 * Almost the same as in Collator.
 *
 * @returns True if the operation is successful, false otherwise.
 */
bool ContestValidateQuery::add_trivial_neighbor() {
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

/**
 * Unpacks block data and performs validation checks.
 *
 * @returns True if the block data is successfully unpacked and passes all validation checks, false otherwise.
 */
bool ContestValidateQuery::unpack_block_data() {
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
  if (!block::tlb::t_InMsgDescr.validate_upto(10000000, *inmsg_cs)) {
    return reject_query("InMsgDescr of the new block failed to pass handwritten validity tests");
  }
  if (!block::tlb::t_OutMsgDescr.validate_upto(10000000, *outmsg_cs)) {
    return reject_query("OutMsgDescr of the new block failed to pass handwritten validity tests");
  }
  if (!block::tlb::t_ShardAccountBlocks.validate_ref(10000000, extra.account_blocks)) {
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

/**
 * Validates and unpacks the value flow of a new block.
 *
 * @param value_flow_root The root of the value flow to be unpacked and validated.
 *
 * @returns True if the value flow is valid and unpacked successfully, false otherwise.
 */
bool ContestValidateQuery::unpack_precheck_value_flow(Ref<vm::Cell> value_flow_root) {
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
  if (!value_flow_.minted.is_zero()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " is invalid (non-zero minted value in a non-masterchain block)");
  }
  if (!value_flow_.recovered.is_zero()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " is invalid (non-zero recovered value in a non-masterchain block)");
  }
  if (!value_flow_.burned.is_zero()) {
    LOG(INFO) << "invalid value flow: " << os.str();
    return reject_query("ValueFlow of block "s + id_.to_str() +
                        " is invalid (non-zero burned value in a non-masterchain block)");
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
  create_fee = (basechain_create_fee_ >> ton::shard_prefix_length(shard_));
  if (value_flow_.created != block::CurrencyCollection{create_fee}) {
    return reject_query("ValueFlow of block "s + id_.to_str() + " declares block creation fee " +
                        value_flow_.created.to_str() + ", but the current configuration expects it to be " +
                        td::dec_string(create_fee));
  }
  if (!value_flow_.fees_imported.is_zero()) {
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
  return true;
}

/**
 * Computes the amount of extra currencies to be minted.
 * Similar to Collator::compute_minted_amount()
 *
 * @param to_mint A reference to the CurrencyCollection object to store the minted amount.
 *
 * @returns True if the computation is successful, false otherwise.
 */
bool ContestValidateQuery::compute_minted_amount(block::CurrencyCollection& to_mint) {
  return to_mint.set_zero();
}

bool ContestValidateQuery::postcheck_one_account_update(td::ConstBitPtr acc_id, Ref<vm::CellSlice> old_value,
                                                        Ref<vm::CellSlice> new_value) {
  LOG(DEBUG) << "checking update of account " << acc_id.to_hex(256);
  old_value = ps_.account_dict_->extract_value(std::move(old_value));
  new_value = ns_.account_dict_->extract_value(std::move(new_value));
  auto acc_blk_root = account_blocks_dict_->lookup(acc_id, 256);
  if (acc_blk_root.is_null()) {
    return reject_query("the state of account "s + acc_id.to_hex(256) +
                        " changed in the new state with respect to the old state, but the block contains no "
                        "AccountBlock for this account");
  }
  if (new_value.not_null()) {
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

/**
 * Post-validates all account updates between the old and new state.
 *
 * @returns True if the pre-check is successful, False otherwise.
 */
bool ContestValidateQuery::postcheck_account_updates() {
  LOG(INFO) << "pre-checking all Account updates between the old and the new state";
  try {
    CHECK(ps_.account_dict_ && ns_.account_dict_);
    if (!ps_.account_dict_->scan_diff(
            *ns_.account_dict_,
            [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val_extra,
                   Ref<vm::CellSlice> new_val_extra) {
              CHECK(key_len == 256);
              return postcheck_one_account_update(key, std::move(old_val_extra), std::move(new_val_extra));
            },
            2 /* check augmentation of changed nodes in the new dict */)) {
      return reject_query("invalid ShardAccounts dictionary in the new state");
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid ShardAccount dictionary difference between the old and the new state: "s +
                        err.get_msg());
  }
  return true;
}

/**
 * Pre-validates a single transaction (without actually running it).
 *
 * @param acc_id The 256-bit account address.
 * @param trans_lt The logical time of the transaction.
 * @param trans_csr The cell slice containing the serialized Transaction.
 * @param prev_trans_hash The hash of the previous transaction.
 * @param prev_trans_lt The logical time of the previous transaction.
 * @param prev_trans_lt_len The logical time length of the previous transaction.
 * @param acc_state_hash The hash of the account state before the transaction. Will be set to the hash of the new state.
 *
 * @returns True if the transaction passes pre-checks, false otherwise.
 */
bool ContestValidateQuery::precheck_one_transaction(td::ConstBitPtr acc_id, ton::LogicalTime trans_lt,
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
/**
 * Pre-validates an AccountBlock and all transactions in it.
 *
 * @param acc_id The 256-bit account address.
 * @param acc_blk_root The root of the AccountBlock.
 *
 * @returns True if the AccountBlock passes pre-checks, false otherwise.
 */
bool ContestValidateQuery::precheck_one_account_block(td::ConstBitPtr acc_id, Ref<vm::CellSlice> acc_blk_root) {
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
  block::tlb::ShardAccount::Record old_state;
  if (!old_state.unpack(ps_.account_dict_->lookup(acc_id, 256))) {
    return reject_query("cannot extract Account from the ShardAccount of "s + acc_id.to_hex(256));
  }
  if (hash_upd.old_hash != old_state.account->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + acc_id.to_hex(256) +
                        " has incorrect old hash");
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
    if (acc_state_hash != hash_upd.new_hash) {
      return reject_query("final state hash mismatch in (HASH_UPDATE Account) for account "s + acc_id.to_hex(256));
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid transaction dictionary in AccountBlock of "s + acc_id.to_hex(256) + " : " +
                        err.get_msg());
  }
  return true;
}

/**
 * Pre-validates all account blocks.
 *
 * @returns True if the pre-checking is successful, otherwise false.
 */
bool ContestValidateQuery::precheck_account_transactions() {
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

/**
 * Looks up a transaction in the account blocks dictionary for a given account address and logical time.
 *
 * @param addr The address of the account.
 * @param lt The logical time of the transaction.
 *
 * @returns A reference to the transaction if found, null otherwise.
 */
Ref<vm::Cell> ContestValidateQuery::lookup_transaction(const ton::StdSmcAddress& addr, ton::LogicalTime lt) const {
  CHECK(account_blocks_dict_);
  block::gen::AccountBlock::Record ab_rec;
  if (!tlb::csr_unpack_safe(account_blocks_dict_->lookup(addr), ab_rec)) {
    return {};
  }
  vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(ab_rec.transactions), 64,
                                     block::tlb::aug_AccountTransactions};
  return trans_dict.lookup_ref(td::BitArray<64>{(long long)lt});
}

/**
 * Checks that a Transaction cell refers to a transaction present in the ShardAccountBlocks.
 *
 * @param trans_ref The reference to the serialized transaction root.
 *
 * @returns True if the transaction reference is valid, False otherwise.
 */
bool ContestValidateQuery::is_valid_transaction_ref(Ref<vm::Cell> trans_ref) const {
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

bool ContestValidateQuery::build_new_message_queue() {
  ns_.out_msg_queue_ =
      std::make_unique<vm::AugmentedDictionary>(ps_.out_msg_queue_->get_root(), 352, block::tlb::aug_OutMsgQueue);
  ns_.dispatch_queue_ =
      std::make_unique<vm::AugmentedDictionary>(ps_.dispatch_queue_->get_root(), 256, block::tlb::aug_DispatchQueue);
  ns_.out_msg_queue_size_ = ps_.out_msg_queue_size_.value();

  bool ok = in_msg_dict_->check_for_each_extra([&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr, int) {
    int tag = block::gen::t_InMsg.get_tag(*value);
    switch (tag) {
      case block::gen::InMsg::msg_import_ext: {
        break;
      }
      case block::gen::InMsg::msg_import_deferred_fin: {
        block::gen::InMsg::Record_msg_import_deferred_fin rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        CHECK(block::gen::csr_unpack(value, rec));
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.in_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_import_deferred_fin");
        }

        WorkchainId wc;
        StdSmcAddress addr;
        if (!block::tlb::t_MsgAddressInt.extract_std_address(msg.src, wc, addr)) {
          return fatal_error("failed to extract src address for msg_import_deferred_fin");
        }
        if (!block::remove_dispatch_queue_entry(*ns_.dispatch_queue_, addr, msg.created_lt)) {
          return fatal_error("failed to remove dispatch queue entry for msg_import_deferred_fin");
        }
        break;
      }
      case block::gen::InMsg::msg_import_deferred_tr: {
        block::gen::InMsg::Record_msg_import_deferred_tr rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        CHECK(block::gen::csr_unpack(value, rec));
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.in_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_import_deferred_tr");
        }

        WorkchainId wc;
        StdSmcAddress addr;
        if (!block::tlb::t_MsgAddressInt.extract_std_address(msg.src, wc, addr)) {
          return fatal_error("failed to extract src address for msg_import_deferred_tr");
        }
        if (!block::remove_dispatch_queue_entry(*ns_.dispatch_queue_, addr, msg.created_lt)) {
          return fatal_error("failed to remove dispatch queue entry for msg_import_deferred_tr");
        }
        break;
      }
      case block::gen::InMsg::msg_import_ihr: {
        break;
      }
      case block::gen::InMsg::msg_import_imm: {
        break;
      }
      case block::gen::InMsg::msg_import_fin: {
        break;
      }
      case block::gen::InMsg::msg_import_tr: {
        break;
      }
      case block::gen::InMsg::msg_discard_fin: {
        break;
      }
      case block::gen::InMsg::msg_discard_tr: {
        break;
      }
    }
    return true;
  });
  if (!ok) {
    return reject_query("failed to parse in msg dict");
  }
  ok = out_msg_dict_->check_for_each_extra([&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr key, int) {
    int tag = block::gen::t_OutMsg.get_tag(*value);
    switch (tag) {
      case block::gen::OutMsg::msg_export_ext: {
        break;
      }
      case block::gen::OutMsg::msg_export_new: {
        block::gen::OutMsg::Record_msg_export_new rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        CHECK(block::gen::csr_unpack(value, rec));
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.out_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_export_new");
        }
        LogicalTime enqueued_lt = msg.created_lt;

        auto src_prefix = block::tlb::MsgAddressInt::get_prefix(msg.src);
        auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(msg.dest);
        auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);

        td::BitArray<32 + 64 + 256> queue_key;
        td::BitPtr ptr = queue_key.bits();
        ptr.store_int(next_prefix.workchain, 32);
        ptr.advance(32);
        ptr.store_uint(next_prefix.account_id_prefix, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        vm::CellBuilder cb;
        CHECK(cb.store_long_bool(enqueued_lt) && cb.store_ref_bool(rec.out_msg));
        if (!ns_.out_msg_queue_->set_builder(queue_key, cb, vm::Dictionary::SetMode::Add)) {
          return fatal_error("failed to store message to out msg queue for msg_export_new");
        }
        ++ns_.out_msg_queue_size_.value();
        break;
      }
      case block::gen::OutMsg::msg_export_imm: {
        break;
      }
      case block::gen::OutMsg::msg_export_tr: {
        block::gen::OutMsg::Record_msg_export_tr rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        CHECK(block::gen::csr_unpack(value, rec));
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.out_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_export_tr");
        }
        LogicalTime enqueued_lt = start_lt_;

        auto src_prefix = block::tlb::MsgAddressInt::get_prefix(msg.src);
        auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(msg.dest);
        auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);

        td::BitArray<32 + 64 + 256> queue_key;
        td::BitPtr ptr = queue_key.bits();
        ptr.store_int(next_prefix.workchain, 32);
        ptr.advance(32);
        ptr.store_uint(next_prefix.account_id_prefix, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        vm::CellBuilder cb;
        CHECK(cb.store_long_bool(enqueued_lt) && cb.store_ref_bool(rec.out_msg));
        if (!ns_.out_msg_queue_->set_builder(queue_key, cb, vm::Dictionary::SetMode::Add)) {
          return fatal_error("failed to store message to out msg queue for msg_export_tr");
        }
        ++ns_.out_msg_queue_size_.value();
        break;
      }
      case block::gen::OutMsg::msg_export_deq_imm: {
        block::gen::OutMsg::Record_msg_export_deq_imm rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.out_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_export_deq_imm");
        }

        auto src_prefix = block::tlb::MsgAddressInt::get_prefix(msg.src);
        auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(msg.dest);
        auto cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);

        td::BitArray<32 + 64 + 256> queue_key;
        td::BitPtr ptr = queue_key.bits();
        ptr.store_int(cur_prefix.workchain, 32);
        ptr.advance(32);
        ptr.store_uint(cur_prefix.account_id_prefix, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        if (ns_.out_msg_queue_->lookup_delete(queue_key).is_null()) {
          return fatal_error("failed to delete message from out msg queue for msg_export_deq_imm");
        }
        --ns_.out_msg_queue_size_.value();
        break;
      }
      case block::gen::OutMsg::msg_export_new_defer: {
        block::gen::OutMsg::Record_msg_export_new_defer rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        CHECK(block::gen::csr_unpack(value, rec));
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.out_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_export_new");
        }
        LogicalTime lt = msg.created_lt;
        WorkchainId wc;
        StdSmcAddress addr;
        if (!block::tlb::t_MsgAddressInt.extract_std_address(msg.src, wc, addr)) {
          return fatal_error("failed to extract src address for msg_export_new_defer");
        }

        vm::Dictionary dispatch_dict{64};
        td::uint64 dispatch_dict_size;
        if (!block::unpack_account_dispatch_queue(ns_.dispatch_queue_->lookup(addr), dispatch_dict,
                                                  dispatch_dict_size)) {
          return fatal_error(PSTRING() << "cannot unpack AccountDispatchQueue for account " << addr.to_hex());
        }
        td::BitArray<64> key;
        key.store_ulong(lt);
        vm::CellBuilder cb;
        CHECK(cb.store_long_bool(lt) && cb.store_ref_bool(rec.out_msg));
        if (!dispatch_dict.set_builder(key, cb, vm::Dictionary::SetMode::Add)) {
          return fatal_error(PSTRING() << "cannot add message to AccountDispatchQueue for account " << addr.to_hex()
                                       << ", lt=" << lt);
        }
        ++dispatch_dict_size;
        ns_.dispatch_queue_->set(addr, block::pack_account_dispatch_queue(dispatch_dict, dispatch_dict_size));
        break;
      }
      case block::gen::OutMsg::msg_export_deferred_tr: {
        block::gen::OutMsg::Record_msg_export_deferred_tr rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.out_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_export_deferred_tr");
        }
        if (!env.emitted_lt) {
          return fatal_error("no emitted_lt in msg_export_deferred_tr");
        }
        LogicalTime enqueued_lt = env.emitted_lt.value();

        auto src_prefix = block::tlb::MsgAddressInt::get_prefix(msg.src);
        auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(msg.dest);
        auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);

        td::BitArray<32 + 64 + 256> queue_key;
        td::BitPtr ptr = queue_key.bits();
        ptr.store_int(next_prefix.workchain, 32);
        ptr.advance(32);
        ptr.store_uint(next_prefix.account_id_prefix, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        vm::CellBuilder cb;
        CHECK(cb.store_long_bool(enqueued_lt) && cb.store_ref_bool(rec.out_msg));
        if (!ns_.out_msg_queue_->set_builder(queue_key, cb, vm::Dictionary::SetMode::Add)) {
          return fatal_error("failed to store message to out msg queue for msg_export_deferred_tr");
        }
        ++ns_.out_msg_queue_size_.value();
        break;
      }
      case block::gen::OutMsg::msg_export_deq: {
        return fatal_error("msg_export_deq are deprecated");
      }
      case block::gen::OutMsg::msg_export_deq_short: {
        block::gen::OutMsg::Record_msg_export_deq_short rec;
        CHECK(block::gen::csr_unpack(value, rec));
        td::BitArray<32 + 64 + 256> queue_key;
        td::BitPtr ptr = queue_key.bits();
        ptr.store_int(rec.next_workchain, 32);
        ptr.advance(32);
        ptr.store_uint(rec.next_addr_pfx, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        if (ns_.out_msg_queue_->lookup_delete(queue_key).is_null()) {
          return fatal_error("cannot delete from out msg queue");
        }
        --ns_.out_msg_queue_size_.value();
        break;
      }
      case block::gen::OutMsg::msg_export_tr_req: {
        block::gen::OutMsg::Record_msg_export_tr_req rec;
        block::tlb::MsgEnvelope::Record_std env;
        block::gen::CommonMsgInfo::Record_int_msg_info msg;
        if (!block::gen::csr_unpack(value, rec) || !block::tlb::unpack_cell(rec.out_msg, env) ||
            !block::gen::csr_unpack_inexact(vm::load_cell_slice_ref(env.msg), msg)) {
          return fatal_error("cannot unpack msg_export_tr_rec");
        }
        LogicalTime enqueued_lt = start_lt_;

        auto src_prefix = block::tlb::MsgAddressInt::get_prefix(msg.src);
        auto dest_prefix = block::tlb::MsgAddressInt::get_prefix(msg.dest);
        auto cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
        auto next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);

        td::BitArray<32 + 64 + 256> queue_key;

        td::BitPtr ptr = queue_key.bits();
        ptr.store_int(cur_prefix.workchain, 32);
        ptr.advance(32);
        ptr.store_uint(cur_prefix.account_id_prefix, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        if (ns_.out_msg_queue_->lookup_delete(queue_key).is_null()) {
          return fatal_error("failed to delete requeued message from out msg queue");
        }

        ptr.store_int(next_prefix.workchain, 32);
        ptr.advance(32);
        ptr.store_uint(next_prefix.account_id_prefix, 64);
        ptr.advance(64);
        ptr.copy_from(key, 256);
        vm::CellBuilder cb;
        CHECK(cb.store_long_bool(enqueued_lt) && cb.store_ref_bool(rec.out_msg));
        if (!ns_.out_msg_queue_->set_builder(queue_key, cb, vm::Dictionary::SetMode::Add)) {
          return fatal_error("failed to store message to out msg queue for msg_export_tr_req");
        }
        break;
      }
    }
    return true;
  });
  if (!ok) {
    return reject_query("failed to parse out msg dict");
  }
  return true;
}

/**
 * Checks that any change in OutMsgQueue in the state is accompanied by an OutMsgDescr record in the block.
 * Also checks that the keys are correct.
 *
 * @param out_msg_id The 32+64+256-bit ID of the outbound message.
 * @param old_value The old value of the message queue entry.
 * @param new_value The new value of the message queue entry.
 *
 * @returns True if the update is valid, false otherwise.
 */
bool ContestValidateQuery::precheck_one_message_queue_update(td::ConstBitPtr out_msg_id, Ref<vm::CellSlice> old_value,
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
      return reject_query(PSTRING() << "old EnqueuedMsg with key "s + out_msg_id.to_hex(352) + " has enqueued_lt="
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
  int tag = block::tlb::t_OutMsg.get_tag(*out_msg_cs);
  if (tag == 12 || tag == 13) {
    tag /= 2;
  } else if (tag == 20) {
    tag = 8;
  } else if (tag == 21) {
    tag = 9;
  }
  // mode for msg_export_{ext,new,imm,tr,deq_imm,???,deq/deq_short,tr_req,new_defer,deferred_tr}
  static const int tag_mode[10] = {0, 2, 0, 2, 1, 0, 1, 3, 0, 2};
  static const char* tag_str[10] = {"ext", "new", "imm",    "tr",        "deq_imm",
                                    "???", "deq", "tr_req", "new_defer", "deferred_tr"};
  if (tag < 0 || tag >= 10 || !(tag_mode[tag] & mode)) {
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

/**
 * Performs a pre-check on the difference between the old and new outbound message queues.
 *
 * @returns True if the pre-check is successful, false otherwise.
 */
bool ContestValidateQuery::precheck_message_queue_update() {
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
            2 /* check augmentation of changed nodes in the new dict */)) {
      return reject_query("invalid OutMsgQueue dictionary in the new state");
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid OutMsgQueue dictionary difference between the old and the new state: "s +
                        err.get_msg());
  }
  if (store_out_msg_queue_size_) {
  } else {
    if (ns_.out_msg_queue_size_) {
      return reject_query("outbound message queue size in the new state is present, but shouldn't");
    }
  }
  return true;
}

/**
 * Performs a check on the difference between the old and new dispatch queues for one account.
 *
 * @param addr The 256-bit address of the account.
 * @param old_queue_csr The old value of the account dispatch queue.
 * @param new_queue_csr The new value of the account dispatch queue.
 *
 * @returns True if the check is successful, false otherwise.
 */
bool ContestValidateQuery::check_account_dispatch_queue_update(td::Bits256 addr, Ref<vm::CellSlice> old_queue_csr,
                                                               Ref<vm::CellSlice> new_queue_csr) {
  vm::Dictionary old_dict{64};
  td::uint64 old_dict_size = 0;
  if (!block::unpack_account_dispatch_queue(old_queue_csr, old_dict, old_dict_size)) {
    return reject_query(PSTRING() << "invalid AccountDispatchQueue for " << addr.to_hex() << " in the old state");
  }
  vm::Dictionary new_dict{64};
  td::uint64 new_dict_size = 0;
  if (!block::unpack_account_dispatch_queue(new_queue_csr, new_dict, new_dict_size)) {
    return reject_query(PSTRING() << "invalid AccountDispatchQueue for " << addr.to_hex() << " in the new state");
  }
  td::uint64 expected_dict_size = old_dict_size;
  LogicalTime max_removed_lt = 0;
  LogicalTime min_added_lt = (LogicalTime)-1;
  bool res = old_dict.scan_diff(
      new_dict, [&](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val, Ref<vm::CellSlice> new_val) {
        CHECK(key_len == 64);
        CHECK(old_val.not_null() || new_val.not_null());
        if (old_val.not_null() && new_val.not_null()) {
          return false;
        }
        td::uint64 lt = key.get_uint(64);
        block::gen::EnqueuedMsg::Record rec;
        if (old_val.not_null()) {
          LOG(DEBUG) << "removed message from DispatchQueue: account=" << addr.to_hex() << ", lt=" << lt;
          --expected_dict_size;
          if (!block::tlb::csr_unpack(old_val, rec)) {
            return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex());
          }
        } else {
          LOG(DEBUG) << "added message to DispatchQueue: account=" << addr.to_hex() << ", lt=" << lt;
          ++expected_dict_size;
          if (!block::tlb::csr_unpack(new_val, rec)) {
            return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex());
          }
        }
        if (lt != rec.enqueued_lt) {
          return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex()
                                        << ": lt mismatch (" << lt << " != " << rec.enqueued_lt << ")");
        }
        block::tlb::MsgEnvelope::Record_std env;
        if (!block::gen::t_MsgEnvelope.validate_ref(rec.out_msg) || !block::tlb::unpack_cell(rec.out_msg, env)) {
          return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex());
        }
        if (env.emitted_lt) {
          return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex()
                                        << ", lt=" << lt << ": unexpected emitted_lt");
        }
        unsigned long long created_lt;
        vm::CellSlice msg_cs = vm::load_cell_slice(env.msg);
        if (!block::tlb::t_Message.get_created_lt(msg_cs, created_lt)) {
          return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex()
                                        << ": cannot get created_lt");
        }
        if (lt != created_lt) {
          return reject_query(PSTRING() << "invalid EnqueuedMsg in AccountDispatchQueue for " << addr.to_hex()
                                        << ": lt mismatch (" << lt << " != " << created_lt << ")");
        }
        if (old_val.not_null()) {
          removed_dispatch_queue_messages_[{addr, lt}] = rec.out_msg;
          max_removed_lt = std::max(max_removed_lt, lt);
        } else {
          new_dispatch_queue_messages_[{addr, lt}] = rec.out_msg;
          min_added_lt = std::min(min_added_lt, lt);
        }
        return true;
      });
  if (!res) {
    return reject_query(PSTRING() << "invalid AccountDispatchQueue diff for account " << addr.to_hex());
  }
  if (expected_dict_size != new_dict_size) {
    return reject_query(PSTRING() << "invalid count in AccountDispatchQuery for " << addr.to_hex()
                                  << ": expected=" << expected_dict_size << ", found=" << new_dict_size);
  }
  if (!new_dict.is_empty()) {
    td::BitArray<64> new_min_lt;
    CHECK(new_dict.get_minmax_key(new_min_lt).not_null());
    if (new_min_lt.to_ulong() <= max_removed_lt) {
      return reject_query(PSTRING() << "invalid AccountDispatchQuery update for " << addr.to_hex()
                                    << ": max removed lt is " << max_removed_lt << ", but lt=" << new_min_lt.to_ulong()
                                    << " is still in queue");
    }
  }
  if (!old_dict.is_empty()) {
    td::BitArray<64> old_max_lt;
    CHECK(old_dict.get_minmax_key(old_max_lt, true).not_null());
    if (old_max_lt.to_ulong() >= min_added_lt) {
      return reject_query(PSTRING() << "invalid AccountDispatchQuery update for " << addr.to_hex()
                                    << ": min added lt is " << min_added_lt << ", but lt=" << old_max_lt.to_ulong()
                                    << " was present in the queue");
    }
    if (max_removed_lt != old_max_lt.to_ulong()) {
      // Some old messages are still in DispatchQueue, meaning that all new messages from this account must be deferred
      account_expected_defer_all_messages_.insert(addr);
    }
  }
  if (old_dict_size > 0 && max_removed_lt != 0) {
    ++processed_account_dispatch_queues_;
  }
  return true;
}

/**
 * Pre-check the difference between the old and new dispatch queues and put the difference to
 * new_dispatch_queue_messages_, old_dispatch_queue_messages_
 *
 * @returns True if the pre-check and unpack is successful, false otherwise.
 */
bool ContestValidateQuery::unpack_dispatch_queue_update() {
  LOG(INFO) << "checking the difference between the old and the new dispatch queues";
  try {
    CHECK(ps_.dispatch_queue_ && ns_.dispatch_queue_);
    CHECK(out_msg_dict_);
    bool res = ps_.dispatch_queue_->scan_diff(
        *ns_.dispatch_queue_,
        [this](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val_extra, Ref<vm::CellSlice> new_val_extra) {
          CHECK(key_len == 256);
          return check_account_dispatch_queue_update(key, ps_.dispatch_queue_->extract_value(std::move(old_val_extra)),
                                                     ns_.dispatch_queue_->extract_value(std::move(new_val_extra)));
        },
        2 /* check augmentation of changed nodes in the new dict */);
    if (!res) {
      return reject_query("invalid DispatchQueue dictionary in the new state");
    }

    if (have_out_msg_queue_size_in_state_ &&
        old_out_msg_queue_size_ <= compute_phase_cfg_.size_limits.defer_out_queue_size_limit) {
      // Check that at least one message was taken from each AccountDispatchQueue
      try {
        have_unprocessed_account_dispatch_queue_ = false;
        td::uint64 total_account_dispatch_queues = 0;
        ps_.dispatch_queue_->check_for_each([&](Ref<vm::CellSlice>, td::ConstBitPtr, int n) -> bool {
          ++total_account_dispatch_queues;
          if (total_account_dispatch_queues > processed_account_dispatch_queues_) {
            return false;
          }
          return true;
        });
        have_unprocessed_account_dispatch_queue_ =
            (total_account_dispatch_queues != processed_account_dispatch_queues_);
      } catch (vm::VmVirtError&) {
        // VmVirtError can happen if we have only a proof of ShardState
        have_unprocessed_account_dispatch_queue_ = true;
      }
    }
  } catch (vm::VmError& err) {
    return reject_query("invalid DispatchQueue dictionary difference between the old and the new state: "s +
                        err.get_msg());
  }
  return true;
}

/**
 * Updates the maximum processed logical time and hash value.
 *
 * @param lt The logical time to compare against the current maximum processed logical time.
 * @param hash The hash value to compare against the current maximum processed hash value.
 *
 * @returns True if the update was successful, false otherwise.
 */
bool ContestValidateQuery::update_max_processed_lt_hash(ton::LogicalTime lt, const ton::Bits256& hash) {
  if (proc_lt_ < lt || (proc_lt_ == lt && proc_hash_ < hash)) {
    proc_lt_ = lt;
    proc_hash_ = hash;
  }
  return true;
}

/**
 * Updates the minimum enqueued logical time and hash values.
 *
 * @param lt The logical time to compare.
 * @param hash The hash value to compare.
 *
 * @returns True if the update was successful, false otherwise.
 */
bool ContestValidateQuery::update_min_enqueued_lt_hash(ton::LogicalTime lt, const ton::Bits256& hash) {
  if (lt < min_enq_lt_ || (lt == min_enq_lt_ && hash < min_enq_hash_)) {
    min_enq_lt_ = lt;
    min_enq_hash_ = hash;
  }
  return true;
}

/**
 * Checks that the MsgEnvelope was present in the output queue of a neighbor, and that it has not been processed before.
 *
 * @param msg_env The message envelope of the imported message.
 *
 * @returns True if the imported internal message passes checks, false otherwise.
 */
bool ContestValidateQuery::check_imported_message(Ref<vm::Cell> msg_env) {
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

/**
 * Checks if the given input message is a special message.
 * A message is considered special if it recovers fees or mints extra currencies.
 *
 * @param in_msg The input message to be checked.
 *
 * @returns True if the input message is special, False otherwise.
 */
bool ContestValidateQuery::is_special_in_msg(const vm::CellSlice& in_msg) const {
  return (recover_create_msg_.not_null() && vm::load_cell_slice(recover_create_msg_).contents_equal(in_msg)) ||
         (mint_msg_.not_null() && vm::load_cell_slice(mint_msg_).contents_equal(in_msg));
}

/**
 * Checks the validity of an inbound message listed in InMsgDescr.
 *
 * @param key The 256-bit key of the inbound message.
 * @param in_msg The inbound message to be checked serialized using InMsg TLB-scheme.
 *
 * @returns True if the inbound message is valid, false otherwise.
 */
bool ContestValidateQuery::check_in_msg(td::ConstBitPtr key, Ref<vm::CellSlice> in_msg) {
  LOG(DEBUG) << "checking InMsg with key " << key.to_hex(256);
  CHECK(in_msg.not_null());
  int tag = block::gen::t_InMsg.get_tag(*in_msg);
  CHECK(tag >= 0);  // NB: the block has been already checked to be valid TL-B in try_validate()
  ton::StdSmcAddress src_addr, dest_addr;
  ton::WorkchainId src_wc, dest_wc;
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
  bool from_dispatch_queue = false;
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
      if (!block::tlb::t_MsgAddressInt.extract_std_address(dest, dest_wc, dest_addr)) {
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
            block::tlb::t_MsgEnvelope.get_emitted_lt(vm::load_cell_slice(inp.in_msg), created_lt) &&
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
    case block::gen::InMsg::msg_import_deferred_fin: {
      from_dispatch_queue = true;
      // msg_import_deferredfin$00100 in_msg:^MsgEnvelope transaction:^Transaction fwd_fee:Grams
      // importing and processing an internal message from DispatchQueue with destination in this shard
      block::gen::InMsg::Record_msg_import_deferred_fin inp;
      CHECK(tlb::csr_unpack(in_msg, inp) && tlb::unpack_cell(inp.in_msg, env) &&
            (fwd_fee = block::tlb::t_Grams.as_integer(std::move(inp.fwd_fee))).not_null());
      transaction = std::move(inp.transaction);
      msg_env = std::move(inp.in_msg);
      msg = env.msg;
      // ...
      break;
    }
    case block::gen::InMsg::msg_import_deferred_tr: {
      from_dispatch_queue = true;
      // msg_import_deferred_tr$00101 in_msg:^MsgEnvelope out_msg:^MsgEnvelope
      // importing and enqueueing internal message from DispatchQueue
      block::gen::InMsg::Record_msg_import_deferred_tr inp;
      CHECK(tlb::csr_unpack(in_msg, inp) && tlb::unpack_cell(inp.in_msg, env));
      fwd_fee = td::zero_refint();
      msg_env = std::move(inp.in_msg);
      msg = env.msg;
      tr_msg_env = std::move(inp.out_msg);
      // ...
      break;
    }
    default:
      return reject_query(PSTRING() << "InMsg with key " << key.to_hex(256) << " has impossible tag " << tag);
  }
  if (have_unprocessed_account_dispatch_queue_ && tag != block::gen::InMsg::msg_import_ext &&
      tag != block::gen::InMsg::msg_import_deferred_tr && tag != block::gen::InMsg::msg_import_deferred_fin) {
    // Collator is requeired to take at least one message from each AccountDispatchQueue
    // (unless the block is full or unless out_msg_queue_size is big)
    // If some AccountDispatchQueue is unporcessed then it's not allowed to import other messages except for externals
    return reject_query("required DispatchQueue processing is not done, but some other internal messages are imported");
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
    // next hop may coincide with current address only if destination is already reached (or it is deferred message)
    if (!from_dispatch_queue && next_prefix == cur_prefix && cur_prefix != dest_prefix) {
      return reject_query(
          "next hop address "s + next_prefix.to_str() + "... of inbound internal message with hash " + key.to_hex(256) +
          " coincides with its current address, but this message has not reached its final destination " +
          dest_prefix.to_str() + "... yet");
    }
    if (from_dispatch_queue && next_prefix != cur_prefix) {
      return reject_query("next hop address "s + next_prefix.to_str() + "... of deferred internal message with hash " +
                          key.to_hex(256) + " must coincide with its current prefix "s + cur_prefix.to_str() + "..."s);
    }
    // if a message is processed by a transaction, it must have destination inside the current shard
    if (transaction.not_null() && !ton::shard_contains(shard_, dest_prefix)) {
      return reject_query("inbound internal message with hash "s + key.to_hex(256) + " has destination address " +
                          dest_prefix.to_str() + "... not in this shard, but it is processed nonetheless");
    }
    // if a message is not processed by a transaction, its final destination must be outside this shard,
    // or it is a deferred message (dispatch queue -> out msg queue)
    if (tag != block::gen::InMsg::msg_import_deferred_tr && transaction.is_null() &&
        ton::shard_contains(shard_, dest_prefix)) {
      return reject_query("inbound internal message with hash "s + key.to_hex(256) + " has destination address " +
                          dest_prefix.to_str() + "... in this shard, but it is not processed by a transaction");
    }
    src = std::move(info.src);
    dest = std::move(info.dest);
    // unpack complete destination address if it is inside this shard
    if (transaction.not_null() && !block::tlb::t_MsgAddressInt.extract_std_address(dest, dest_wc, dest_addr)) {
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
    // Unpacr src address
    if (!block::tlb::t_MsgAddressInt.extract_std_address(src, src_wc, src_addr)) {
      return reject_query("cannot unpack source address of inbound external message with hash "s + key.to_hex(256));
    }
  }

  if (from_dispatch_queue) {
    // Check that the message was removed from DispatchQueue
    LogicalTime lt = info.created_lt;
    auto it = removed_dispatch_queue_messages_.find({src_addr, lt});
    if (it == removed_dispatch_queue_messages_.end()) {
      return reject_query(PSTRING() << "deferred InMsg with src_addr=" << src_addr.to_hex() << ", lt=" << lt
                                    << " was not removed from the dispatch queue");
    }
    // InMsg msg_import_deferred_* has emitted_lt in MessageEnv, but this emitted_lt is not present in DispatchQueue
    Ref<vm::Cell> dispatched_msg_env = it->second;
    td::Ref<vm::Cell> expected_msg_env;
    if (!env.emitted_lt) {
      return reject_query(PSTRING() << "no dispatch_lt in deferred InMsg with src_addr=" << src_addr.to_hex()
                                    << ", lt=" << lt);
    }
    auto emitted_lt = env.emitted_lt.value();
    if (emitted_lt < start_lt_ || emitted_lt > end_lt_) {
      return reject_query(PSTRING() << "dispatch_lt in deferred InMsg with src_addr=" << src_addr.to_hex()
                                    << ", lt=" << lt << " is not between start and end of the block");
    }
    auto env2 = env;
    env2.emitted_lt = {};
    CHECK(block::tlb::pack_cell(expected_msg_env, env2));
    if (dispatched_msg_env->get_hash() != expected_msg_env->get_hash()) {
      return reject_query(PSTRING() << "deferred InMsg with src_addr=" << src_addr.to_hex() << ", lt=" << lt
                                    << " msg envelope hasg mismatch: " << dispatched_msg_env->get_hash().to_hex()
                                    << " in DispatchQueue, " << expected_msg_env->get_hash().to_hex() << " expected");
    }
    removed_dispatch_queue_messages_.erase(it);
    if (tag == block::gen::InMsg::msg_import_deferred_fin) {
      msg_emitted_lt_.emplace_back(src_addr, lt, env.emitted_lt.value());
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
    if (dest_addr != trans_addr) {
      block::gen::t_InMsg.print(std::cerr, *in_msg);
      return reject_query(PSTRING() << "InMsg corresponding to inbound message with hash " << key.to_hex(256)
                                    << " and destination address " << dest_addr.to_hex()
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
      // msg_import_deferred_fin$00100 in_msg:^MsgEnvelope transaction:^Transaction fwd_fee:Grams
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
    case block::gen::InMsg::msg_import_deferred_fin: {
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
    case block::gen::InMsg::msg_import_deferred_tr:
    case block::gen::InMsg::msg_import_tr: {
      // msg_import_tr$101 in_msg:^MsgEnvelope out_msg:^MsgEnvelope transit_fee:Grams
      // msg_import_deferred_tr$00101 in_msg:^MsgEnvelope out_msg:^MsgEnvelope
      // importing and relaying a (transit) internal message with destination outside this shard
      if (cur_prefix == dest_prefix && tag == block::gen::InMsg::msg_import_tr) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_tr$101 (a transit message), but its current address " +
                            cur_prefix.to_str() + " is already equal to its final destination");
      }
      if (cur_prefix != next_prefix && tag == block::gen::InMsg::msg_import_deferred_tr) {
        return reject_query("internal message from DispatchQueue with hash "s + key.to_hex(256) +
                            " is a msg_import_deferred_tr$00101, but its current address " + cur_prefix.to_str() +
                            " is not equal to next address");
      }
      CHECK(transaction.is_null());
      auto out_msg_cs = out_msg_dict_->lookup(key, 256);
      if (out_msg_cs.is_null()) {
        return reject_query("inbound internal message with hash "s + key.to_hex(256) +
                            " is a msg_import_tr$101 (transit message), but the corresponding OutMsg does not exist");
      }
      if (shard_contains(shard_, cur_prefix) && tag == block::gen::InMsg::msg_import_tr) {
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
      } else if (tag == block::gen::InMsg::msg_import_tr) {
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
      } else {
        block::gen::OutMsg::Record_msg_export_deferred_tr out_msg;
        if (!tlb::csr_unpack_safe(out_msg_cs, out_msg)) {
          return reject_query(
              "inbound internal message with hash "s + key.to_hex(256) +
              " is a msg_import_deferred_tr$00101 with current address " + cur_prefix.to_str() +
              "... outside of our shard, but the corresponding OutMsg is not a valid msg_export_deferred_tr$10101");
        }
        out_msg_env = std::move(out_msg.out_msg);
        reimport = std::move(out_msg.imported);
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
      if (tr_env.metadata != env.metadata) {
        return reject_query(
            PSTRING() << "InMsg for transit message with hash " << key.to_hex(256) << " contains invalid MsgMetadata: "
                      << (env.metadata ? env.metadata.value().to_str() : "<none>") << " in in_msg, but "
                      << (tr_env.metadata ? tr_env.metadata.value().to_str() : "<none>") << " in out_msg");
      }
      if (tr_env.emitted_lt != env.emitted_lt) {
        return reject_query(
            PSTRING() << "InMsg for transit message with hash " << key.to_hex(256) << " contains invalid emitted_lt: "
                      << (env.emitted_lt ? td::to_string(env.emitted_lt.value()) : "<none>") << " in in_msg, but "
                      << (tr_env.emitted_lt ? td::to_string(tr_env.emitted_lt.value()) : "<none>") << " in out_msg");
      }
      if (tr_msg_env->get_hash() != out_msg_env->get_hash()) {
        return reject_query(
            "InMsg for transit message with hash "s + key.to_hex(256) +
            " contains rewritten MsgEnvelope different from that stored in corresponding OutMsgDescr (" +
            (tr_req ? "requeued" : "usual") + "transit)");
      }
      // check the amount of the transit fee
      td::RefInt256 transit_fee =
          from_dispatch_queue ? td::zero_refint() : action_phase_cfg_.fwd_std.get_next_part(env.fwd_fee_remaining);
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
    if (tag != block::gen::InMsg::msg_import_tr && tag != block::gen::InMsg::msg_import_deferred_tr &&
        out_msg_env->get_hash() != msg_env->get_hash()) {
      return reject_query(
          "InMsg with hash "s + key.to_hex(256) +
          " is a reimport record, but the corresponding OutMsg exports a MsgEnvelope with a different hash");
    }
  }
  return true;
}

/**
 * Checks the validity of the inbound messages listed in the InMsgDescr dictionary.
 *
 * @returns True if the inbound messages dictionary is valid, false otherwise.
 */
bool ContestValidateQuery::check_in_msg_descr() {
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

/**
 * Checks the validity of an outbound message listed in OutMsgDescr.
 *
 * @param key The 256-bit key of the outbound message.
 * @param in_msg The outbound message to be checked serialized using OutMsg TLB-scheme.
 *
 * @returns True if the outbound message is valid, false otherwise.
 */
bool ContestValidateQuery::check_out_msg(td::ConstBitPtr key, Ref<vm::CellSlice> out_msg) {
  LOG(DEBUG) << "checking OutMsg with key " << key.to_hex(256);
  CHECK(out_msg.not_null());
  int tag = block::gen::t_OutMsg.get_tag(*out_msg);
  CHECK(tag >= 0);  // NB: the block has been already checked to be valid TL-B in try_validate()
  ton::StdSmcAddress src_addr;
  ton::WorkchainId src_wc;
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
      if (!block::tlb::t_MsgAddressInt.extract_std_address(src, src_wc, src_addr)) {
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
            block::tlb::t_MsgEnvelope.get_emitted_lt(vm::load_cell_slice(out.out_msg), created_lt));
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
    case block::gen::OutMsg::msg_export_new_defer: {
      block::gen::OutMsg::Record_msg_export_new_defer out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env) &&
            block::tlb::t_MsgEnvelope.get_emitted_lt(vm::load_cell_slice(out.out_msg), created_lt));
      transaction = std::move(out.transaction);
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      // ...
      break;
    }
    case block::gen::OutMsg::msg_export_deferred_tr: {
      block::gen::OutMsg::Record_msg_export_deferred_tr out;
      CHECK(tlb::csr_unpack(out_msg, out) && tlb::unpack_cell(out.out_msg, env));
      msg_env = std::move(out.out_msg);
      msg = env.msg;
      reimport = std::move(out.imported);
      in_tag = block::gen::InMsg::msg_import_deferred_tr;
      mode = 2;  // added to OutMsgQueue
      if (!env.emitted_lt) {
        return reject_query(PSTRING() << "msg_export_deferred_tr for OutMsg with key " << key.to_hex(256)
                                      << " does not have emitted_lt in MsgEnvelope");
      }
      if (env.emitted_lt.value() < start_lt_ || env.emitted_lt.value() > end_lt_) {
        return reject_query(PSTRING() << "emitted_lt for msg_export_deferred_tr with key " << key.to_hex(256)
                                      << " is not between start and end lt of the block");
      }
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
    if (tag == block::gen::OutMsg::msg_export_new_defer) {
      if (env.cur_addr != 0 || env.next_addr != 0) {
        return reject_query("cur_addr and next_addr of the message in DispatchQueue must be zero");
      }
    } else {
      cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.cur_addr);
      next_prefix = block::interpolate_addr(src_prefix, dest_prefix, env.next_addr);
      if (!(cur_prefix.is_valid() && next_prefix.is_valid())) {
        return reject_query("cannot compute current and next hop addresses of outbound internal message with hash "s +
                            key.to_hex(256));
      }
      // check that next hop is nearer to the destination than the current address
      if (count_matching_bits(dest_prefix, next_prefix) < count_matching_bits(dest_prefix, cur_prefix)) {
        return reject_query("next hop address "s + next_prefix.to_str() +
                            "... of outbound internal message with hash " + key.to_hex(256) +
                            " is further from its destination " + dest_prefix.to_str() +
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
    if (!block::tlb::t_MsgAddressInt.extract_std_address(src, src_wc, src_addr)) {
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
    if (src_addr != trans_addr) {
      block::gen::t_OutMsg.print(std::cerr, *out_msg);
      return reject_query(PSTRING() << "OutMsg corresponding to outbound message with hash " << key.to_hex(256)
                                    << " and source address " << src_addr.to_hex()
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

  if (tag == block::gen::OutMsg::msg_export_new_defer) {
    // check the DispatchQueue update
    if (old_q_entry.not_null() || q_entry.not_null()) {
      return reject_query("OutMsg with key (message hash) "s + key.to_hex(256) +
                          " shouldn't exist in the old and the new message queues");
    }
    auto it = new_dispatch_queue_messages_.find({src_addr, created_lt});
    if (it == new_dispatch_queue_messages_.end()) {
      return reject_query(PSTRING() << "new deferred OutMsg with src_addr=" << src_addr.to_hex()
                                    << ", lt=" << created_lt << " was not added to the dispatch queue");
    }
    Ref<vm::Cell> expected_msg_env = it->second;
    if (expected_msg_env->get_hash() != msg_env->get_hash()) {
      return reject_query(PSTRING() << "new deferred OutMsg with src_addr=" << src_addr.to_hex() << ", lt="
                                    << created_lt << " msg envelope hasg mismatch: " << msg_env->get_hash().to_hex()
                                    << " in OutMsg, " << expected_msg_env->get_hash().to_hex() << " in DispatchQueue");
    }
    new_dispatch_queue_messages_.erase(it);
  } else {
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
                          " refers to a (re)import InMsg, which is not one of msg_import_imm, msg_import_fin, "
                          "msg_import_tr or msg_import_deferred_tr as expected");
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
    case block::gen::OutMsg::msg_export_new_defer: {
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
    case block::gen::OutMsg::msg_export_deferred_tr: {
      block::gen::InMsg::Record_msg_import_deferred_tr in;
      block::tlb::MsgEnvelope::Record_std in_env;
      if (!(tlb::unpack_cell(reimport, in) && tlb::unpack_cell(in.in_msg, in_env))) {
        return reject_query(
            "cannot unpack msg_import_deferred_tr InMsg record corresponding to msg_export_deferred_tr OutMsg record with key "s +
            key.to_hex(256));
      }
      CHECK(in_env.msg->get_hash() == msg->get_hash());
      auto in_cur_prefix = block::interpolate_addr(src_prefix, dest_prefix, in_env.cur_addr);
      if (!shard_contains(shard_, in_cur_prefix)) {
        return reject_query(
            "msg_export_deferred_tr OutMsg record with key "s + key.to_hex(256) +
            " corresponds to msg_import_deferred_tr InMsg record with current imported message address " +
            in_cur_prefix.to_str() + " NOT inside the current shard");
      }
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
        LOG(INFO) << "msg_export_deq OutMsg entry with key " << key.to_hex(256)
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

  if (tag == block::gen::OutMsg::msg_export_imm || tag == block::gen::OutMsg::msg_export_deq_imm ||
      tag == block::gen::OutMsg::msg_export_new || tag == block::gen::OutMsg::msg_export_deferred_tr) {
    if (src_wc != workchain()) {
      return true;
    }
    if (tag == block::gen::OutMsg::msg_export_imm && is_special_in_msg(vm::load_cell_slice(reimport))) {
      return true;
    }
    unsigned long long created_lt;
    auto cs = vm::load_cell_slice(env.msg);
    if (!block::tlb::t_Message.get_created_lt(cs, created_lt)) {
      return reject_query(PSTRING() << "cannot get created_lt for OutMsg with key " << key.to_hex(256)
                                    << ", tag=" << tag);
    }
    auto emitted_lt = env.emitted_lt ? env.emitted_lt.value() : created_lt;
    msg_emitted_lt_.emplace_back(src_addr, created_lt, emitted_lt);
  }

  return true;
}

/**
 * Checks the validity of the outbound messages listed in the OutMsgDescr dictionary.
 *
 * @returns True if the outbound messages dictionary is valid, false otherwise.
 */
bool ContestValidateQuery::check_out_msg_descr() {
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

/**
 * Checks if the processed up to information is valid and consistent.
 * Compare to Collator::update_processed_upto()
 *
 * @returns True if the processed up to information is valid and consistent, false otherwise.
 */
bool ContestValidateQuery::check_processed_upto() {
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
  processed_upto_updated_ = upd;
  if (upd) {
    if (upd->shard != shard_.shard) {
      return reject_query("newly-added ProcessedInfo entry refers to shard "s +
                          ShardIdFull{workchain(), upd->shard}.to_str() + " distinct from the current shard " +
                          shard_.to_str());
    }
    auto ref_mc_seqno = mc_seqno_;
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

/**
 * Check that the difference between the old and new dispatch queues is reflected in OutMsgs and InMsgs
 *
 * @returns True if the check is successful, false otherwise.
 */
bool ContestValidateQuery::check_dispatch_queue_update() {
  if (!new_dispatch_queue_messages_.empty()) {
    auto it = new_dispatch_queue_messages_.begin();
    return reject_query(PSTRING() << "DispatchQueue has a new message with src_addr=" << it->first.first.to_hex()
                                  << ", lt=" << it->first.second << ", but no correseponding OutMsg exists");
  }
  if (!removed_dispatch_queue_messages_.empty()) {
    auto it = removed_dispatch_queue_messages_.begin();
    return reject_query(PSTRING() << "message with src_addr=" << it->first.first.to_hex() << ", lt=" << it->first.second
                                  << " was removed from DispatchQueue, but no correseponding InMsg exists");
  }
  return true;
}

/**
 * Checks the validity of an outbound message in the neighbor's queue.
 * Similar to Collator::process_inbound_message.
 *
 * @param enq_msg The enqueued message to validate.
 * @param lt The logical time of the message.
 * @param key The 32+64+256-bit key of the message.
 * @param nb The neighbor's description.
 * @param unprocessed A boolean flag that will be set to true if the message is unprocessed, false otherwise.
 *
 * @returns True if the message is valid, false otherwise.
 */
bool ContestValidateQuery::check_neighbor_outbound_message(Ref<vm::CellSlice> enq_msg, ton::LogicalTime lt,
                                                           td::ConstBitPtr key, const block::McShardDescr& nb,
                                                           bool& unprocessed, bool& processed_here,
                                                           td::Bits256& msg_hash) {
  unprocessed = false;
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
  processed_here = f1 && !f0;
  msg_hash = enq.hash_;
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
      LOG(INFO) << "old processed_upto: " << ps_.processed_upto_->to_str();
      LOG(INFO) << "new processed_upto: " << ns_.processed_upto_->to_str();
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

/**
 * Checks messages from the outbound queues of the neighbors.
 *
 * @returns True if the messages are valid, false otherwise.
 */
bool ContestValidateQuery::check_in_queue() {
  int imported_messages_count = 0;
  in_msg_dict_->check_for_each_extra([&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr, int) {
    int tag = block::gen::t_InMsg.get_tag(*value);
    if (tag == block::gen::InMsg::msg_import_fin || tag == block::gen::InMsg::msg_import_tr) {
      ++imported_messages_count;
    }
    return true;
  });
  if (imported_messages_count == 0 && claimed_proc_lt_ == 0) {
    return true;
  }

  std::vector<block::OutputQueueMerger::Neighbor> neighbor_queues;
  for (const auto& descr : neighbors_) {
    td::BitArray<96> key;
    key.bits().store_int(descr.workchain(), 32);
    (key.bits() + 32).store_uint(descr.shard().shard, 64);
    neighbor_queues.emplace_back(descr.top_block_id(), descr.outmsg_root, descr.disabled_);
  }
  block::OutputQueueMerger nb_out_msgs(shard_, std::move(neighbor_queues));
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
    bool processed_here = false;
    td::Bits256 msg_hash;
    if (!check_neighbor_outbound_message(kv->msg, kv->lt, kv->key.cbits(), neighbors_.at(kv->source), unprocessed,
                                         processed_here, msg_hash)) {
      if (verbosity > 1) {
        std::cerr << "invalid neighbor outbound message: lt=" << kv->lt << " from=" << kv->source
                  << " key=" << kv->key.to_hex() << " msg=";
        block::gen::t_EnqueuedMsg.print(std::cerr, *(kv->msg));
      }
      return reject_query("error processing outbound internal message "s + kv->key.to_hex() + " of neighbor " +
                          neighbors_.at(kv->source).blk_.to_str());
    }
    if (processed_here) {
      --imported_messages_count;
    }
    auto msg_lt = kv->lt;
    if (imported_messages_count == 0 && msg_lt == claimed_proc_lt_ && msg_hash == claimed_proc_hash_) {
      return true;
    }
    if (unprocessed) {
      return true;
    }
    nb_out_msgs.next();
  }
  return true;
}

/**
 * Creates a new Account object from the given address and serialized account data.
 * Creates a new Account if not found.
 * Similar to Collator::make_account_from()
 *
 * @param addr A pointer to the 256-bit address of the account.
 * @param account A cell slice with an account serialized using ShardAccount TLB-scheme.
 *
 * @returns A unique pointer to the created Account object, or nullptr if the creation failed.
 */
std::unique_ptr<block::Account> ContestValidateQuery::make_account_from(td::ConstBitPtr addr,
                                                                        Ref<vm::CellSlice> account) {
  auto ptr = std::make_unique<block::Account>(workchain(), addr);
  if (account.is_null()) {
    if (!ptr->init_new(now_)) {
      return nullptr;
    }
  } else if (!ptr->unpack(std::move(account), now_, false)) {
    return nullptr;
  }
  ptr->block_lt = start_lt_;
  return ptr;
}

/**
 * Retreives an Account object from the data in the shard state.
 * Accounts are cached in the ValidatorQuery's map.
 * Similar to Collator::make_account()
 *
 * @param addr The 256-bit address of the account.
 *
 * @returns Pointer to the account if found or created successfully.
 *          Returns nullptr if an error occured.
 */
std::unique_ptr<block::Account> ContestValidateQuery::unpack_account(td::ConstBitPtr addr) {
  auto dict_entry = ps_.account_dict_->lookup_extra(addr, 256);
  auto new_acc = make_account_from(addr, std::move(dict_entry.first));
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

/**
 * Checks the validity of a single transaction for a given account.
 * Performs transaction execution.
 *
 * @param account The account of the transaction.
 * @param lt The logical time of the transaction.
 * @param trans_root The root of the transaction.
 * @param is_first Flag indicating if this is the first transaction of the account.
 * @param is_last Flag indicating if this is the last transaction of the account.
 *
 * @returns True if the transaction is valid, false otherwise.
 */
bool ContestValidateQuery::check_one_transaction(block::Account& account, ton::LogicalTime lt, Ref<vm::Cell> trans_root,
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
  bool is_special_tx = false;  // recover/mint transaction
  auto td_cs = vm::load_cell_slice(trans.description);
  int tag = block::gen::t_TransactionDescr.get_tag(td_cs);
  CHECK(tag >= 0);  // we have already validated the serialization of all Transactions
  td::optional<block::MsgMetadata> in_msg_metadata;
  if (in_msg_root.not_null()) {
    auto in_descr_cs = in_msg_dict_->lookup(in_msg_root->get_hash().as_bitslice());
    if (in_descr_cs.is_null()) {
      return reject_query(PSTRING() << "inbound message with hash " << in_msg_root->get_hash().to_hex()
                                    << " of transaction " << lt << " of account " << addr.to_hex()
                                    << " does not have a corresponding InMsg record");
    }
    auto in_msg_tag = block::gen::t_InMsg.get_tag(*in_descr_cs);
    if (in_msg_tag != block::gen::InMsg::msg_import_ext && in_msg_tag != block::gen::InMsg::msg_import_fin &&
        in_msg_tag != block::gen::InMsg::msg_import_imm && in_msg_tag != block::gen::InMsg::msg_import_ihr &&
        in_msg_tag != block::gen::InMsg::msg_import_deferred_fin) {
      return reject_query(PSTRING() << "inbound message with hash " << in_msg_root->get_hash().to_hex()
                                    << " of transaction " << lt << " of account " << addr.to_hex()
                                    << " has an invalid InMsg record (not one of msg_import_ext, msg_import_fin, "
                                       "msg_import_imm, msg_import_ihr or msg_import_deferred_fin)");
    }
    is_special_tx = is_special_in_msg(*in_descr_cs);
    // once we know there is a InMsg with correct hash, we already know that it contains a message with this hash (by the verification of InMsg), so it is our message
    // have still to check its destination address and imported value
    // and that it refers to this transaction
    Ref<vm::CellSlice> dest;
    if (in_msg_tag == block::gen::InMsg::msg_import_ext) {
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
      LogicalTime emitted_lt = info.created_lt;  // See ContestValidateQuery::check_message_processing_order
      if (in_msg_tag == block::gen::InMsg::msg_import_imm || in_msg_tag == block::gen::InMsg::msg_import_fin ||
          in_msg_tag == block::gen::InMsg::msg_import_deferred_fin) {
        block::tlb::MsgEnvelope::Record_std msg_env;
        if (!block::tlb::unpack_cell(in_descr_cs->prefetch_ref(), msg_env)) {
          return reject_query(PSTRING() << "InMsg record for inbound message with hash "
                                        << in_msg_root->get_hash().to_hex() << " of transaction " << lt
                                        << " of account " << addr.to_hex() << " does not have a valid MsgEnvelope");
        }
        in_msg_metadata = std::move(msg_env.metadata);
        if (msg_env.emitted_lt) {
          emitted_lt = msg_env.emitted_lt.value();
        }
      }
      if (info.created_lt != start_lt_ || !is_special_tx) {
        msg_proc_lt_.emplace_back(addr, lt, emitted_lt);
      }
      dest = std::move(info.dest);
      CHECK(money_imported.validate_unpack(info.value));
      ihr_delivered = (in_msg_tag == block::gen::InMsg::msg_import_ihr);
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
  td::optional<block::MsgMetadata> new_msg_metadata;
  if (msg_metadata_enabled_) {
    if (external || is_special_tx || tag != block::gen::TransactionDescr::trans_ord) {
      new_msg_metadata = block::MsgMetadata{0, account.workchain, account.addr, (LogicalTime)trans.lt};
    } else if (in_msg_metadata) {
      new_msg_metadata = std::move(in_msg_metadata);
      ++new_msg_metadata.value().depth;
    }
  }
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
        tag != block::gen::OutMsg::msg_export_imm && tag != block::gen::OutMsg::msg_export_new_defer) {
      return reject_query(PSTRING() << "outbound message #" << i + 1 << " with hash "
                                    << out_msg_root->get_hash().to_hex() << " of transaction " << lt << " of account "
                                    << addr.to_hex()
                                    << " has an invalid OutMsg record (not one of msg_export_ext, msg_export_new, "
                                       "msg_export_imm or msg_export_new_defer)");
    }
    // once we know there is an OutMsg with correct hash, we already know that it contains a message with this hash
    // (by the verification of OutMsg), so it is our message
    // have still to check its source address, lt and imported value
    // and that it refers to this transaction as its origin
    Ref<vm::CellSlice> src;
    LogicalTime message_lt;
    if (tag == block::gen::OutMsg::msg_export_ext) {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info;
      CHECK(tlb::unpack_cell_inexact(out_msg_root, info));
      src = std::move(info.src);
      message_lt = info.created_lt;
    } else {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      CHECK(tlb::unpack_cell_inexact(out_msg_root, info));
      src = std::move(info.src);
      message_lt = info.created_lt;
      block::tlb::MsgEnvelope::Record_std msg_env;
      CHECK(tlb::unpack_cell(out_descr_cs->prefetch_ref(), msg_env));
      // unpack exported message value (from this transaction)
      block::CurrencyCollection msg_export_value;
      CHECK(msg_export_value.unpack(info.value));
      msg_export_value += block::tlb::t_Grams.as_integer(info.ihr_fee);
      msg_export_value += msg_env.fwd_fee_remaining;
      CHECK(msg_export_value.is_valid());
      money_exported += msg_export_value;
      if (msg_env.metadata != new_msg_metadata) {
        return reject_query(PSTRING() << "outbound message #" << i + 1 << " with hash "
                                      << out_msg_root->get_hash().to_hex() << " of transaction " << lt << " of account "
                                      << addr.to_hex() << " has invalid metadata in an OutMsg record: expected "
                                      << (new_msg_metadata ? new_msg_metadata.value().to_str() : "<none>") << ", found "
                                      << (msg_env.metadata ? msg_env.metadata.value().to_str() : "<none>"));
      }
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
    if (tag != block::gen::OutMsg::msg_export_ext) {
      bool is_deferred = tag == block::gen::OutMsg::msg_export_new_defer;
      if (account_expected_defer_all_messages_.count(ss_addr) && !is_deferred) {
        return reject_query(
            PSTRING() << "outbound message #" << i + 1 << " on account " << workchain() << ":" << ss_addr.to_hex()
                      << " must be deferred because this account has earlier messages in DispatchQueue");
      }
      if (is_deferred) {
        LOG(INFO) << "message from account " << workchain() << ":" << ss_addr.to_hex() << " with lt " << message_lt
                  << " was deferred";
        if (!deferring_messages_enabled_ && !account_expected_defer_all_messages_.count(ss_addr)) {
          return reject_query(PSTRING() << "outbound message #" << i + 1 << " on account " << workchain() << ":"
                                        << ss_addr.to_hex() << " is deferred, but deferring messages is disabled");
        }
        if (i == 0 && !account_expected_defer_all_messages_.count(ss_addr)) {
          return reject_query(PSTRING() << "outbound message #1 on account " << workchain() << ":" << ss_addr.to_hex()
                                        << " must not be deferred (the first message cannot be deferred unless some "
                                           "prevoius messages are deferred)");
        }
        account_expected_defer_all_messages_.insert(ss_addr);
      }
    }
  }
  CHECK(money_exported.is_valid());
  // check general transaction data
  block::CurrencyCollection old_balance{account.get_balance()};
  if (tag == block::gen::TransactionDescr::trans_merge_prepare ||
      tag == block::gen::TransactionDescr::trans_merge_install ||
      tag == block::gen::TransactionDescr::trans_split_prepare ||
      tag == block::gen::TransactionDescr::trans_split_install) {
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
    return reject_query(PSTRING() << "transaction " << lt << " of account " << addr.to_hex()
                                  << " is a tick-tock transaction, which is impossible outside a masterchain block");
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
  int trans_type = block::transaction::Transaction::tr_none;
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord: {
      trans_type = block::transaction::Transaction::tr_ord;
      if (in_msg_root.is_null()) {
        return reject_query(PSTRING() << "ordinary transaction " << lt << " of account " << addr.to_hex()
                                      << " has no inbound message");
      }
      need_credit_phase = !external;
      break;
    }
    case block::gen::TransactionDescr::trans_storage: {
      trans_type = block::transaction::Transaction::tr_storage;
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
      trans_type = is_tock ? block::transaction::Transaction::tr_tock : block::transaction::Transaction::tr_tick;
      if (in_msg_root.not_null()) {
        return reject_query(PSTRING() << (is_tock ? "tock" : "tick") << " transaction " << lt << " of account "
                                      << addr.to_hex() << " has an inbound message");
      }
      break;
    }
    case block::gen::TransactionDescr::trans_merge_prepare: {
      trans_type = block::transaction::Transaction::tr_merge_prepare;
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
      trans_type = block::transaction::Transaction::tr_merge_install;
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
      trans_type = block::transaction::Transaction::tr_split_prepare;
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
      trans_type = block::transaction::Transaction::tr_split_install;
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
  std::unique_ptr<block::transaction::Transaction> trs =
      std::make_unique<block::transaction::Transaction>(account, trans_type, lt, now_, in_msg_root);
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
  if (trs->bounce_enabled &&
      (!trs->compute_phase->success || trs->action_phase->state_exceeds_limits || trs->action_phase->bounce) &&
      !trs->prepare_bounce_phase(action_phase_cfg_)) {
    return reject_query(PSTRING() << "cannot re-create bounce phase of  transaction " << lt << " for smart contract "
                                  << addr.to_hex());
  }
  if (!trs->serialize()) {
    return reject_query(PSTRING() << "cannot re-create the serialization of  transaction " << lt
                                  << " for smart contract " << addr.to_hex());
  }
  if (!trs->update_limits(*block_limit_status_, /* with_gas = */ false, /* with_size = */ false)) {
    return fatal_error(PSTRING() << "cannot update block limit status to include transaction " << lt << " of account "
                                 << addr.to_hex());
  }

  // Collator should stop if total gas usage exceeds limits, including transactions on special accounts, but without
  // ticktocks and mint/recover.
  // Here Validator checks a weaker condition
  if (!is_special_tx && !trs->gas_limit_overridden && trans_type == block::transaction::Transaction::tr_ord) {
    (account.is_special ? total_special_gas_used_ : total_gas_used_) += trs->gas_used();
  }
  if (total_gas_used_ > block_limits_->gas.hard() + compute_phase_cfg_.gas_limit) {
    return reject_query(PSTRING() << "gas block limits are exceeded: total_gas_used > gas_limit_hard + trx_gas_limit ("
                                  << "total_gas_used=" << total_gas_used_
                                  << ", gas_limit_hard=" << block_limits_->gas.hard()
                                  << ", trx_gas_limit=" << compute_phase_cfg_.gas_limit << ")");
  }
  if (total_special_gas_used_ > block_limits_->gas.hard() + compute_phase_cfg_.special_gas_limit) {
    return reject_query(
        PSTRING() << "gas block limits are exceeded: total_special_gas_used > gas_limit_hard + special_gas_limit ("
                  << "total_special_gas_used=" << total_special_gas_used_
                  << ", gas_limit_hard=" << block_limits_->gas.hard()
                  << ", special_gas_limit=" << compute_phase_cfg_.special_gas_limit << ")");
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
  total_burned_ += trs->blackhole_burned;
  // check new balance and value flow
  auto new_balance = account.get_balance();
  block::CurrencyCollection total_fees;
  if (!total_fees.validate_unpack(trans.total_fees)) {
    return reject_query(PSTRING() << "transaction " << lt << " of " << addr.to_hex()
                                  << " has an invalid total_fees value");
  }
  if (old_balance + money_imported != new_balance + money_exported + total_fees + trs->blackhole_burned) {
    return reject_query(
        PSTRING() << "transaction " << lt << " of " << addr.to_hex()
                  << " violates the currency flow condition: old balance=" << old_balance.to_str()
                  << " + imported=" << money_imported.to_str() << " does not equal new balance=" << new_balance.to_str()
                  << " + exported=" << money_exported.to_str() << " + total_fees=" << total_fees.to_str()
                  << (trs->blackhole_burned.is_zero() ? ""
                                                      : PSTRING() << " burned=" << trs->blackhole_burned.to_str()));
  }
  return true;
}

/**
 * Checks the validity of transactions for a given account block.
 * NB: may be run in parallel for different accounts
 *
 * @param acc_addr The address of the account.
 * @param acc_blk_root The root of the AccountBlock.
 *
 * @returns True if the account transactions are valid, false otherwise.
 */
bool ContestValidateQuery::check_account_transactions(const StdSmcAddress& acc_addr, Ref<vm::CellSlice> acc_blk_root) {
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

  // See Collator::combine_account_trabsactions
  if (account.total_state->get_hash() != account.orig_total_state->get_hash()) {
    // account changed
    if (account.orig_status == block::Account::acc_nonexist) {
      // account created
      CHECK(account.status != block::Account::acc_nonexist);
      vm::CellBuilder cb;
      if (!(cb.store_ref_bool(account.total_state)             // account_descr$_ account:^Account
            && cb.store_bits_bool(account.last_trans_hash_)    // last_trans_hash:bits256
            && cb.store_long_bool(account.last_trans_lt_, 64)  // last_trans_lt:uint64
            && ns_.account_dict_->set_builder(account.addr, cb, vm::Dictionary::SetMode::Add))) {
        return fatal_error(std::string{"cannot add newly-created account "} + account.addr.to_hex() +
                           " into ShardAccounts");
      }
    } else if (account.status == block::Account::acc_nonexist) {
      // account deleted
      if (verbosity > 2) {
        std::cerr << "deleting account " << account.addr.to_hex() << " with empty new value ";
        block::gen::t_Account.print_ref(std::cerr, account.total_state);
      }
      if (ns_.account_dict_->lookup_delete(account.addr).is_null()) {
        return fatal_error(std::string{"cannot delete account "} + account.addr.to_hex() + " from ShardAccounts");
      }
    } else {
      // existing account modified
      if (verbosity > 4) {
        std::cerr << "modifying account " << account.addr.to_hex() << " to ";
        block::gen::t_Account.print_ref(std::cerr, account.total_state);
      }
      vm::CellBuilder cb;
      if (!(cb.store_ref_bool(account.total_state)             // account_descr$_ account:^Account
            && cb.store_bits_bool(account.last_trans_hash_)    // last_trans_hash:bits256
            && cb.store_long_bool(account.last_trans_lt_, 64)  // last_trans_lt:uint64
            && ns_.account_dict_->set_builder(account.addr, cb, vm::Dictionary::SetMode::Replace))) {
        return fatal_error(std::string{"cannot modify existing account "} + account.addr.to_hex() +
                           " in ShardAccounts");
      }
    }
  }

  block::gen::HASH_UPDATE::Record hash_upd;
  if (!tlb::type_unpack_cell(std::move(acc_blk.state_update), block::gen::t_HASH_UPDATE_Account, hash_upd)) {
    return reject_query("cannot extract (HASH_UPDATE Account) from the AccountBlock of "s + account.addr.to_hex());
  }
  block::tlb::ShardAccount::Record old_state, new_state;
  if (!(old_state.unpack(ps_.account_dict_->lookup(account.addr)) &&
        new_state.unpack(ns_.account_dict_->lookup(account.addr)))) {
    return reject_query("cannot extract Account from the ShardAccount of "s + account.addr.to_hex());
  }
  if (hash_upd.old_hash != old_state.account->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + account.addr.to_hex() +
                        " has incorrect old hash");
  }
  if (hash_upd.new_hash != new_state.account->get_hash().bits()) {
    return reject_query("(HASH_UPDATE Account) from the AccountBlock of "s + account.addr.to_hex() +
                        " has incorrect new hash");
  }

  return true;
}

/**
 * Checks all transactions in the account blocks.
 *
 * @returns True if all transactions pass the check, False otherwise.
 */
bool ContestValidateQuery::check_transactions() {
  LOG(INFO) << "checking all transactions";
  ns_.account_dict_ =
      std::make_unique<vm::AugmentedDictionary>(ps_.account_dict_->get_root(), 256, block::tlb::aug_ShardAccounts);
  bool ok = account_blocks_dict_->check_for_each_extra(
      [this](Ref<vm::CellSlice> value, Ref<vm::CellSlice> extra, td::ConstBitPtr key, int key_len) {
        CHECK(key_len == 256);
        return check_account_transactions(key, std::move(value));
      });

  return ok;
}

/**
 * Checks the processing order of messages in a block.
 *
 * @returns True if the processing order of messages is valid, false otherwise.
 */
bool ContestValidateQuery::check_message_processing_order() {
  // Old rule: if messages m1 and m2 with the same destination generate transactions t1 and t2,
  // then (m1.created_lt < m2.created_lt) => (t1.lt < t2.lt).
  // New rule:
  // If message was taken from dispatch queue, instead of created_lt use emitted_lt
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

  // Check that if messages m1 and m2 with the same source have m1.created_lt < m2.created_lt then
  // m1.emitted_lt < m2.emitted_lt.
  std::sort(msg_emitted_lt_.begin(), msg_emitted_lt_.end());
  for (std::size_t i = 1; i < msg_emitted_lt_.size(); i++) {
    auto &a = msg_emitted_lt_[i - 1], &b = msg_emitted_lt_[i];
    if (std::get<0>(a) == std::get<0>(b) && std::get<2>(a) >= std::get<2>(b)) {
      return reject_query(PSTRING() << "incorrect deferred message processing order for sender "
                                    << std::get<0>(a).to_hex() << ": message with created_lt " << std::get<1>(a)
                                    << " has emitted_lt" << std::get<2>(a) << ", but message with created_lt "
                                    << std::get<1>(b) << " has emitted_lt" << std::get<2>(b));
    }
  }
  return true;
}

/**
 * Checks the validity of the new shard state.
 *
 * @returns True if the new state is valid, false otherwise.
 */
bool ContestValidateQuery::check_new_state() {
  // shard_state#9023afe2 global_id:int32 -> checked in unpack_next_state()
  // shard_id:ShardIdent -> checked in unpack_next_state()
  // seq_no:uint32 vert_seq_no:# -> checked in unpack_next_state()
  // gen_utime:uint32 gen_lt:uint64 -> checked in unpack_next_state()
  // min_ref_mc_seqno:uint32
  ton::BlockSeqno my_mc_seqno = mc_seqno_;
  ton::BlockSeqno ref_mc_seqno =
      std::min(std::min(my_mc_seqno, min_shard_ref_mc_seqno_), ns_.processed_upto_->min_mc_seqno());
  ns_.min_ref_mc_seqno_ = ref_mc_seqno;
  // out_msg_queue_info:^OutMsgQueueInfo
  // -> _ out_queue:OutMsgQueue proc_info:ProcessedInfo
  //      ihr_pending:IhrPendingInfo = OutMsgQueueInfo;

  // before_split:(## 1) -> checked in unpack_next_state()
  // accounts:^ShardAccounts -> checked in precheck_account_updates() + other
  // ^[ overload_history:uint64 underload_history:uint64
  ns_.overload_history_ = ((ps_.overload_history_ << 1) | extra_collated_data_.overload);
  ns_.underload_history_ = ((ps_.underload_history_ << 1) | extra_collated_data_.underload);

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
  block::CurrencyCollection old_total_validator_fees(ps_.total_validator_fees_);
  ns_.total_validator_fees_ = old_total_validator_fees + value_flow_.fees_collected - value_flow_.recovered;
  ns_.total_balance_ = value_flow_.to_next_blk;
  return true;
}

/**
 * Validates the value flow of a block.
 *
 * @returns True if the value flow is valid, False otherwise.
 */
bool ContestValidateQuery::postcheck_value_flow() {
  auto accounts_extra = ns_.account_dict_->get_root_extra();
  block::CurrencyCollection cc;
  if (!(accounts_extra.write().advance(5) && cc.unpack(std::move(accounts_extra)))) {
    return reject_query("cannot unpack CurrencyCollection from the root of new accounts dictionary");
  }
  if (cc != value_flow_.to_next_blk) {
    return reject_query("ValueFlow for "s + id_.to_str() + " declares to_next_blk=" + value_flow_.to_next_blk.to_str() +
                        " but the sum over all accounts present in the new state is " + cc.to_str());
  }

  auto expected_fees =
      value_flow_.fees_imported + value_flow_.created + transaction_fees_ + import_fees_ - fees_burned_;
  if (value_flow_.fees_collected != expected_fees) {
    return reject_query(PSTRING() << "ValueFlow for " << id_.to_str() << " declares fees_collected="
                                  << value_flow_.fees_collected.to_str() << " but the total message import fees are "
                                  << import_fees_ << ", the total transaction fees are " << transaction_fees_.to_str()
                                  << ", creation fee for this block is " << value_flow_.created.to_str()
                                  << ", the total imported fees from shards are " << value_flow_.fees_imported.to_str()
                                  << " and the burned fees are " << fees_burned_.to_str() << " with a total of "
                                  << expected_fees.to_str());
  }
  if (total_burned_ != value_flow_.burned) {
    return reject_query(PSTRING() << "invalid burned in value flow: " << id_.to_str() << " declared "
                                  << value_flow_.burned.to_str() << ", correct value is " << total_burned_.to_str());
  }
  return true;
}

Ref<vm::Cell> ContestValidateQuery::get_virt_state_root(td::Bits256 block_root_hash) {
  auto it = virt_roots_.find(block_root_hash);
  if (it == virt_roots_.end()) {
    return {};
  }
  Ref<vm::Cell> root = it->second;
  block::gen::Block::Record block;
  if (!tlb::unpack_cell(root, block)) {
    return {};
  }
  vm::CellSlice upd_cs{vm::NoVmSpec(), block.state_update};
  if (!(upd_cs.is_special() && upd_cs.prefetch_long(8) == 4  // merkle update
        && upd_cs.size_ext() == 0x20228)) {
    return {};
  }
  return vm::MerkleProof::virtualize_raw(upd_cs.prefetch_ref(1), {0, 1});
}

/**
 * MAIN VALIDATOR FUNCTION (invokes other methods in a suitable order).
 *
 * @returns True if the validation is successful, False otherwise.
 */
bool ContestValidateQuery::try_validate() {
  if (pending) {
    return true;
  }
  try {
    if (!stage_) {
      LOG(INFO) << "try_validate stage 0";
      if (!compute_prev_state()) {
        return fatal_error(-666, "cannot compute previous state");
      }
      if (!request_neighbor_queues()) {
        return fatal_error("cannot request neighbor output queues");
      }
      if (!unpack_prev_state()) {
        return fatal_error("cannot unpack previous state");
      }
      if (!init_next_state()) {
        return fatal_error("cannot unpack previous state");
      }
      if (!check_utime_lt()) {
        return reject_query("creation utime/lt of the new block is invalid");
      }
      if (!prepare_out_msg_queue_size()) {
        return reject_query("cannot request out msg queue size");
      }
      stage_ = 1;
      if (pending) {
        return true;
      }
    }
    LOG(INFO) << "try_validate stage 1";
    LOG(INFO) << "running automated validity checks for block candidate " << id_.to_str();
    if (!block::gen::t_BlockRelaxed.validate_ref(10000000, block_root_)) {
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
    if (!precheck_account_transactions()) {
      return reject_query("invalid collection of account transactions in ShardAccountBlocks");
    }
    if (!build_new_message_queue()) {
      return reject_query("cannot build a new message queue");
    }
    if (!precheck_message_queue_update()) {
      return reject_query("invalid OutMsgQueue update");
    }
    if (!unpack_dispatch_queue_update()) {
      return reject_query("invalid DispatchQueue update");
    }
    if (!check_in_msg_descr()) {
      return reject_query("invalid InMsgDescr");
    }
    if (!check_out_msg_descr()) {
      return reject_query("invalid OutMsgDescr");
    }
    if (!check_dispatch_queue_update()) {
      return reject_query("invalid OutMsgDescr");
    }
    if (!check_processed_upto()) {
      return reject_query("invalid ProcessedInfo");
    }
    if (!check_in_queue()) {
      return reject_query("cannot check inbound message queues");
    }
    if (!check_transactions()) {
      return reject_query("invalid collection of account transactions in ShardAccountBlocks");
    }
    if (!postcheck_account_updates()) {
      return reject_query("invalid AccountState update");
    }
    if (!check_message_processing_order()) {
      return reject_query("some messages have been processed by transactions in incorrect order");
    }
    if (!check_new_state()) {
      return reject_query("the header of the new shardchain state is invalid");
    }
    if (!postcheck_value_flow()) {
      return reject_query("new ValueFlow is invalid");
    }
    if (!build_state_update()) {
      return reject_query("cannot build state update");
    }
  } catch (vm::VmError& err) {
    return fatal_error(-666, err.get_msg());
  } catch (vm::VmVirtError& err) {
    return reject_query(err.get_msg());
  }
  finish_query();
  return true;
}

/**
 * Creates a new shard state and generates Merkle update. The serialized update is stored to result_state_update_.
 *
 * @return True on success, False on error.
 */
bool ContestValidateQuery::build_state_update() {
  td::Ref<vm::Cell> msg_q_info;
  {
    vm::CellBuilder cb;
    // out_msg_queue_extra#0 dispatch_queue:DispatchQueue out_queue_size:(Maybe uint48) = OutMsgQueueExtra;
    // ... extra:(Maybe OutMsgQueueExtra)
    if (!(cb.store_long_bool(1, 1) && cb.store_long_bool(0, 4) && ns_.dispatch_queue_->append_dict_to_bool(cb))) {
      return false;
    }
    if (!(cb.store_bool_bool(true) && ns_.out_msg_queue_size_ &&
          cb.store_long_bool(ns_.out_msg_queue_size_.value(), 48))) {
      return false;
    }
    vm::CellSlice maybe_extra = cb.as_cellslice();
    cb.reset();
    bool ok = ns_.out_msg_queue_->append_dict_to_bool(cb)                  // _ out_queue:OutMsgQueue
              && cb.append_cellslice_bool(extra_collated_data_.proc_info)  // proc_info:ProcessedInfo
              && cb.append_cellslice_bool(maybe_extra)                     // extra:(Maybe OutMsgQueueExtra)
              && cb.finalize_to(msg_q_info);
    if (!ok) {
      return false;
    }
  }

  td::Ref<vm::Cell> state_root;
  vm::CellBuilder cb, cb2;

  // See Collator::create_shard_state
  if (!(cb.store_long_bool(0x9023afe2, 32)                     // shard_state#9023afe2
        && cb.store_long_bool(global_id_, 32)                  // global_id:int32
        && block::ShardId{shard_}.serialize(cb)                // shard_id:ShardIdent
        && cb.store_long_bool(id_.seqno(), 32)                 // seq_no:uint32
        && cb.store_long_bool(vert_seqno_, 32)                 // vert_seq_no:#
        && cb.store_long_bool(now_, 32)                        // gen_utime:uint32
        && cb.store_long_bool(ns_.lt_, 64)                     // gen_lt:uint64
        && cb.store_long_bool(ns_.min_ref_mc_seqno_, 32)       // min_ref_mc_seqno:uint32
        && cb.store_ref_bool(msg_q_info)                       // out_msg_queue_info:^OutMsgQueueInfo
        && cb.store_long_bool(before_split_, 1)                // before_split:Bool
        && ns_.account_dict_->append_dict_to_bool(cb2)         // accounts:^ShardAccounts
        && cb.store_ref_bool(cb2.finalize())                   // ...
        && cb2.store_long_bool(ns_.overload_history_, 64)      // ^[ overload_history:uint64
        && cb2.store_long_bool(ns_.underload_history_, 64)     //    underload_history:uint64
        && ns_.total_balance_.store(cb2)                       //  total_balance:CurrencyCollection
        && ns_.total_validator_fees_.store(cb2)                //  total_validator_fees:CurrencyCollection
        && cb2.store_bool_bool(false)                          //    libraries:(HashmapE 256 LibDescr)
        && cb2.store_bool_bool(true) && store_master_ref(cb2)  // master_ref:(Maybe BlkMasterInfo)
        && cb.store_ref_bool(cb2.finalize())                   // ]
        && cb.store_bool_bool(false)                           // custom:(Maybe ^McStateExtra)
        && cb.finalize_to(state_root))) {
    return fatal_error("cannot create new ShardState");
  }

  auto state_update = vm::MerkleUpdate::generate(prev_state_root_, state_root, state_usage_tree_.get());
  if (state_update.is_null()) {
    return fatal_error("failed to generate Merkle update");
  }
  result_state_update_ = vm::std_boc_serialize(state_update).move_as_ok();
  return true;
}

/**
 * Stores BlkMasterInfo (for non-masterchain blocks) in the provided CellBuilder.
 *
 * @param cb The CellBuilder to store the reference in.
 *
 * @returns True if the reference is successfully stored, false otherwise.
 */
bool ContestValidateQuery::store_master_ref(vm::CellBuilder& cb) {
  return cb.store_long_bool(mc_state_->get_logical_time(), 64)  // end_lt:uint64
         && cb.store_long_bool(mc_blkid_.seqno(), 32)           // seq_no:uint32
         && cb.store_bits_bool(mc_blkid_.root_hash)             // root_hash:bits256
         && cb.store_bits_bool(mc_blkid_.file_hash);            // file_hash:bits256
}

}  // namespace solution
