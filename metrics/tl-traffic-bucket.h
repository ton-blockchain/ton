/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <map>

#include "td/utils/Slice.h"
#include "td/utils/int_types.h"

#include "collectors.h"

namespace ton::metrics {

// Accounts traffic by inner TL constructor. A payload's first 4 bytes are its constructor magic;
// schema-known magics get their own `tl` cell (named via ton_api::Object::nameof), while unknown or
// short payloads funnel into a single "unknown" cell so label cardinality stays bounded by the schema.
class TlTrafficBucket {
 public:
  void account(td::Slice payload);
  void account(td::int32 magic, td::uint64 size);

  TlTrafficBucket &operator+=(const TlTrafficBucket &other);

  void collect(Context ctx) const;

 private:
  struct Cell {
    td::uint64 bytes = 0;
    td::uint64 messages = 0;
  };
  std::map<td::int32, Cell> known_;
  Cell unknown_;
};

}  // namespace ton::metrics
