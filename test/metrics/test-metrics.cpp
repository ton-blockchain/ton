/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <chrono>

#include "auto/tl/ton_api.h"
#include "metrics/collectors.h"
#include "metrics/tl-traffic-bucket.h"
#include "td/utils/tests.h"
#include "tl-utils/tl-utils.hpp"

namespace ton::metrics::test {
namespace {

struct Cell {
  int v = 0;
  void collect(Context) const {
  }
};

#define DIRECTION_LIST(F) \
  F(in)                   \
  F(out)
TON_METRIC_DEFINE_LABEL(Direction, "direction", DIRECTION_LIST)
#undef DIRECTION_LIST

#define KIND_LIST(F) \
  F(message)         \
  F(query)           \
  F(answer)
TON_METRIC_DEFINE_LABEL(Kind, "kind", KIND_LIST)
#undef KIND_LIST

#define REASON_LIST(F) \
  F(invalid)           \
  F(limited)           \
  F(internal)
TON_METRIC_DEFINE_LABEL(Reason, "reason", REASON_LIST)
#undef REASON_LIST

TEST(Metrics, LabeledDesignatedInitOneDim) {
  Labeled<Cell, Direction> m{{.in = {.v = 1}, .out = {.v = 2}}};
  ASSERT_EQ(1, m.at(Direction::in).v);
  ASSERT_EQ(2, m.at(Direction::out).v);
}

TEST(Metrics, LabeledDesignatedInitTwoDim) {
  // Nested designated initializers, outer by Direction, inner by Kind. Omitted cells stay zero.
  Labeled<Cell, Direction, Kind> m{{
      .in = {.message = {.v = 1}, .query = {.v = 2}},
      .out = {.answer = {.v = 3}},
  }};
  ASSERT_EQ(1, m.at(Direction::in, Kind::message).v);
  ASSERT_EQ(2, m.at(Direction::in, Kind::query).v);
  ASSERT_EQ(0, m.at(Direction::in, Kind::answer).v);
  ASSERT_EQ(0, m.at(Direction::out, Kind::message).v);
  ASSERT_EQ(0, m.at(Direction::out, Kind::query).v);
  ASSERT_EQ(3, m.at(Direction::out, Kind::answer).v);
}

TEST(Metrics, LabeledDesignatedInitMatchesManual) {
  // The construct-from-axis path must place every cell exactly where at() expects it, across three
  // label dimensions (the order the labels appear in the type == the brace nesting order).
  Labeled<Cell, Direction, Reason, Kind> via_init{{
      .in = {.invalid = {.message = {.v = 11}}, .internal = {.query = {.v = 22}}},
      .out = {.limited = {.answer = {.v = 33}}},
  }};

  Labeled<Cell, Direction, Reason, Kind> via_at;
  via_at.at(Direction::in, Reason::invalid, Kind::message).v = 11;
  via_at.at(Direction::in, Reason::internal, Kind::query).v = 22;
  via_at.at(Direction::out, Reason::limited, Kind::answer).v = 33;

  for (auto d : {Direction::in, Direction::out}) {
    for (auto r : {Reason::invalid, Reason::limited, Reason::internal}) {
      for (auto k : {Kind::message, Kind::query, Kind::answer}) {
        ASSERT_EQ(via_at.at(d, r, k).v, via_init.at(d, r, k).v);
      }
    }
  }
}

TEST(Metrics, LabeledDefaultConstructsAndMutates) {
  // Adding the converting constructor must not break default construction / at() assignment.
  Labeled<Cell, Direction, Kind> m;
  ASSERT_EQ(0, m.at(Direction::in, Kind::message).v);
  m.at(Direction::out, Kind::query).v = 7;
  ASSERT_EQ(7, m.at(Direction::out, Kind::query).v);
}

TEST(Metrics, DurationGaugeStoresWithoutCast) {
  // A duration-typed gauge holds the duration directly — no double cast at set()/add().
  Gauge<std::chrono::nanoseconds> g;
  g.set(std::chrono::milliseconds(1500));  // ms -> ns widening, no cast
  ASSERT_EQ(std::chrono::nanoseconds(std::chrono::milliseconds(1500)).count(), g.value().count());
  g.add(std::chrono::milliseconds(500));
  ASSERT_EQ(std::chrono::nanoseconds(std::chrono::seconds(2)).count(), g.value().count());
}

TEST(Metrics, DurationGaugeRendersSecondsSuffix) {
  Sink sink;
  Context ctx(sink);
  Gauge<std::chrono::milliseconds> g{std::chrono::milliseconds(1500)};
  ctx.collect(g, "latency");
  auto out = std::move(sink).build().render();
  // _seconds suffix on the sample line; 1500ms rendered as 1.5 seconds.
  EXPECT_EQ("# TYPE latency gauge\nlatency_seconds 1.500000\n", out);
}

TEST(Metrics, PlainGaugeHasNoSecondsSuffix) {
  Sink sink;
  Context ctx(sink);
  Gauge<double> g{0.25};
  ctx.collect(g, "ratio");
  auto out = std::move(sink).build().render();
  // Plain double gauge: bare name, no _seconds suffix.
  EXPECT_EQ("# TYPE ratio gauge\nratio 0.250000\n", out);
}

// ===== TlTrafficBucket =====

// The `tl` label a single payload lands under, read back off the rendered exposition.
std::string tl_label(td::Slice payload) {
  TlTrafficBucket bucket;
  bucket.account(payload);
  Sink sink;
  Context(sink).collect(bucket, "tl");
  auto out = std::move(sink).build().render();

  const std::string prefix = "tl_messages_total{tl=\"", suffix = "\"} 1.000000";
  for (auto pos = out.find(prefix); pos != std::string::npos; pos = out.find(prefix, pos + 1)) {
    auto end = out.find(suffix, pos);
    if (end != std::string::npos && end < out.find('\n', pos)) {
      return out.substr(pos + prefix.size(), end - pos - prefix.size());
    }
  }
  return out;  // nothing was accounted: return the whole rendering so the failure is readable
}

td::BufferSlice a_function() {
  return create_serialize_tl_object<ton_api::tonNode_getCapabilities>();
}

tl_object_ptr<ton_api::PublicKey> a_key() {
  return create_tl_object<ton_api::pub_ed25519>(td::Bits256::zero());
}

TEST(Metrics, TlNakedFunctionResolves) {
  // Function constructors live in a separate nameof table from Object's.
  ASSERT_EQ("tonNode.getCapabilities", tl_label(a_function()));
}

TEST(Metrics, TlOverlayQueryUnwrapsToInnerFunction) {
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_query>(a_function(), td::Bits256::zero());
  ASSERT_EQ("tonNode.getCapabilities", tl_label(payload));
}

TEST(Metrics, TlQueryWithExtraUnwrapsWhenNoCertificate) {
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_queryWithExtra>(
      a_function(), td::Bits256::zero(), create_tl_object<ton_api::overlay_messageExtra>(0, nullptr));
  ASSERT_EQ("tonNode.getCapabilities", tl_label(payload));
}

TEST(Metrics, TlBroadcastUnwrapsToItsContent) {
  auto content = create_serialize_tl_object<ton_api::tonNode_capabilities>(2, 0, 0);
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcast>(
      a_key(), create_tl_object<ton_api::overlay_emptyCertificate>(), 0, std::move(content), 12345,
      td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  ASSERT_EQ("tonNode.capabilities", tl_label(payload));
}

TEST(Metrics, TlBroadcastFecShortStaysItself) {
  // FEC parts carry no content magic — the outer constructor is the honest label.
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcastFecShort>(
      a_key(), create_tl_object<ton_api::overlay_emptyCertificate>(), td::Bits256::zero(), td::Bits256::zero(), 7,
      td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  ASSERT_EQ("overlay.broadcastFecShort", tl_label(payload));
}

TEST(Metrics, TlDhtQueryStaysCoarse) {
  auto node = create_tl_object<ton_api::dht_node>(
      a_key(),
      create_tl_object<ton_api::adnl_addressList>(std::vector<tl_object_ptr<ton_api::adnl_Address>>(), 0, 0, 0, 0), 0,
      td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::dht_query>(a_function(), std::move(node));
  ASSERT_EQ("dht.query", tl_label(payload));
}

TEST(Metrics, TlTruncatedEnvelopeKeepsEnvelope) {
  auto full = create_serialize_tl_object_suffix<ton_api::overlay_query>(a_function(), td::Bits256::zero());
  ASSERT_EQ("overlay.query", tl_label(full.as_slice().substr(0, 20)));
}

TEST(Metrics, TlGarbageMagicIsUnknown) {
  ASSERT_EQ("unknown", tl_label(td::Slice("\x11\x22\x33\x44", 4)));
}

TEST(Metrics, TlShortPayloadIsUnknown) {
  ASSERT_EQ("unknown", tl_label(td::Slice("\x11\x22", 2)));
}

}  // namespace
}  // namespace ton::metrics::test
