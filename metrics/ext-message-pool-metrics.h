/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "collectors.h"

namespace ton::metrics {

struct ExtMessagePoolSnapshot {
  td::uint64 pending_ext_messages{0};
  td::uint64 check_ok{0};
  td::uint64 check_error{0};

  void collect(Context ctx) const {
    auto pending = ctx.with_name("ext_messages");
    pending.open_family("gauge");
    pending.push(double(pending_ext_messages));

    auto check = ctx.with_name("ext_check");
    check.open_family("counter", "total");
    check.with_label("result", "ok").push(double(check_ok));
    check.with_label("result", "error").push(double(check_error));
  }
};

}  // namespace ton::metrics
