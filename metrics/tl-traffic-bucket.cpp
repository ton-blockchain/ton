/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "td/utils/as.h"

#include "tl-traffic-bucket.h"

namespace ton::metrics {

namespace {

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

  // Skips a whole `bytes` field: a 1-byte length (< 254), or 0xFE plus a 3-byte little-endian
  // length, then the content zero-padded so that header + content is a multiple of 4.
  bool skip_bytes() {
    return walk_bytes(true);
  }

  // Steps into a `bytes` field instead, leaving the cursor on the content for the caller to read as
  // a nested object — so the declared content must really be there and be at least a magic wide, or
  // the caller would read padding or the field that follows. The cursor is narrowed to exactly the
  // declared content, so nothing the caller reads afterwards can come from outside it.
  bool enter_bytes() {
    return walk_bytes(false);
  }

 private:
  bool walk_bytes(bool with_content) {
    if (data_.empty()) {
      return false;
    }
    size_t header = 1;
    td::uint64 len = static_cast<td::uint8>(data_[0]);
    if (len == 254) {
      if (data_.size() < 4) {
        return false;
      }
      header = 4;
      len = static_cast<td::uint8>(data_[1]) | static_cast<td::uint64>(static_cast<td::uint8>(data_[2])) << 8 |
            static_cast<td::uint64>(static_cast<td::uint8>(data_[3])) << 16;
    } else if (len == 255) {  // 8-byte header with a 7-byte length, as TlParser accepts
      if (data_.size() < 8) {
        return false;
      }
      header = 8;
      len = 0;
      for (int i = 1; i < 8; i++) {
        len |= static_cast<td::uint64>(static_cast<td::uint8>(data_[i])) << (8 * (i - 1));
      }
    }
    if (len > data_.size()) {
      return false;
    }
    if (with_content) {
      return skip((header + static_cast<size_t>(len) + 3) / 4 * 4);
    }
    // The declared content must be present in full, padded to the 4-byte boundary a well-formed
    // field always carries, and wide enough to hold the magic the caller is about to read.
    auto padded = (header + static_cast<size_t>(len) + 3) / 4 * 4;
    if (len < sizeof(td::int32) || padded > data_.size() || !skip(header)) {
      return false;
    }
    // Confine the cursor to the content: the fields that follow it are the peer's to choose too, and
    // reading into them would let a short `data` steer the label.
    data_.truncate(static_cast<size_t>(len));
    return true;
  }

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
      return cur.skip(sizeof(td::int32)) && cur.enter_bytes();
    case ton_api::overlay_queryWithExtra::ID:
    case ton_api::overlay_messageWithExtra::ID:
      return cur.skip(kIdSize) && skip_message_extra(cur);
    case ton_api::overlay_broadcast::ID:  // src, certificate, flags:int, then data:bytes
      return cur.skip(sizeof(td::int32)) && skip_public_key(cur) && skip_certificate(cur) &&
             cur.skip(sizeof(td::int32)) && cur.enter_bytes();
    case ton_api::overlay_broadcastPlumtreeSimple::ID:  // flags:int timestamp:double, src, certificate,
      return cur.skip(sizeof(td::int32) + 4 + 8) && skip_public_key(cur) && skip_certificate(cur) && cur.skip(36) &&
             cur.enter_bytes();                        // broadcast_id:int256 tree_index:int, then data:bytes
    case ton_api::overlay_broadcastTwostepSimple::ID:  // flags:int date:int, src, src_adnl_id:int256,
      return cur.skip(sizeof(td::int32) + 4 + 4) && skip_public_key(cur) && cur.skip(32) && skip_certificate(cur) &&
             cur.enter_bytes();  // certificate, then data:bytes
    // Liteserver queries arrive wrapped the way lite-client's get_query_info() unwraps them: either
    // a liteServer.query carrying the real query in `data`, or a bare liteServer.queryPrefix, then
    // an optional liteServer.waitMasterchainSeqno prefix before the lite_api function itself.
    case lite_api::liteServer_query::ID:
      return cur.skip(sizeof(td::int32)) && cur.enter_bytes();
    case lite_api::liteServer_queryPrefix::ID:
      return cur.skip(sizeof(td::int32));
    case lite_api::liteServer_waitMasterchainSeqno::ID:  // seqno:int timeout_ms:int
      return cur.skip(sizeof(td::int32) + 4 + 4);
    default:
      return false;
  }
}

}  // namespace

// The magic that says what this traffic is: the outermost one is usually just routing. A payload too
// short to hold a magic resolves to 0, which no envelope and no schema name matches.
td::int32 resolve_tl_magic(td::Slice payload) {
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

std::optional<std::string> tl_schema_name(td::int32 magic) {
  if (auto name = ton_api::Object::nameof(magic)) {
    return name;
  }
  if (auto name = ton_api::Function::nameof(magic)) {
    return name;
  }
  // Liteserver traffic speaks its own schema, so ton_api alone leaves every lite query unnamed.
  if (auto name = lite_api::Object::nameof(magic)) {
    return name;
  }
  return lite_api::Function::nameof(magic);
}

std::string tl_name(td::int32 magic) {
  if (auto name = tl_schema_name(magic)) {
    return *name;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%08x", static_cast<td::uint32>(magic));
  return buf;
}

std::string tl_name(td::Slice payload) {
  return payload.size() < sizeof(td::int32) ? "unknown" : tl_name(resolve_tl_magic(payload));
}

void TlTrafficBucket::account(td::Slice payload) {
  account(resolve_tl_magic(payload), payload.size());
}

void TlTrafficBucket::account(td::int32 magic, td::uint64 size) {
  auto &cell = tl_.at(magic);
  cell.bytes.inc(size);
  cell.messages.inc();
}

TlTrafficBucket &TlTrafficBucket::operator+=(const TlTrafficBucket &other) {
  tl_ += other.tl_;
  return *this;
}

void TlTrafficBucket::collect(Context ctx) const {
  tl_.collect(ctx, [](Context cell_ctx, const Cell &cell) {
    cell_ctx.collect(cell.bytes, "bytes");
    cell_ctx.collect(cell.messages, "messages");
  });
}

void TlLatencyBucket::observe(td::int32 magic, double seconds, bool ok) {
  auto &cell = tl_.at(magic);
  cell.duration.observe(seconds);
  if (!ok) {
    cell.failed.inc();
  }
}

void TlLatencyBucket::collect(Context ctx) const {
  tl_.collect(ctx, [this](Context cell_ctx, const Cell &cell) {
    cell_ctx.collect(cell.duration, duration_name_);
    cell_ctx.collect(cell.failed, "failed");
  });
}

}  // namespace ton::metrics
