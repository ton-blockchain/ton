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

#include "validator/interfaces/validator-manager.h"
#include "validator/interfaces/shard.h"

namespace ton {

namespace validator {

class ValidateBroadcast : public td::actor::Actor {
 private:
  BlockBroadcast broadcast_;
  BlockHandle last_masterchain_block_handle_;
  td::Ref<MasterchainState> last_masterchain_state_;
  BlockHandle last_known_masterchain_block_handle_;
  td::Ref<Proof> last_known_masterchain_block_proof_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Timestamp timeout_;
  td::Promise<td::Unit> promise_;

  td::Ref<BlockData> data_;
  td::Ref<BlockSignatureSet> sig_set_;
  td::Ref<Proof> proof_;
  td::Ref<ProofLink> proof_link_;
  BlockHandle handle_;

  td::PerfWarningTimer perf_timer_{"validatebroadcast", 0.1};

 public:
  ValidateBroadcast(BlockBroadcast broadcast, BlockHandle last_masterchain_block_handle,
                    td::Ref<MasterchainState> last_masterchain_state, BlockHandle last_known_masterchain_block_handle,
                    td::actor::ActorId<ValidatorManager> manager, td::Timestamp timeout, td::Promise<td::Unit> promise)
      : broadcast_(std::move(broadcast))
      , last_masterchain_block_handle_(std::move(last_masterchain_block_handle))
      , last_masterchain_state_(std::move(last_masterchain_state))
      , last_known_masterchain_block_handle_(std::move(last_known_masterchain_block_handle))
      , manager_(manager)
      , timeout_(timeout)
      , promise_(std::move(promise)) {
  }

  void start_up() override;
  void checked_signatures();
  void got_block_handle(BlockHandle handle);
  void written_block_data();
  void checked_proof();

  void abort_query(td::Status reason);
  void finish_query();
  void alarm() override;
};

}  // namespace validator

}  // namespace ton
