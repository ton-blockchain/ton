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
#include "shard.hpp"
#include "message-queue.hpp"
#include "validator-set.hpp"
#include "vm/boc.h"
#include "td/db/utils/BlobView.h"
#include "vm/db/StaticBagOfCellsDb.h"
#include "vm/cellslice.h"
#include "vm/cells/MerkleUpdate.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "td/utils/filesystem.h"

#define LAZY_STATE_DESERIALIZE 1

namespace ton {

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

ShardStateQ::ShardStateQ(const ShardStateQ& other)
    : blkid(other.blkid)
    , rhash(other.rhash)
    , data(other.data.is_null() ? td::BufferSlice{} : other.data.clone())
    , bocs_(other.bocs_)
    , root(other.root)
    , lt(other.lt)
    , utime(other.utime)
    , global_id_(other.global_id_)
    , before_split_(other.before_split_)
    , fake_split_(other.fake_split_)
    , fake_merge_(other.fake_merge_) {
}

ShardStateQ* ShardStateQ::make_copy() const {
  return new ShardStateQ(*this);
}

ShardStateQ::ShardStateQ(const BlockIdExt& _id, td::BufferSlice _data) : blkid(_id), data(std::move(_data)) {
}

ShardStateQ::ShardStateQ(const BlockIdExt& _id, Ref<vm::Cell> _root, td::BufferSlice _data)
    : blkid(_id), data(std::move(_data)), root(std::move(_root)) {
}

td::Result<Ref<ShardStateQ>> ShardStateQ::fetch(const BlockIdExt& _id, td::BufferSlice _data, Ref<vm::Cell> _root) {
  if (_id.is_masterchain()) {
    auto res = MasterchainStateQ::fetch(_id, std::move(_data), std::move(_root));
    if (res.is_error()) {
      return res.move_as_error();
    } else {
      return Ref<ShardStateQ>{res.move_as_ok()};
    }
  }
  Ref<ShardStateQ> res{true, _id, std::move(_root), std::move(_data)};
  td::Status err = res.unique_write().init();
  if (err.is_error()) {
    return err;
  } else {
    return std::move(res);
  }
}

td::Status ShardStateQ::init() {
  if (root.is_null()) {
    if (data.empty()) {
      return td::Status::Error(
          -668, "cannot initialize shardchain state without either a root cell or a BufferSlice with serialized data");
    }
#if LAZY_STATE_DESERIALIZE
    vm::StaticBagOfCellsDbLazy::Options options;
    options.check_crc32c = true;
    auto res = vm::StaticBagOfCellsDbLazy::create(td::BufferSliceBlobView::create(data.clone()), options);
    if (res.is_error()) {
      return res.move_as_error();
    }
    auto boc = res.move_as_ok();
    auto rc = boc->get_root_count();
    if (rc.is_error()) {
      return rc.move_as_error();
    }
    if (rc.move_as_ok() != 1) {
      return td::Status::Error(-668, "shardchain state BoC is invalid");
    }
    auto res3 = boc->get_root_cell(0);
    bocs_.clear();
    bocs_.push_back(std::move(boc));
#else
    auto res3 = vm::std_boc_deserialize(data.as_slice());
#endif
    if (res3.is_error()) {
      return res3.move_as_error();
    }
    root = res3.move_as_ok();
    if (root.is_null()) {
      return td::Status::Error(-668, "cannot extract root cell out of a shardchain state BoC");
    }
  }
  rhash = root->get_hash().bits();
  block::gen::ShardStateUnsplit::Record info;
  if (!tlb::unpack_cell(root, info)) {
    return td::Status::Error(-668,
                             "shardchain state for block "s + blkid.id.to_str() + " does not contain a valid header");
  }
  lt = info.gen_lt;
  utime = info.gen_utime;
  global_id_ = info.global_id;
  before_split_ = info.before_split;
  block::ShardId id{info.shard_id};
  ton::BlockId hdr_id{ton::ShardIdFull(id), info.seq_no};
  if (!id.is_valid() || get_shard() != ton::ShardIdFull(id) || get_seqno() != info.seq_no) {
    return td::Status::Error(-668, "header of unpacked shardchain state for block "s + blkid.id.to_str() +
                                       " contains BlockId " + hdr_id.to_str() +
                                       " different from the one originally required");
  }
  if (info.r1.master_ref.write().fetch_long(1)) {
    BlockIdExt mc_id;
    if (!block::tlb::t_ExtBlkRef.unpack(info.r1.master_ref, mc_id, nullptr)) {
      return td::Status::Error(-668, "cannot unpack master_ref in shardchain state of "s + blkid.to_str());
    }
    master_ref = mc_id;
  } else {
    master_ref = {};
  }
  return td::Status::OK();
}

td::Status ShardStateQ::validate_deep() const {
  if (data.empty()) {
    return td::Status::Error(-668,
                             "cannot validate serialized shard state because no serialized shard state is present");
  }
  auto res = vm::std_boc_deserialize(data.as_slice());
  if (res.is_error()) {
    return res.move_as_error();
  }
  auto root = res.move_as_ok();
  if (root.is_null()) {
    return td::Status::Error(-668, "cannot extract root cell out of a shardchain state BoC");
  }
  if (rhash != root->get_hash().bits()) {
    return td::Status::Error(-668, "root hash mismatch in a shardchain state BoC : expected "s + rhash.to_hex() +
                                       " , found " + root->get_hash().bits().to_hex(256));
  }
  return td::Status::OK();
}

td::Result<Ref<MessageQueue>> ShardStateQ::message_queue() const {
  if (root.is_null()) {
    return {};  // GIGO
  }
  vm::CellSlice cs{vm::NoVmOrd(), root};
  if (!cs.have(64, 1) || cs.prefetch_ulong(32) != (unsigned)block::tlb::ShardState::shard_state) {
    return td::Status::Error(-668, "state for block "s + blkid.id.to_str() + " is invalid");
  }
  if (fake_split_ || fake_merge_) {
    return td::Status::Error(-668, "cannot obtain message queue from a virtually split or merged state");
  }
  auto out_queue_info = cs.prefetch_ref();
  return Ref<MessageQueue>(Ref<MessageQueueQ>{true, blkid, std::move(out_queue_info)});
}

td::Status ShardStateQ::apply_block(BlockIdExt newid, td::Ref<BlockData> block) {
  if (block.is_null()) {
    return td::Status::Error(-666, "the block to be applied to a previous state is absent");
  }
  Ref<vm::Cell> block_root = block->root_cell();
  if (root.is_null() || block_root.is_null()) {
    return td::Status::Error(-666, "cannot apply an (empty) block to an (empty) state");
  }
  if (newid != block->block_id()) {
    return td::Status::Error(-666, "block id mismatch in apply_block()");
  }
  RootHash blk_rhash{block_root->get_hash().bits()};
  if (blk_rhash != newid.root_hash) {
    return td::Status::Error(-666, "cannot apply a block because its root hash differs from expected");
  }
  if (before_split_ != fake_split_) {
    return td::Status::Error(
        -666, "cannot apply a block because previous state has before_split set, but it has not been split virtually");
  }
  vm::CellSlice cs{vm::NoVmOrd{}, block_root};
  if (cs.prefetch_ulong(32) != 0x11ef55aa || !cs.have_refs(4)) {
    return td::Status::Error(-666, "invalid shardchain block header for block "s + block->block_id().id.to_str());
  }
  Ref<vm::Cell> update = cs.prefetch_ref(2);  // Merkle update
  auto next_state_root = vm::MerkleUpdate::apply(root, update);
  if (next_state_root.is_null()) {
    return td::Status::Error("cannot apply Merkle update from block "s + block->block_id().id.to_str() +
                             " to previous state");
  }
  blkid = block->block_id();
  // boc.reset();  // keep old lazy static bag of cells in case undeserialized branches are inherited by the current state
  data.clear();
  root = std::move(next_state_root);
  rhash = root->get_hash().bits();
  block::gen::ShardStateUnsplit::Record info;
  if (!tlb::unpack_cell(root, info)) {
    return td::Status::Error(
        -668, "newly-computed shardchain state for block "s + blkid.id.to_str() + " does not contain a valid header");
  }
  lt = info.gen_lt;
  utime = info.gen_utime;
  before_split_ = info.before_split;
  fake_split_ = fake_merge_ = false;
  block::ShardId id{info.shard_id};
  ton::BlockId hdr_id{ton::ShardIdFull(id), info.seq_no};
  if (!id.is_valid() || get_shard() != ton::ShardIdFull(id) || get_seqno() != info.seq_no) {
    return td::Status::Error(-668, "header of newly-computed shardchain state for block "s + blkid.id.to_str() +
                                       " contains a BlockId " + hdr_id.to_str() +
                                       " different from the one originally required");
  }
  return td::Status::OK();
}

td::Result<td::Ref<ShardState>> ShardStateQ::merge_with(const ShardState& with) const {
  const ShardStateQ& other = dynamic_cast<const ShardStateQ&>(with);
  if (fake_split_ || fake_merge_ || other.fake_split_ || other.fake_merge_) {
    return td::Status::Error(-666, "cannot merge blockchain states which have been split or merged immediately before");
  }
  if (before_split_ || other.before_split_) {
    return td::Status::Error(-666, "cannot merge blockchain states which have before_split flag set");
  }
  if (blkid.is_masterchain()) {
    return td::Status::Error(-666, "cannot merge masterchain states");
  }
  auto shard1 = blkid.shard_full(), shard2 = other.blkid.shard_full();
  if (shard1 == shard2 || !ton::shard_is_sibling(shard1, shard2)) {
    return td::Status::Error(-666, PSTRING() << "cannot merge states of shards " << shard1.to_str() << " and "
                                             << shard2.to_str() << " that are not siblings");
  }
  Ref<vm::Cell> root, root1 = root_cell(), root2 = other.root_cell();
  if (shard1.shard > shard2.shard) {
    std::swap(root1, root2);
  }
  if (!block::gen::t_ShardState.cell_pack_split_state(root, std::move(root1), std::move(root2))) {
    return td::Status::Error(-667, "cannot construct a virtual split_state after a merge");
  }
  auto m = Ref<ShardStateQ>{
      true,
      ton::BlockIdExt{blkid.id.workchain, ton::shard_parent(blkid.id.shard),
                      std::max(blkid.seqno(), other.blkid.seqno()), ton::Bits256::zero(), ton::Bits256::zero()},
      root};
  auto& ms = m.unique_write();
  ms.fake_merge_ = true;
  ms.rhash = root->get_hash().bits();
  ms.lt = std::max(lt, other.lt);
  ms.utime = std::max(utime, other.utime);
  ms.bocs_ = bocs_;
  ms.bocs_.insert(ms.bocs_.end(), other.bocs_.begin(), other.bocs_.end());
  return std::move(m);
}

td::Result<std::pair<td::Ref<ShardState>, td::Ref<ShardState>>> ShardStateQ::split() const {
  if (fake_split_ || fake_merge_) {
    return td::Status::Error(-666, "cannot split blockchain state which has been split or merged immediately before");
  }
  if (!before_split_) {
    return td::Status::Error(-666, "cannot split blockchain state which does not have before_split flag set");
  }
  if (blkid.is_masterchain()) {
    return td::Status::Error(-666, "cannot split masterchain state");
  }
  auto l = Ref<ShardStateQ>{true, *this};
  auto r = Ref<ShardStateQ>{true, *this};
  auto& ls = l.unique_write();
  auto& rs = r.unique_write();
  ls.fake_split_ = rs.fake_split_ = true;
  ls.blkid.id.shard = ton::shard_child(blkid.id.shard, true);
  rs.blkid.id.shard = ton::shard_child(blkid.id.shard, false);
  return std::make_pair<Ref<ShardState>, Ref<ShardState>>(std::move(l), std::move(r));
}

td::Result<td::BufferSlice> ShardStateQ::serialize() const {
  TD_PERF_COUNTER(serialize_state);
  td::PerfWarningTimer perf_timer_{"serializestate", 0.1};
  if (!data.is_null()) {
    return data.clone();
  }
  if (root.is_null()) {
    return td::Status::Error(-666, "cannot serialize an uninitialized state");
  }
  vm::BagOfCells new_boc;
  new_boc.set_root(root);
  auto res = new_boc.import_cells();
  if (res.is_error()) {
    return res.move_as_error();
  }
  auto st_res = new_boc.serialize_to_slice(31);
  if (st_res.is_error()) {
    LOG(ERROR) << "cannot serialize a shardchain state";
    return st_res.move_as_error();
  }
  // data = st_res.move_as_ok();
  // return data.clone();
  return st_res.move_as_ok();
}

td::Status ShardStateQ::serialize_to_file(td::FileFd& fd) const {
  TD_PERF_COUNTER(serialize_state_to_file);
  td::PerfWarningTimer perf_timer_{"serializestate", 0.1};
  if (!data.is_null()) {
    auto cur_data = data.clone();
    while (cur_data.size() > 0) {
      TRY_RESULT(s, fd.write(cur_data.as_slice()));
      cur_data.confirm_read(s);
    }
    return td::Status::OK();
  }
  if (root.is_null()) {
    return td::Status::Error(-666, "cannot serialize an uninitialized state");
  }
  vm::BagOfCells new_boc;
  new_boc.set_root(root);
  TRY_STATUS(new_boc.import_cells());
  auto st_res = new_boc.serialize_to_file(fd, 31);
  if (st_res.is_error()) {
    LOG(ERROR) << "cannot serialize a shardchain state";
    return st_res.move_as_error();
  }
  return td::Status::OK();
}

MasterchainStateQ::MasterchainStateQ(const BlockIdExt& _id, td::BufferSlice _data)
    : MasterchainState(), ShardStateQ(_id, std::move(_data)) {
}

MasterchainStateQ::MasterchainStateQ(const BlockIdExt& _id, Ref<vm::Cell> _root, td::BufferSlice _data)
    : MasterchainState(), ShardStateQ(_id, std::move(_root), std::move(_data)) {
}

MasterchainStateQ* MasterchainStateQ::make_copy() const {
  return new MasterchainStateQ(*this);
}

td::Result<Ref<MasterchainStateQ>> MasterchainStateQ::fetch(const BlockIdExt& _id, td::BufferSlice _data,
                                                            Ref<vm::Cell> _root) {
  if (!ShardIdFull(_id).is_masterchain_ext()) {
    return td::Status::Error(-666,
                             "invalid masterchain block/state id passed for creating a new masterchain state object");
  }
  Ref<MasterchainStateQ> res{true, _id, std::move(_root), std::move(_data)};
  td::Status err = res.unique_write().mc_init();
  if (err.is_error()) {
    return err;
  } else {
    return std::move(res);
  }
}

td::Status MasterchainStateQ::mc_init() {
  auto err = init();
  if (err.is_error()) {
    return err;
  }
  return mc_reinit();
}

td::Status MasterchainStateQ::mc_reinit() {
  auto res = block::ConfigInfo::extract_config(
      root_cell(), block::ConfigInfo::needStateRoot | block::ConfigInfo::needValidatorSet |
                       block::ConfigInfo::needShardHashes | block::ConfigInfo::needPrevBlocks |
                       block::ConfigInfo::needWorkchainInfo);
  cur_validators_.reset();
  next_validators_.reset();
  if (res.is_error()) {
    return res.move_as_error();
  }
  config_ = res.move_as_ok();
  CHECK(config_);
  CHECK(config_->set_block_id_ext(get_block_id()));

  auto cv_root = config_->get_config_param(35, 34);
  if (cv_root.not_null()) {
    TRY_RESULT(validators, block::Config::unpack_validator_set(std::move(cv_root), true));
    cur_validators_ = std::move(validators);
  }
  auto nv_root = config_->get_config_param(37, 36);
  if (nv_root.not_null()) {
    TRY_RESULT(validators, block::Config::unpack_validator_set(std::move(nv_root), true));
    next_validators_ = std::move(validators);
  }

  zerostate_id_ = config_->get_zerostate_id();
  return td::Status::OK();
}

td::Status MasterchainStateQ::apply_block(BlockIdExt id, td::Ref<BlockData> block) {
  auto err = ShardStateQ::apply_block(id, block);
  if (err.is_error()) {
    return err;
  }
  config_.reset();
  err = mc_reinit();
  if (err.is_error()) {
    LOG(ERROR) << "cannot extract masterchain-specific state data from newly-computed state for block "
               << id.id.to_str() << " : " << err.to_string();
  }
  return err;
}

td::Status MasterchainStateQ::prepare() {
  if (config_) {
    return td::Status::OK();
  }
  return mc_reinit();
}

Ref<ValidatorSet> MasterchainStateQ::compute_validator_set(ShardIdFull shard, const block::ValidatorSet& vset,
                                                           UnixTime time, CatchainSeqno ccseqno) const {
  if (!config_) {
    return {};
  }
  LOG(DEBUG) << "in compute_validator_set() for " << shard.to_str();
  auto nodes = config_->compute_validator_set_cc(shard, vset, time, &ccseqno);
  if (nodes.empty()) {
    return {};
  }
  return Ref<ValidatorSetQ>{true, ccseqno, shard, std::move(nodes)};
}

Ref<ValidatorSet> MasterchainStateQ::get_validator_set(ShardIdFull shard) const {
  if (!config_ || !cur_validators_) {
    LOG(ERROR) << "MasterchainStateQ::get_validator_set() : no config or no cur_validators";
    return {};
  }
  return compute_validator_set(shard, *cur_validators_, config_->utime, 0);
}

Ref<ValidatorSet> MasterchainStateQ::get_validator_set(ShardIdFull shard, UnixTime ts, CatchainSeqno cc_seqno) const {
  if (!config_ || !cur_validators_) {
    LOG(ERROR) << "MasterchainStateQ::get_validator_set() : no config or no cur_validators";
    return {};
  }
  auto nodes = config_->compute_validator_set(shard, *cur_validators_, ts, cc_seqno);
  if (nodes.empty()) {
    return {};
  }
  return Ref<ValidatorSetQ>{true, cc_seqno, shard, std::move(nodes)};
}

// next = -1 -> prev, next = 0 -> cur
Ref<ValidatorSet> MasterchainStateQ::get_total_validator_set(int next) const {
  if (!config_) {
    LOG(ERROR) << "MasterchainStateQ::get_total_validator_set() : no config";
    return {};
  }
  auto nodes = config_->compute_total_validator_set(next);
  if (nodes.empty()) {
    return {};
  }
  return Ref<ValidatorSetQ>{true, 0, ton::ShardIdFull{}, std::move(nodes)};
}

Ref<ValidatorSet> MasterchainStateQ::get_next_validator_set(ShardIdFull shard) const {
  if (!config_ || !cur_validators_) {
    LOG(ERROR) << "MasterchainStateQ::get_next_validator_set() : no config or no cur_validators";
    return {};
  }
  if (!next_validators_) {
    return compute_validator_set(shard, *cur_validators_, config_->utime, 1);
  }
  bool is_mc = shard.is_masterchain();
  auto ccv_cfg = config_->get_catchain_validators_config();
  unsigned cc_lifetime = is_mc ? ccv_cfg.mc_cc_lifetime : ccv_cfg.shard_cc_lifetime;
  if (next_validators_->utime_since > (config_->utime / cc_lifetime + 1) * cc_lifetime) {
    return compute_validator_set(shard, *cur_validators_, config_->utime, 1);
  } else {
    return compute_validator_set(shard, *next_validators_, config_->utime, 1);
  }
}

std::vector<Ref<McShardHash>> MasterchainStateQ::get_shards() const {
  if (!config_) {
    return {};
  }
  std::vector<ton::BlockId> shard_ids = config_->get_shard_hash_ids(true);
  std::vector<Ref<McShardHash>> v;
  for (const auto& b : shard_ids) {
    v.emplace_back(config_->get_shard_hash(ton::ShardIdFull(b)));
    CHECK(v.back().not_null());
  }
  return v;
}

td::Ref<McShardHash> MasterchainStateQ::get_shard_from_config(ShardIdFull shard) const {
  if (!config_) {
    return {};
  }
  return config_->get_shard_hash(shard);
}

bool MasterchainStateQ::rotated_all_shards() const {
  if (!config_) {
    return false;
  }
  return config_->rotated_all_shards();
}

bool MasterchainStateQ::get_old_mc_block_id(ton::BlockSeqno seqno, ton::BlockIdExt& blkid,
                                            ton::LogicalTime* end_lt) const {
  return config_ && config_->get_old_mc_block_id(seqno, blkid, end_lt);
}

bool MasterchainStateQ::check_old_mc_block_id(const ton::BlockIdExt& blkid, bool strict) const {
  return config_ && config_->check_old_mc_block_id(blkid, strict);
}

td::uint32 MasterchainStateQ::monitor_min_split_depth(WorkchainId workchain_id) const {
  if (!config_) {
    return 0;
  }
  auto wc_info = config_->get_workchain_info(workchain_id);
  return wc_info.not_null() ? wc_info->monitor_min_split : 0;
}

td::uint32 MasterchainStateQ::min_split_depth(WorkchainId workchain_id) const {
  if (!config_) {
    return 0;
  }
  auto wc_info = config_->get_workchain_info(workchain_id);
  return wc_info.not_null() ? wc_info->min_split : 0;
}

BlockSeqno MasterchainStateQ::min_ref_masterchain_seqno() const {
  return config_ ? config_->min_ref_mc_seqno_ : 0;
}

BlockIdExt MasterchainStateQ::last_key_block_id() const {
  BlockIdExt block_id;
  LogicalTime lt = 0;
  if (config_) {
    config_->get_last_key_block(block_id, lt);
  }
  return block_id;
}

BlockIdExt MasterchainStateQ::next_key_block_id(BlockSeqno seqno) const {
  BlockIdExt block_id;
  if (config_) {
    config_->get_next_key_block(seqno, block_id);
  }
  return block_id;
}

BlockIdExt MasterchainStateQ::prev_key_block_id(BlockSeqno seqno) const {
  BlockIdExt block_id;
  if (config_) {
    config_->get_prev_key_block(seqno, block_id);
  }
  return block_id;
}

bool MasterchainStateQ::is_key_state() const {
  return config_ ? config_->is_key_state() : false;
}

}  // namespace validator
}  // namespace ton
