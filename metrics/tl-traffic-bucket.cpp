/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/ton_api.h"
#include "td/utils/as.h"

#include "tl-traffic-bucket.h"

namespace ton::metrics {

namespace {

std::optional<std::string> nameof(td::int32 magic) {
  if (auto name = ton_api::Object::nameof(magic)) {
    return name;
  }
  return ton_api::Function::nameof(magic);
}

// Bounds-checked walk over a TL payload: a read that would run past the end reports failure instead
// of advancing, and the caller then keeps the last constructor it did identify.
class Cursor {
 public:
  explicit Cursor(td::Slice data) : data_(data) {
  }

  bool skip(size_t n) {
    if (data_.size() < n) {
      return false;
    }
    data_.remove_prefix(n);
    return true;
  }

  bool peek_int(td::int32 &out) const {
    if (data_.size() < sizeof(td::int32)) {
      return false;
    }
    out = td::as<td::int32>(data_.data());
    return true;
  }

  bool fetch_int(td::int32 &out) {
    return peek_int(out) && skip(sizeof(td::int32));
  }

  // `bytes` = 1-byte length (< 254), or 0xFE plus a 3-byte little-endian length, then the content
  // zero-padded so that header + content is a multiple of 4.
  bool skip_bytes(bool with_content = true) {
    if (data_.empty()) {
      return false;
    }
    size_t header = 1, len = static_cast<td::uint8>(data_[0]);
    if (len == 254) {
      if (data_.size() < 4) {
        return false;
      }
      header = 4;
      len = static_cast<td::uint8>(data_[1]) | static_cast<size_t>(static_cast<td::uint8>(data_[2])) << 8 |
            static_cast<size_t>(static_cast<td::uint8>(data_[3])) << 16;
    } else if (len > 254) {
      return false;
    }
    return skip(header) && (!with_content || skip((header + len + 3) / 4 * 4 - header));
  }

 private:
  td::Slice data_;
};

bool skip_public_key(Cursor &cur) {
  td::int32 magic = 0;
  if (!cur.fetch_int(magic)) {
    return false;
  }
  switch (magic) {
    case ton_api::pub_ed25519::ID:
    case ton_api::pub_aes::ID:
      return cur.skip(32);
    case ton_api::pub_unenc::ID:
    case ton_api::pub_overlay::ID:
      return cur.skip_bytes();
    default:
      return false;
  }
}

bool skip_certificate(Cursor &cur) {
  td::int32 magic = 0;
  if (!cur.fetch_int(magic)) {
    return false;
  }
  switch (magic) {
    case ton_api::overlay_emptyCertificate::ID:
      return true;
    case ton_api::overlay_certificate::ID:  // expire_at:int max_size:int
      return skip_public_key(cur) && cur.skip(8) && cur.skip_bytes();
    case ton_api::overlay_certificateV2::ID:  // expire_at:int max_size:int flags:int
      return skip_public_key(cur) && cur.skip(12) && cur.skip_bytes();
    default:
      return false;
  }
}

bool skip_message_extra(Cursor &cur) {
  td::int32 flags = 0, magic = 0;
  if (!cur.fetch_int(flags)) {
    return false;
  }
  if (!(flags & 1)) {
    return true;
  }
  if (!cur.fetch_int(magic)) {
    return false;
  }
  switch (magic) {
    case ton_api::overlay_emptyMemberCertificate::ID:
      return true;
    case ton_api::overlay_memberCertificate::ID:  // flags:int slot:int expire_at:int
      return skip_public_key(cur) && cur.skip(12) && cur.skip_bytes();
    default:
      return false;
  }
}

// Skips one routing envelope, leaving the cursor on the payload it wraps. False means `magic` is not
// an envelope, or its header is malformed — either way the label stays `magic`.
bool skip_envelope(Cursor &cur, td::int32 magic) {
  constexpr size_t kIdSize = sizeof(td::int32) + 32;  // magic + overlay:int256
  switch (magic) {
    case ton_api::overlay_query::ID:
    case ton_api::overlay_message::ID:
      return cur.skip(kIdSize);
    case ton_api::tonNode_query::ID:
      return cur.skip(sizeof(td::int32));
    case ton_api::overlay_unicast::ID:
      return cur.skip(sizeof(td::int32)) && cur.skip_bytes(false);
    case ton_api::overlay_queryWithExtra::ID:
    case ton_api::overlay_messageWithExtra::ID:
      return cur.skip(kIdSize) && skip_message_extra(cur);
    case ton_api::overlay_broadcast::ID:  // src, certificate, flags:int, then data:bytes
      return cur.skip(sizeof(td::int32)) && skip_public_key(cur) && skip_certificate(cur) &&
             cur.skip(sizeof(td::int32)) && cur.skip_bytes(false);
    default:
      return false;
  }
}

// The magic that says what this traffic is: the outermost one is usually just routing.
td::int32 resolve_magic(td::Slice payload) {
  Cursor cur(payload);
  td::int32 magic = 0;
  cur.peek_int(magic);
  for (int depth = 0; depth < 4; ++depth) {
    td::int32 inner = 0;
    if (!skip_envelope(cur, magic) || !cur.peek_int(inner)) {
      break;
    }
    magic = inner;
  }
  return magic;
}

}  // namespace

td::int32 resolve_tl_magic(td::Slice payload) {
  if (payload.size() < sizeof(td::int32)) {
    return 0;
  }
  return resolve_magic(payload);
}

std::string tl_name(td::int32 magic) {
  if (auto name = nameof(magic)) {
    return *name;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%08x", static_cast<td::uint32>(magic));
  return buf;
}

std::string tl_name(td::Slice payload) {
  if (payload.size() < sizeof(td::int32)) {
    return "unknown";
  }
  return tl_name(resolve_tl_magic(payload));
}

void TlTrafficBucket::account(td::Slice payload) {
  if (payload.size() < sizeof(td::int32)) {
    unknown_.bytes += payload.size();
    unknown_.messages++;
    return;
  }
  account(resolve_magic(payload), payload.size());
}

void TlTrafficBucket::account(td::int32 magic, td::uint64 size) {
  if (!nameof(magic).has_value()) {
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
      bytes.with_label("tl", *nameof(magic)).push(static_cast<double>(cell.bytes));
    }
    bytes.with_label("tl", "unknown").push(static_cast<double>(unknown_.bytes));
  }
  {
    auto messages = ctx.with_name("messages");
    messages.open_family("counter", "total");
    for (const auto &[magic, cell] : known_) {
      messages.with_label("tl", *nameof(magic)).push(static_cast<double>(cell.messages));
    }
    messages.with_label("tl", "unknown").push(static_cast<double>(unknown_.messages));
  }
}

void TlLatencyBucket::observe(td::int32 magic, double seconds, bool ok) {
  Cell &cell = nameof(magic).has_value() ? known_[magic] : unknown_;
  cell.duration.observe(seconds);
  if (!ok) {
    cell.failed.inc();
  }
}

void TlLatencyBucket::collect(Context ctx) const {
  // Every cell re-emits the same two families into the same slots, so each one rewinds first.
  auto emit = [](Context cell_ctx, const Cell &cell) {
    cell_ctx.collect(cell.duration, "duration_seconds");
    cell_ctx.collect(cell.failed, "failed");
  };
  size_t start = ctx.mark();
  for (const auto &[magic, cell] : known_) {
    ctx.rewind(start);
    emit(ctx.with_label("tl", *nameof(magic)), cell);
  }
  if (unknown_.duration.count() != 0) {
    ctx.rewind(start);
    emit(ctx.with_label("tl", "unknown"), unknown_);
  }
}

}  // namespace ton::metrics
