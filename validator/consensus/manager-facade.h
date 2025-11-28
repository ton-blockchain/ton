/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "validator/fabric.h"

namespace ton::validator::consensus {

class ManagerFacade : public td::actor::Actor {
 public:
  virtual td::actor::Task<GeneratedCandidate> collate_block(ShardIdFull shard, BlockIdExt min_masterchain_block_id,
                                                            std::vector<BlockIdExt> prev, Ed25519_PublicKey creator,
                                                            BlockCandidatePriority priority, td::uint64 max_answer_size,
                                                            td::CancellationToken cancellation_token) = 0;

  virtual td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate,
                                                                            ValidateParams params,
                                                                            td::Timestamp timeout) = 0;

  virtual td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                         td::Ref<block::BlockSignatureSet> signatures, int send_broadcast_mode,
                                         bool apply) = 0;

  virtual void log_validator_session_stats(validatorsession::ValidatorSessionStats stats) {
  }
};

}  // namespace ton::validator::consensus
