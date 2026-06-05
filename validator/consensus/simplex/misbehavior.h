/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "td/utils/buffer.h"
#include "validator/consensus/misbehavior.h"

namespace ton::validator::consensus::simplex {

class ConflictingVotes : public Misbehavior {
 public:
  static MisbehaviorRef create(td::BufferSlice vote1, td::BufferSlice vote2) {
    return td::make_ref<ConflictingVotes>(std::move(vote1), std::move(vote2));
  }

  ConflictingVotes(td::BufferSlice vote1, td::BufferSlice vote2) : vote1_(std::move(vote1)), vote2_(std::move(vote2)) {
  }

 private:
  td::BufferSlice vote1_;
  td::BufferSlice vote2_;
};

class ConflictingCandidateAndCertificate : public Misbehavior {
 public:
  static MisbehaviorRef create() {
    return td::make_ref<ConflictingCandidateAndCertificate>();
  }

  ConflictingCandidateAndCertificate() {
  }
};

}  // namespace ton::validator::consensus::simplex
