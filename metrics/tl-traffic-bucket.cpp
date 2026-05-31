/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/ton_api.h"
#include "td/utils/as.h"

#include "tl-traffic-bucket.h"

namespace ton::metrics {

void TlTrafficBucket::account(td::Slice payload) {
  if (payload.size() < sizeof(td::int32)) {
    unknown_.bytes += payload.size();
    unknown_.messages++;
    return;
  }
  account(td::as<td::int32>(payload.data()), payload.size());
}

void TlTrafficBucket::account(td::int32 magic, td::uint64 size) {
  if (!ton_api::Object::nameof(magic).has_value()) {
    unknown_.bytes += size;
    unknown_.messages++;
    return;
  }
  auto &cell = known_[magic];
  cell.bytes += size;
  cell.messages++;
}

TlTrafficBucket &TlTrafficBucket::operator+=(const TlTrafficBucket &other) {
  for (const auto &[magic, cell] : other.known_) {
    auto &dst = known_[magic];
    dst.bytes += cell.bytes;
    dst.messages += cell.messages;
  }
  unknown_.bytes += other.unknown_.bytes;
  unknown_.messages += other.unknown_.messages;
  return *this;
}

void TlTrafficBucket::collect(Context ctx) const {
  {
    auto bytes = ctx.with_name("bytes");
    bytes.open_family("counter", "total");
    for (const auto &[magic, cell] : known_) {
      bytes.with_label("tl", *ton_api::Object::nameof(magic)).push(static_cast<double>(cell.bytes));
    }
    bytes.with_label("tl", "unknown").push(static_cast<double>(unknown_.bytes));
  }
  {
    auto messages = ctx.with_name("messages");
    messages.open_family("counter", "total");
    for (const auto &[magic, cell] : known_) {
      messages.with_label("tl", *ton_api::Object::nameof(magic)).push(static_cast<double>(cell.messages));
    }
    messages.with_label("tl", "unknown").push(static_cast<double>(unknown_.messages));
  }
}

}  // namespace ton::metrics
