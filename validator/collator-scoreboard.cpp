/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/utils/Random.h"

#include "collator-scoreboard.hpp"

namespace ton::validator {

void CollatorScoreboard::report_outcome(adnl::AdnlNodeIdShort collator, bool success) {
  auto [it, inserted] = scores_.try_emplace(collator, 1.0);
  it->second = it->second * (1 - outcome_weight) + (success ? 1.0 : 0.0) * outcome_weight;
  LOG(INFO) << "Collator " << collator << " " << (success ? "produced" : "missed") << " a delegated window, score is "
            << it->second;
}

void CollatorScoreboard::pick_collator(std::vector<adnl::AdnlNodeIdShort> candidates,
                                       td::Promise<adnl::AdnlNodeIdShort> promise) {
  std::vector<adnl::AdnlNodeIdShort> good;
  for (auto candidate : candidates) {
    auto it = scores_.find(candidate);
    if (it == scores_.end() || it->second >= good_enough_score) {
      good.push_back(candidate);
    }
  }
  if (good.empty()) {
    promise.set_error(td::Status::Error("no good enough collators"));
    return;
  }
  promise.set_value(std::move(good[td::Random::fast(0, static_cast<int>(good.size()) - 1)]));
}

}  // namespace ton::validator
