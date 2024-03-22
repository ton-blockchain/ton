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
*/
#pragma once
#include "ton/ton-types.h"
#include "td/actor/actor.h"
#include "interfaces/validator-manager.h"

namespace ton::validator {

class CandidatesBuffer : public td::actor::Actor {
 public:
  explicit CandidatesBuffer(td::actor::ActorId<ValidatorManager> manager) : manager_(std::move(manager)) {
  }

  void start_up() override;
  void alarm() override;

  void add_new_candidate(BlockIdExt id, PublicKey source, FileHash collated_data_file_hash);
  void get_block_data(BlockIdExt id, td::Promise<td::Ref<BlockData>> promise);
  void get_block_state(BlockIdExt id, td::Promise<td::Ref<ShardState>> promise);

 private:
  td::actor::ActorId<ValidatorManager> manager_;

  struct Candidate {
    PublicKey source_;
    FileHash collated_data_file_hash_;
    td::Timestamp ttl_;

    td::Ref<BlockData> data_;
    std::vector<td::Promise<td::Ref<BlockData>>> data_waiters_;
    bool data_requested_{false};

    td::Ref<ShardState> state_;
    std::vector<td::Promise<td::Ref<ShardState>>> state_waiters_;
    bool state_requested_{false};
  };
  std::map<BlockIdExt, Candidate> candidates_;

  void got_block_candidate(BlockIdExt id, td::Result<BlockCandidate> R);

  void get_block_state_cont(BlockIdExt id, td::Ref<BlockData> data);
  void get_block_state_cont2(td::Ref<BlockData> block, std::vector<BlockIdExt> prev,
                             std::vector<td::Ref<ShardState>> prev_states);

  void finish_get_block_data(BlockIdExt id, td::Result<td::Ref<BlockData>> res);
  void finish_get_block_state(BlockIdExt id, td::Result<td::Ref<ShardState>> res);
};

}  // namespace ton::validator
