/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <map>

#include "adnl/adnl-node-id.hpp"
#include "td/actor/actor.h"

namespace ton::validator {

class CollatorScoreboard : public td::actor::Actor {
 public:
  void report_outcome(adnl::AdnlNodeIdShort collator, bool success);
  void pick_collator(std::vector<adnl::AdnlNodeIdShort> candidates, td::Promise<adnl::AdnlNodeIdShort> promise);

 private:
  static constexpr double outcome_weight = 0.25;
  static constexpr double good_enough_score = 0.5;

  std::map<adnl::AdnlNodeIdShort, double> scores_;
};

}  // namespace ton::validator
