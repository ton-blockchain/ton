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

#include "validator/interfaces/validator-manager.h"

namespace ton {

namespace validator {

class CollateQuery : public td::actor::Actor {
 public:
  CollateQuery(ShardIdFull shard, td::uint32 min_ts, BlockIdExt min_masterchain_block_id, std::vector<BlockIdExt> prev,
               td::Ref<ValidatorSet> validator_set, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
               td::Promise<BlockCandidate> promise);
  CollateQuery(ShardIdFull shard, td::uint32 min_ts, BlockIdExt min_masterchain_block_id, ZeroStateIdExt zero_state_id,
               td::Ref<ValidatorSet> validator_set, td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout,
               td::Promise<BlockCandidate> promise);

  void alarm() override;

  void abort_query(td::Status reason);
  void finish_query();

  void start_up() override;
  void got_prev_state(td::Ref<MasterchainState> state);
  void written_block_data();
  void written_block_collated_data();

 private:
  ShardIdFull shard_;
  UnixTime min_ts_;
  BlockIdExt min_masterchain_block_id_;
  std::vector<BlockIdExt> prev_;
  ZeroStateIdExt zero_state_id_;
  td::Ref<ValidatorSet> validator_set_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<BlockCandidate> promise_;

  BlockCandidate candidate_;
  UnixTime ts_;
};

}  // namespace validator

}  // namespace ton
