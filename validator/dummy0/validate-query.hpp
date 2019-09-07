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
#pragma once

#include "interfaces/validator-manager.h"

namespace ton {

namespace validator {

namespace dummy0 {

class ValidateQuery : public td::actor::Actor {
 public:
  ValidateQuery(ShardIdFull shard, UnixTime min_ts, BlockIdExt min_masterchain_block_id, std::vector<BlockIdExt> prev,
                BlockCandidate candidate, CatchainSeqno catchain_seqno, td::uint32 validator_set_hash,
                td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
                td::Promise<ValidateCandidateResult> promise);

  void abort_query(td::Status reason);
  void reject_query(std::string reason, td::BufferSlice proof);
  void finish_query();
  void alarm() override;

  void start_up() override;
  void got_prev_state(td::Ref<ShardState> state);
  void got_masterchain_handle(BlockHandle masterchain_handle);
  void got_masterchain_state(td::Ref<ShardState> masterchain_state);
  void written_candidate();

 private:
  ShardIdFull shard_;
  UnixTime min_ts_;
  BlockIdExt min_masterchain_block_id_;
  std::vector<BlockIdExt> prev_;
  BlockCandidate candidate_;
  CatchainSeqno catchain_seqno_;
  td::uint32 validator_set_hash_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<ValidateCandidateResult> promise_;

  UnixTime block_ts_;
  tl_object_ptr<ton_api::test0_shardchain_block> unserialized_block_;
};

}  // namespace dummy0

}  // namespace validator

}  // namespace ton

