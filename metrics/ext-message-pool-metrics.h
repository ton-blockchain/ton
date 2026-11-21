/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <array>
#include <string_view>

#include "collectors.h"

namespace ton::metrics {

enum class ExtMessageAdmissionOutcome : size_t {
  accepted,
  not_ready,
  too_large,
  backpressure,
  invalid,
  state_unavailable,
  vm_rejected,
  rate_limited,
  pool_full,
  address_full,
  duplicate,
  internal_error,
  reprioritized,
  count
};

enum class ExtMessageRemovalReason : size_t { applied, expired, rejected_final, filtered, pool_pressure, count };

struct ExtMessagePoolSnapshot {
  td::uint64 pending_ext_messages{0};
  double oldest_ext_message_age_seconds{0.0};
  td::uint64 check_ok{0};
  td::uint64 check_error{0};
  std::array<td::uint64, static_cast<size_t>(ExtMessageAdmissionOutcome::count)> admission{};
  std::array<td::uint64, static_cast<size_t>(ExtMessageRemovalReason::count)> removed{};
  td::uint64 applied_master{0};
  td::uint64 applied_shard{0};

  void collect(Context ctx) const {
    auto mempool = ctx.with_name("mempool");
    auto pending = mempool.with_name("ext_messages");
    pending.open_family("gauge");
    pending.push(double(pending_ext_messages));

    auto oldest = mempool.with_name("oldest_ext_message_age_seconds");
    oldest.open_family("gauge");
    oldest.push(oldest_ext_message_age_seconds);

    auto check = mempool.with_name("ext_check");
    check.open_family("counter", "total");
    check.with_label("result", "ok").push(double(check_ok));
    check.with_label("result", "error").push(double(check_error));

    auto admission_family = mempool.with_name("ext_admission");
    admission_family.open_family("counter", "total");
    for (size_t i = 0; i < admission.size(); ++i) {
      admission_family.with_label("outcome", admission_outcome_names_[i]).push(double(admission[i]));
    }

    auto removed_family = mempool.with_name("ext_removed");
    removed_family.open_family("counter", "total");
    for (size_t i = 0; i < removed.size(); ++i) {
      removed_family.with_label("reason", removal_reason_names_[i]).push(double(removed[i]));
    }

    auto applied = ctx.with_name("applied_ext_messages");
    applied.open_family("counter", "total");
    applied.with_label("chain", "master").push(double(applied_master));
    applied.with_label("chain", "shard").push(double(applied_shard));
  }

 private:
  static constexpr auto admission_outcome_names_ = std::to_array<std::string_view>({
      "accepted",
      "not_ready",
      "too_large",
      "backpressure",
      "invalid",
      "state_unavailable",
      "vm_rejected",
      "rate_limited",
      "pool_full",
      "address_full",
      "duplicate",
      "internal_error",
      "reprioritized",
  });
  static_assert(admission_outcome_names_.size() == static_cast<size_t>(ExtMessageAdmissionOutcome::count));

  static constexpr auto removal_reason_names_ = std::to_array<std::string_view>({
      "applied",
      "expired",
      "rejected_final",
      "filtered",
      "pool_pressure",
  });
  static_assert(removal_reason_names_.size() == static_cast<size_t>(ExtMessageRemovalReason::count));
};

}  // namespace ton::metrics
