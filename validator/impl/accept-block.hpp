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

#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "ton/ton-shard.h"
#include "interfaces/validator-manager.h"
#include "validator-set.hpp"
#include "signature-set.hpp"
#include "shard.hpp"

namespace ton {

namespace validator {
using td::Ref;

/*
 *
 * block data (if not given) can be obtained from:
 *   db as part of collated block
 *   db as block
 *   net
 * must write block data, block signatures and block state
 * initialize prev, before_split, after_merge
 * for masterchain write block proof and set next for prev block
 * for masterchain run new_block callback
 *
 */

class AcceptBlockQuery : public td::actor::Actor {
 public:
  struct IsFake {};
  struct ForceFork {};
  AcceptBlockQuery(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                   td::Ref<ValidatorSet> validator_set, td::Ref<BlockSignatureSet> signatures,
                   td::Ref<BlockSignatureSet> approve_signatures, bool send_broadcast,
                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise);
  AcceptBlockQuery(IsFake fake, BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                   td::Ref<ValidatorSet> validator_set, td::actor::ActorId<ValidatorManager> manager,
                   td::Promise<td::Unit> promise);
  AcceptBlockQuery(ForceFork ffork, BlockIdExt id, td::Ref<BlockData> data,
                   td::actor::ActorId<ValidatorManager> manager, td::Promise<td::Unit> promise);

 private:
  static constexpr td::uint32 priority() {
    return 2;
  }

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void written_block_data();
  void written_block_signatures();
  void got_block_handle(BlockHandle handle);
  void written_block_info();
  void got_block_data(td::Ref<BlockData> data);
  void got_prev_state(td::Ref<ShardState> state);
  void written_state(td::Ref<ShardState> state);
  void written_block_proof();
  void got_last_mc_block(std::pair<td::Ref<MasterchainState>, BlockIdExt> last);
  void got_mc_state(Ref<ShardState> res);
  void find_known_ancestors();
  void require_proof_link(BlockIdExt id);
  void got_proof_link(BlockIdExt id, Ref<ProofLink> proof);
  bool create_top_shard_block_description();
  void create_topshard_blk_descr();
  void top_block_descr_validated(td::Result<Ref<ShardTopBlockDescription>> R);
  void written_block_next();
  void written_block_info_2();
  void applied();

 private:
  BlockIdExt id_;
  Ref<BlockData> data_;
  std::vector<BlockIdExt> prev_;
  Ref<ValidatorSetQ> validator_set_;
  Ref<BlockSignatureSetQ> signatures_;
  Ref<BlockSignatureSetQ> approve_signatures_;
  bool is_fake_;
  bool is_fork_;
  bool send_broadcast_;
  bool ancestors_split_{false}, is_key_block_{false};
  td::Timestamp timeout_ = td::Timestamp::in(600.0);
  td::actor::ActorId<ValidatorManager> manager_;
  td::Promise<td::Unit> promise_;

  FileHash signatures_hash_;
  BlockHandle handle_;
  Ref<Proof> proof_;
  Ref<ProofLink> proof_link_;

  Ref<ShardState> state_;
  Ref<vm::Cell> block_root_;
  LogicalTime lt_;
  UnixTime created_at_;
  RootHash state_keep_old_hash_, state_old_hash_, state_hash_;
  BlockIdExt mc_blkid_, prev_mc_blkid_;

  Ref<MasterchainStateQ> last_mc_state_;
  BlockIdExt last_mc_id_;
  std::vector<Ref<block::McShardHash>> ancestors_;
  BlockSeqno ancestors_seqno_;
  std::vector<Ref<ProofLink>> proof_links_;
  std::vector<Ref<vm::Cell>> proof_roots_;
  std::vector<BlockIdExt> link_prev_;
  Ref<vm::Cell> signatures_cell_;
  td::BufferSlice top_block_descr_data_;
  Ref<ShardTopBlockDescription> top_block_descr_;

  td::PerfWarningTimer perf_timer_;

  bool fatal_error(std::string msg, int code = -666);
  static bool check_send_error(td::actor::ActorId<AcceptBlockQuery> SelfId, td::Status error);
  template <typename T>
  static bool check_send_error(td::actor::ActorId<AcceptBlockQuery> SelfId, td::Result<T>& res) {
    return res.is_error() && check_send_error(std::move(SelfId), res.move_as_error());
  }
  bool precheck_header();
  bool create_new_proof();
  bool unpack_proof_link(BlockIdExt id, Ref<ProofLink> proof);

  bool is_masterchain() const {
    return id_.id.is_masterchain();
  }
};

}  // namespace validator

}  // namespace ton
