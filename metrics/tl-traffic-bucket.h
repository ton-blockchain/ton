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

// The magic a payload's traffic should be attributed to: the outermost constructor is usually a
// routing envelope (overlay.query, overlay.message, ...), so it is unwrapped down to the content.
// Returns 0 for payloads shorter than a magic.
td::int32 resolve_tl_magic(td::Slice payload);

// For logs: the TL constructor name a payload resolves to ("consensus.simplex.vote"), unwrapping
// routing envelopes the same way traffic accounting does; hex when the schema doesn't know it.
std::string tl_name(td::Slice payload);
std::string tl_name(td::int32 magic);

// Accounts traffic by inner TL constructor. The payload's leading magic is usually a routing
// envelope (overlay.query, overlay.message, ...), so it is unwrapped down to the content it carries;
// a malformed envelope falls back to the envelope itself, never to whatever bytes follow. Schema-known
// magics get their own `tl` cell, while unknown or short payloads funnel into a single "unknown" cell
// so label cardinality stays bounded by the schema.
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
