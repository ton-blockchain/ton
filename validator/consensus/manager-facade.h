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
  virtual td::actor::Task<GeneratedCandidate> collate_block(CollateParams params,
                                                            td::CancellationToken cancellation_token) = 0;

  virtual td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate candidate,
                                                                            ValidateParams params,
                                                                            td::Timestamp timeout) = 0;

  virtual td::actor::Task<> accept_block(BlockIdExt id, td::Ref<BlockData> data, std::vector<BlockIdExt> prev,
                                         size_t creator_idx, td::Ref<block::BlockSignatureSet> signatures,
                                         int send_broadcast_mode, bool apply) = 0;

  virtual td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt block_id, td::Timestamp timeout) = 0;
  virtual td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt block_id, td::Timestamp timeout) = 0;

  virtual td::actor::Task<BlockCandidate> load_block_candidate(PublicKey source, BlockIdExt block_id,
                                                               FileHash collated_data_hash) = 0;
  virtual td::actor::Task<> store_block_candidate(BlockCandidate candidate) = 0;

  virtual void log_validator_session_stats(validatorsession::ValidatorSessionStats stats) {
  }
};

}  // namespace ton::validator::consensus
