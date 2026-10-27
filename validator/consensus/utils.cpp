/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "vm/boc.h"
#include "vm/cells/MerkleUpdate.h"

#include "block-auto.h"
#include "fabric.h"
#include "utils.h"

namespace ton::validator::consensus {

td::Result<double> get_candidate_gen_utime_exact(const BlockCandidate& candidate, CandidateRootsRef roots) {
  std::vector<td::Ref<vm::Cell>> decoded_roots;
  if (roots.is_null()) {
    TRY_RESULT_ASSIGN(decoded_roots,
                      vm::std_boc_deserialize_multi(candidate.collated_data, vm::BagOfCells::default_max_roots, true));
  }
  const auto& cdata_roots = roots.not_null() ? roots->collated_roots : decoded_roots;
  for (const td::Ref<vm::Cell>& root : cdata_roots) {
    if (!block::gen::t_ConsensusExtraData.validate_ref(10000, root)) {
      continue;
    }
    block::gen::ConsensusExtraData::Record rec;
    CHECK(block::gen::unpack_cell(root, rec));
    return (double)rec.gen_utime_ms / 1000.0;
  }
  return td::Status::Error("no ConsensusExtraData in candidate");
}

}  // namespace ton::validator::consensus
