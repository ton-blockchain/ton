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
  void report_outcome(adnl::AdnlNodeIdShort collator_id, bool success);
  void pick_collator(std::vector<adnl::AdnlNodeIdShort> candidates, td::Promise<adnl::AdnlNodeIdShort> promise);

 private:
  static constexpr double MIN_BAN_DURATION = 60.0;
  static constexpr double MAX_BAN_DURATION = 3600.0;
  static constexpr double BAN_DURATION_MULTIPLIER = 1.5;

  struct Collator {
    td::Timestamp ban_until = td::Timestamp::never();
    double ban_duration = MIN_BAN_DURATION;
  };
  std::map<adnl::AdnlNodeIdShort, Collator> collators_;

  bool is_banned(adnl::AdnlNodeIdShort collator_id);
};

}  // namespace ton::validator
