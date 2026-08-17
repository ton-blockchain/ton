/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "td/utils/Time.h"
#include "td/utils/int_types.h"

#include "collectors.h"

namespace ton::metrics {

namespace detail {

double estimate_ticks_per_second(double elapsed_seconds, td::uint64 begin_ticks, td::uint64 current_ticks);

}  // namespace detail

// Actor-framework observability for the scheduler group hosting the exporter. Actor and worker
// numerators therefore have the same scope as their scheduler thread denominators.
class ActorMetrics {
 public:
  ActorMetrics();

  void collect(Context ctx) const;

 private:
  // rdtsc_frequency() is hardcoded on x86, so calibrate it against monotonic time.
  double estimate_ticks_per_second() const;

  td::Timestamp begin_ts_;
  td::uint64 begin_ticks_{};
};

}  // namespace ton::metrics
