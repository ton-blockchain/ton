/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/utils/Random.h"

#include "collator-scoreboard.hpp"

namespace ton::validator {

void CollatorScoreboard::report_outcome(adnl::AdnlNodeIdShort collator_id, bool success) {
  Collator& collator = collators_[collator_id];
  if (success) {
    collator.ban_duration = std::max(MIN_BAN_DURATION, collator.ban_duration / BAN_DURATION_MULTIPLIER);
    LOG(INFO) << "Collator " << collator_id << " produced a delegation window";
  } else {
    collator.ban_until = td::Timestamp::in(collator.ban_duration);
    LOG(INFO) << "Collator " << collator_id << " missed a delegation window, banning it for " << collator.ban_duration
              << " s";
    collator.ban_duration = std::min(MAX_BAN_DURATION, collator.ban_duration * BAN_DURATION_MULTIPLIER);
  }
}

void CollatorScoreboard::pick_collator(std::vector<adnl::AdnlNodeIdShort> candidates,
                                       td::Promise<adnl::AdnlNodeIdShort> promise) {
  std::vector<adnl::AdnlNodeIdShort> good;
  for (auto candidate : candidates) {
    if (!is_banned(candidate)) {
      good.push_back(candidate);
    }
  }
  if (good.empty()) {
    promise.set_error(td::Status::Error("no good collators"));
    return;
  }
  promise.set_value(std::move(good[td::Random::fast(0, static_cast<int>(good.size()) - 1)]));
}

bool CollatorScoreboard::is_banned(adnl::AdnlNodeIdShort collator_id) {
  Collator& collator = collators_[collator_id];
  return collator.ban_until && !collator.ban_until.is_in_past();
}

}  // namespace ton::validator
