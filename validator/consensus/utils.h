/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "interfaces/block.h"
#include "td/utils/Status.h"
#include "ton/ton-types.h"

namespace ton::validator::consensus {

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate);
td::Result<std::pair<td::Ref<vm::Cell>, td::Ref<BlockData>>> apply_block_to_state(
    const std::vector<td::Ref<vm::Cell>>& state_roots, const BlockCandidate& candidate);
td::Result<bool> get_before_split(const td::Ref<BlockData>& block);

}  // namespace ton::validator::consensus
