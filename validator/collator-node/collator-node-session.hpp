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

#include "interfaces/validator-manager.h"
#include "rldp/rldp.h"
#include "rldp2/rldp.h"
#include <map>
#include <optional>

namespace ton::validator {

class ValidatorManager;

class CollatorNodeSession : public td::actor::Actor {
 public:
  CollatorNodeSession(ShardIdFull shard, std::vector<BlockIdExt> prev, td::Ref<ValidatorSet> validator_set,
                      BlockIdExt min_masterchain_block_id, bool can_generate, adnl::AdnlNodeIdShort local_id,
                      td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                      td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp2::Rldp> rldp);

  void start_up() override;
  void tear_down() override;

  void update_options(td::Ref<ValidatorManagerOptions> opts) {
    opts_ = std::move(opts);
  }

  void new_shard_block_accepted(BlockIdExt block_id, bool can_generate);

  void generate_block(std::vector<BlockIdExt> prev_blocks, td::optional<BlockCandidatePriority> o_priority,
                      td::Timestamp timeout, td::Promise<BlockCandidate> promise);

 private:
  ShardIdFull shard_;
  std::vector<BlockIdExt> prev_;
  td::Ref<ValidatorSet> validator_set_;
  BlockIdExt min_masterchain_block_id_;
  bool can_generate_;
  adnl::AdnlNodeIdShort local_id_;
  td::Ref<ValidatorManagerOptions> opts_;
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp2::Rldp> rldp_;

  struct CacheEntry {
    bool started = false;
    td::Timestamp has_internal_query_at;
    td::Timestamp has_external_query_at;
    td::Timestamp has_result_at;
    BlockSeqno block_seqno = 0;
    td::optional<BlockCandidate> result;
    td::CancellationTokenSource cancellation_token_source;
    std::vector<td::Promise<BlockCandidate>> promises;

    void cancel(td::Status reason);
  };

  BlockSeqno next_block_seqno_;
  std::map<std::vector<BlockIdExt>, std::shared_ptr<CacheEntry>> cache_;

  void process_result(std::shared_ptr<CacheEntry> cache_entry, td::Result<BlockCandidate> R);
};

}  // namespace ton::validator
