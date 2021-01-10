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
#pragma once

#include "interfaces/shard-block.h"
#include "interfaces/validator-manager.h"
#include "block/mc-config.h"

namespace ton {

namespace validator {
using td::Ref;

class ValidateShardTopBlockDescr;

class ShardTopBlockDescrQBase : public ShardTopBlockDescription {
 protected:
  td::BufferSlice data_;
  ShardTopBlockDescrQBase() = default;
  ShardTopBlockDescrQBase(const ShardTopBlockDescrQBase& other) : data_(other.data_.clone()) {
  }
  ShardTopBlockDescrQBase(ShardTopBlockDescrQBase&&) = default;
  ShardTopBlockDescrQBase(td::BufferSlice data) : data_(std::move(data)) {
  }
};

class ShardTopBlockDescrQ final : public ShardTopBlockDescrQBase {
 public:
  enum Mode { fail_new = 1, fail_too_new = 2, allow_old = 4, allow_next_vset = 8, skip_check_sig = 16 };
  ShardIdFull shard() const override {
    return block_id_.shard_full();
  }
  BlockIdExt block_id() const override {
    return block_id_;
  }

  bool may_be_valid(BlockHandle last_masterchain_block_handle,
                    Ref<MasterchainState> last_masterchain_block_state) const override;
  td::Result<int> prevalidate(BlockIdExt last_mc_block_id, Ref<MasterchainState> last_mc_state, int mode,
                              int& res_flags) const;
  td::Result<int> validate(BlockIdExt last_mc_block_id, Ref<MasterchainState> last_mc_state, int mode, int& res_flags);

  td::BufferSlice serialize() const override {
    return data_.clone();
  }
  bool before_split() const override {
    return before_split_;
  }
  bool after_split() const override {
    return after_split_;
  }
  bool after_merge() const override {
    return after_merge_;
  }
  bool is_valid() const {
    return is_valid_;
  }
  std::size_t size() const {
    return chain_blk_ids_.size();
  }
  UnixTime generated_at() const override {
    return gen_utime_;
  }
  CatchainSeqno catchain_seqno() const override {
    return catchain_seqno_;
  }
  std::vector<BlockIdExt> get_prev_at(int pos) const;
  Ref<block::McShardHash> get_prev_descr(int pos, int sum_cnt = 0) const;
  Ref<block::McShardHash> get_top_descr(int sum_cnt = 0) const {
    return get_prev_descr(0, sum_cnt);
  }
  std::vector<td::Bits256> get_creator_list(int count) const;
  Ref<vm::Cell> get_root() const {
    return root_;
  }
  BlockSeqno get_vert_seqno() const {
    return vert_seqno_;
  }
  ShardTopBlockDescrQ(td::BufferSlice data, bool is_fake = false)
      : ShardTopBlockDescrQBase(std::move(data)), is_fake_(is_fake) {
  }
  ShardTopBlockDescrQ(Ref<vm::Cell> root, bool is_fake = false)
      : ShardTopBlockDescrQBase(), root_(std::move(root)), is_fake_(is_fake) {
  }

  ShardTopBlockDescrQ* make_copy() const override;

  static td::Result<Ref<ShardTopBlockDescrQ>> fetch(td::BufferSlice data, bool is_fake = false);
  static td::Result<Ref<ShardTopBlockDescrQ>> fetch(Ref<vm::Cell> root, bool is_fake = false);

 protected:
  friend class ValidateShardTopBlockDescr;
  td::Status unpack();

 private:
  BlockIdExt block_id_;
  Ref<vm::Cell> root_;
  bool is_valid_{false};
  bool is_fake_{false};
  bool after_split_{false};
  bool after_merge_{false};
  bool before_split_{false};
  bool hd_after_split_{false};
  bool hd_after_merge_{false};
  bool sig_ok_{false};
  bool sig_bad_{false};
  bool vset_cur_{false};
  bool vset_next_{false};

  UnixTime gen_utime_{0};
  CatchainSeqno catchain_seqno_{0};
  td::uint32 validator_set_hash_{0};
  BlockSeqno vert_seqno_{~0U};
  td::uint32 sig_count_;
  ValidatorWeight sig_weight_;
  Ref<vm::Cell> sig_root_;
  Ref<BlockSignatureSet> sig_set_;
  std::vector<Ref<vm::Cell>> proof_roots_;
  std::vector<BlockIdExt> chain_blk_ids_;
  std::vector<BlockIdExt> chain_mc_blk_ids_;
  std::vector<BlockIdExt> link_prev_;
  std::vector<std::pair<block::CurrencyCollection, block::CurrencyCollection>> chain_fees_;
  std::vector<td::Bits256> creators_;
  UnixTime first_gen_utime_;

  ShardTopBlockDescrQ(const ShardTopBlockDescrQ& other) = default;
  ShardTopBlockDescrQ(ShardTopBlockDescrQ&& other) = default;

  td::Status unpack_one_proof(BlockIdExt& cur_id, Ref<vm::Cell> proof_root, bool is_head);
  td::Result<int> validate_internal(BlockIdExt last_mc_block_id, Ref<MasterchainState> last_mc_state, int& res_flags,
                                    int mode) const;
};

class ValidateShardTopBlockDescr : public td::actor::Actor {
 public:
  ValidateShardTopBlockDescr(td::BufferSlice data, BlockIdExt masterchain_block, BlockHandle masterchain_handle,
                             Ref<MasterchainState> masterchain_state, td::actor::ActorId<ValidatorManager> manager,
                             td::Timestamp timeout, bool is_fake, td::Promise<Ref<ShardTopBlockDescription>> promise)
      : data_(std::move(data))
      , mc_blkid_(masterchain_block)
      , handle_(std::move(masterchain_handle))
      , state_(std::move(masterchain_state))
      , manager_(manager)
      , timeout_(timeout)
      , is_fake_(is_fake)
      , promise_(std::move(promise)) {
  }

  void finish_query();
  void abort_query(td::Status reason);
  bool fatal_error(td::Status error);
  bool fatal_error(std::string err_msg, int err_code = -666);
  void alarm() override;

  void start_up() override;

 private:
  td::BufferSlice data_;
  Ref<ShardTopBlockDescrQ> descr_;

  BlockIdExt mc_blkid_;
  BlockHandle handle_;
  Ref<MasterchainState> state_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  bool is_fake_;
  td::Promise<Ref<ShardTopBlockDescription>> promise_;
};

}  // namespace validator

}  // namespace ton
