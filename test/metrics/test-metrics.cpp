/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <chrono>

#include "adnl/adnl-peer-table.hpp"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "metrics/collectors.h"
#include "metrics/tl-traffic-bucket.h"
#include "metrics/well-known.h"
#ifdef TON_TEST_METRICS_QUIC
#include "quic/metrics.h"
#endif
#include "rldp2/RldpConnection.h"
#include "rldp2/rldp-metrics.h"
#include "td/utils/as.h"
#include "td/utils/tests.h"
#include "tl-utils/lite-utils.hpp"
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
  // The unit suffix is part of the family name (OpenMetrics: gauge sample == family name);
  // 1500ms rendered as 1.5 seconds.
  EXPECT_EQ("# TYPE latency_seconds gauge\nlatency_seconds 1.500000\n", out);
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

// ===== Histogram =====

std::string render(const Collectable auto &node, std::string_view name) {
  Sink sink;
  Context(sink).collect(node, name);
  return std::move(sink).build().render();
}

bool has_line(const std::string &out, const std::string &line) {
  return out.find(line + '\n') != std::string::npos;
}

size_t count_of(const std::string &out, const std::string &needle) {
  size_t n = 0;
  for (auto pos = out.find(needle); pos != std::string::npos; pos = out.find(needle, pos + needle.size())) {
    ++n;
  }
  return n;
}

TEST(Metrics, HistogramEmptyRendersEveryBucket) {
  auto out = render(Histogram<kDurationBuckets>{}, "x");
  EXPECT_EQ(
      "# TYPE x histogram\n"
      "x_bucket{le=\"0.001\"} 0.000000\n"
      "x_bucket{le=\"0.0025\"} 0.000000\n"
      "x_bucket{le=\"0.005\"} 0.000000\n"
      "x_bucket{le=\"0.01\"} 0.000000\n"
      "x_bucket{le=\"0.025\"} 0.000000\n"
      "x_bucket{le=\"0.05\"} 0.000000\n"
      "x_bucket{le=\"0.1\"} 0.000000\n"
      "x_bucket{le=\"0.25\"} 0.000000\n"
      "x_bucket{le=\"0.5\"} 0.000000\n"
      "x_bucket{le=\"1\"} 0.000000\n"
      "x_bucket{le=\"2.5\"} 0.000000\n"
      "x_bucket{le=\"5\"} 0.000000\n"
      "x_bucket{le=\"10\"} 0.000000\n"
      "x_bucket{le=\"30\"} 0.000000\n"
      "x_bucket{le=\"+Inf\"} 0.000000\n"
      "x_sum 0.000000\n"
      "x_count 0.000000\n",
      out);
}

TEST(Metrics, HistogramBucketsAreCumulative) {
  Histogram<kDurationBuckets> h;
  h.observe(0.0005);  // le=0.001
  h.observe(0.003);   // le=0.005
  h.observe(0.003);
  h.observe(100.0);  // overflow
  auto out = render(h, "x");
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"0.001\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"0.0025\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"0.005\"} 3.000000"));
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"30\"} 3.000000"));
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"+Inf\"} 4.000000"));
  ASSERT_TRUE(has_line(out, "x_sum 100.006500"));
  ASSERT_TRUE(has_line(out, "x_count 4.000000"));
  ASSERT_EQ(td::uint64{4}, h.count());
}

TEST(Metrics, HistogramBoundsAreCompactlyFormatted) {
  // %g, not the %f the sample values use: no `le="30.000000"`.
  auto out = render(Histogram<kDurationBuckets>{}, "x");
  ASSERT_TRUE(out.find("le=\"0.001\"") != std::string::npos);
  ASSERT_TRUE(out.find("le=\"2.5\"") != std::string::npos);
  ASSERT_TRUE(out.find("le=\"30\"") != std::string::npos);
  ASSERT_TRUE(out.find("le=\"+Inf\"") != std::string::npos);
  ASSERT_TRUE(out.find("le=\"30.000000\"") == std::string::npos);
}

TEST(Metrics, HistogramComposesWithOuterLabel) {
  Histogram<kDurationBuckets> h;
  h.observe(0.02);
  Sink sink;
  Context(sink).with_label("tl", "x").collect(h, "q");
  auto out = std::move(sink).build().render();
  // Outer labels come first, `le` last.
  ASSERT_TRUE(has_line(out, "q_bucket{tl=\"x\",le=\"0.025\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "q_bucket{tl=\"x\",le=\"0.01\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "q_sum{tl=\"x\"} 0.020000"));
  ASSERT_TRUE(has_line(out, "q_count{tl=\"x\"} 1.000000"));
}

TEST(Metrics, HistogramMerges) {
  Histogram<kDurationBuckets> a, b;
  a.observe(0.003);
  b.observe(0.003);
  b.observe(5.0);
  a += b;
  ASSERT_EQ(td::uint64{3}, a.count());
  ASSERT_TRUE(a.sum() > 5.005 && a.sum() < 5.007);
  auto out = render(a, "x");
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"0.005\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"5\"} 3.000000"));
  ASSERT_TRUE(has_line(out, "x_bucket{le=\"+Inf\"} 3.000000"));
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

TEST(Metrics, TlNameForLogs) {
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_query>(a_function(), td::Bits256::zero());
  ASSERT_EQ("tonNode.getCapabilities", tl_name(payload.as_slice()));
  ASSERT_EQ("overlay.message", tl_name(ton_api::overlay_message::ID));
  ASSERT_EQ("0xdeadbeef", tl_name(static_cast<td::int32>(0xdeadbeef)));
  ASSERT_EQ("unknown", tl_name(td::Slice("ab")));
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

TEST(Metrics, TlBroadcastWithEmptyDataStaysItself) {
  // `data` too short to hold a magic: the bytes after it (here `date`) must never be read as one.
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcast>(
      a_key(), create_tl_object<ton_api::overlay_emptyCertificate>(), 0, td::BufferSlice(), 12345, td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  ASSERT_EQ("overlay.broadcast", tl_label(payload));
}

TEST(Metrics, TlBroadcastDataStopsAtItsDeclaredLength) {
  // `data` holding exactly one magic and nothing else — an envelope's, so the resolver wants to
  // unwrap it once more. It must not, because everything past `data` (`date`, `signature`) is the
  // peer's to choose: reading there would let it name any constructor it likes.
  const td::int32 steer = ton_api::tonNode_capabilities::ID;
  td::BufferSlice data(sizeof(td::int32));
  td::as<td::int32>(data.as_slice().data()) = ton_api::overlay_message::ID;
  td::BufferSlice signature(64);
  for (size_t i = 0; i < signature.size(); i += sizeof(steer)) {
    td::as<td::int32>(signature.as_slice().substr(i).data()) = steer;
  }
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcast>(
      a_key(), create_tl_object<ton_api::overlay_emptyCertificate>(), 0, std::move(data), steer, std::move(signature));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  // The inner envelope's own header does not fit in the 4 bytes it was given, so the label stays
  // there — never "tonNode.capabilities", which is only reachable by reading past `data`.
  ASSERT_EQ("overlay.message", tl_label(payload));
}

TEST(Metrics, TlBroadcastPlumtreeSimpleUnwrapsToItsContent) {
  auto content = create_serialize_tl_object<ton_api::tonNode_capabilities>(2, 0, 0);
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcastPlumtreeSimple>(
      0, 1.5, a_key(), create_tl_object<ton_api::overlay_emptyCertificate>(), td::Bits256::zero(), 3,
      std::move(content), td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  ASSERT_EQ("tonNode.capabilities", tl_label(payload));
}

TEST(Metrics, TlBroadcastTwostepSimpleUnwrapsToItsContent) {
  auto content = create_serialize_tl_object<ton_api::tonNode_capabilities>(2, 0, 0);
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcastTwostepSimple>(
      0, 12345, a_key(), td::Bits256::zero(),
      create_tl_object<ton_api::overlay_certificate>(a_key(), 0, 1024, td::BufferSlice(64)), std::move(content),
      td::BufferSlice(), td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  ASSERT_EQ("tonNode.capabilities", tl_label(payload));
}

TEST(Metrics, TlLongFormBytesHeaderResolves) {
  // The 0xFF + 7-byte length form: no honest TON sender emits it for small payloads, but TlParser
  // accepts it, so the resolver must too — otherwise a hostile encoder could steer any payload's
  // label back to the envelope.
  auto inner = a_function();
  std::string payload;
  td::int32 magic = ton_api::overlay_unicast::ID;
  payload.append(reinterpret_cast<const char *>(&magic), sizeof(magic));
  payload += '\xff';
  payload += static_cast<char>(inner.size());
  payload.append(6, '\0');
  payload.append(inner.as_slice().data(), inner.size());
  while (payload.size() % 4 != 0) {
    payload += '\0';
  }
  ASSERT_EQ("tonNode.getCapabilities", tl_label(payload));
}

TEST(Metrics, TlTruncationSweepNeverEscapesTheSchema) {
  // Whatever prefix of an envelope chain arrives, the label is a schema name or "unknown" — the
  // resolver must never walk off the payload nor mint a label from trailing bytes.
  auto content = create_serialize_tl_object<ton_api::tonNode_capabilities>(2, 0, 0);
  auto broadcast = create_serialize_tl_object<ton_api::overlay_broadcast>(
      a_key(), create_tl_object<ton_api::overlay_emptyCertificate>(), 0, std::move(content), 12345,
      td::BufferSlice(64));
  auto payload = create_serialize_tl_object_suffix<ton_api::overlay_message>(broadcast, td::Bits256::zero());
  for (size_t len = 0; len <= payload.size(); len++) {
    auto label = tl_label(payload.as_slice().substr(0, len));
    ASSERT_TRUE(label == "unknown" || label.find('.') != std::string::npos);
  }
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

TEST(Metrics, TlLiteServerQueryUnwrapsToItsMethod) {
  auto inner = create_serialize_tl_object<lite_api::liteServer_getMasterchainInfo>();
  auto payload = create_serialize_tl_object<lite_api::liteServer_query>(std::move(inner));
  ASSERT_EQ("liteServer.getMasterchainInfo", tl_label(payload));
}

TEST(Metrics, TlLiteServerWaitSeqnoPrefixUnwrapsToItsMethod) {
  auto inner = create_serialize_tl_object<lite_api::liteServer_getMasterchainInfo>();
  lite_api::liteServer_waitMasterchainSeqno wait_prefix(7, 5000);
  auto waited = serialize_tl_object(&wait_prefix, true, std::move(inner));
  auto payload = create_serialize_tl_object<lite_api::liteServer_query>(std::move(waited));
  ASSERT_EQ("liteServer.getMasterchainInfo", tl_label(payload));
}

TEST(Metrics, TlLiteServerQueryPrefixUnwrapsToItsMethod) {
  auto inner = create_serialize_tl_object<lite_api::liteServer_getMasterchainInfo>();
  lite_api::liteServer_queryPrefix query_prefix;
  auto payload = serialize_tl_object(&query_prefix, true, std::move(inner));
  ASSERT_EQ("liteServer.getMasterchainInfo", tl_label(payload));
}

TEST(Metrics, TlLiteServerEmptyQueryStaysItself) {
  auto payload = create_serialize_tl_object<lite_api::liteServer_query>(td::BufferSlice());
  ASSERT_EQ("liteServer.query", tl_label(payload));
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

// ===== TlLatencyBucket =====

TEST(Metrics, TlLatencySplitsByConstructor) {
  TlLatencyBucket bucket{"test query"};
  bucket.observe(ton_api::tonNode_getCapabilities::ID, 0.02, true);
  bucket.observe(ton_api::overlay_message::ID, 3.0, false);
  bucket.observe(static_cast<td::int32>(0x11223344), 0.02, false);  // not in the schema
  auto out = render(bucket, "q");

  ASSERT_TRUE(has_line(out, "q_duration_seconds_bucket{tl=\"tonNode.getCapabilities\",le=\"0.025\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "q_duration_seconds_count{tl=\"tonNode.getCapabilities\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "q_failed_total{tl=\"tonNode.getCapabilities\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "q_duration_seconds_bucket{tl=\"overlay.message\",le=\"2.5\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "q_duration_seconds_bucket{tl=\"overlay.message\",le=\"5\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "q_failed_total{tl=\"overlay.message\"} 1.000000"));
  // An unknown magic must never become a label of its own.
  ASSERT_TRUE(out.find("0x11223344") == std::string::npos);
  ASSERT_TRUE(has_line(out, "q_duration_seconds_bucket{tl=\"unknown\",le=\"+Inf\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "q_failed_total{tl=\"unknown\"} 1.000000"));
  // Two families for the whole bucket, however many cells it holds.
  ASSERT_EQ(1u, count_of(out, "# TYPE q_duration_seconds histogram\n"));
  ASSERT_EQ(1u, count_of(out, "# TYPE q_failed counter\n"));
}

TEST(Metrics, TlLatencyDurationNameIsConfigurable) {
  // Outbound buckets carry their kind in the collect name, so their histogram leaf is just `seconds`.
  TlLatencyBucket roundtrip{"test query roundtrip", "seconds"};
  roundtrip.observe(ton_api::tonNode_getCapabilities::ID, 0.02, false);
  auto out = render(roundtrip, "query_roundtrip");
  ASSERT_TRUE(has_line(out, "query_roundtrip_seconds_bucket{tl=\"tonNode.getCapabilities\",le=\"0.025\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "query_roundtrip_seconds_count{tl=\"tonNode.getCapabilities\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "query_roundtrip_failed_total{tl=\"tonNode.getCapabilities\"} 1.000000"));
  ASSERT_TRUE(out.find("duration") == std::string::npos);

  // The default keeps the inbound family names, which dashboards pin.
  TlLatencyBucket inbound{"test query"};
  inbound.observe(ton_api::tonNode_getCapabilities::ID, 0.02, true);
  auto inbound_out = render(inbound, "query");
  ASSERT_TRUE(has_line(inbound_out, "query_duration_seconds_count{tl=\"tonNode.getCapabilities\"} 1.000000"));
  ASSERT_TRUE(has_line(inbound_out, "query_failed_total{tl=\"tonNode.getCapabilities\"} 0.000000"));
}

TEST(Metrics, TlLatencyAlwaysEmitsBothFamilies) {
  // The Sink identifies families by position, so the family sequence must not depend on which cells
  // are populated: the unknown cell is emitted even when empty, and an empty bucket is still two
  // families rather than none.
  TlLatencyBucket empty{"test query"};
  auto empty_out = render(empty, "q");
  ASSERT_TRUE(has_line(empty_out, "q_duration_seconds_count{tl=\"unknown\"} 0.000000"));
  ASSERT_TRUE(has_line(empty_out, "q_failed_total{tl=\"unknown\"} 0.000000"));
  ASSERT_EQ(1u, count_of(empty_out, "# TYPE q_duration_seconds histogram\n"));
  ASSERT_EQ(1u, count_of(empty_out, "# TYPE q_failed counter\n"));

  TlLatencyBucket bucket{"test query"};
  bucket.observe(ton_api::tonNode_getCapabilities::ID, 0.02, true);
  auto out = render(bucket, "q");
  ASSERT_TRUE(has_line(out, "q_duration_seconds_count{tl=\"unknown\"} 0.000000"));
}

// ===== Golden family names =====
//
// The full family set each subsystem puts on the wire, one `<name> <type>` per line in emission
// order. Dashboards and alerts pin these names, so a rename must fail here rather than silently
// blank a panel; the expected lists are hand-written from the metric structs, never regenerated.

// Renders `emit` under the root prefix production seeds (PrometheusExporter's `ton`), and returns
// the rendering's `# TYPE` lines with the marker stripped.
std::string emitted_families(auto &&emit) {
  Sink sink;
  emit(Context(sink).with_name("ton"));
  auto out = std::move(sink).build().render();

  const std::string marker = "# TYPE ";
  std::string result;
  for (size_t pos = 0; (pos = out.find(marker, pos)) != std::string::npos;) {
    auto end = out.find('\n', pos);
    result.append(out, pos + marker.size(), end - pos - marker.size());
    result += '\n';
    pos = end;
  }
  return result;
}

std::string families(std::initializer_list<std::string_view> expected) {
  std::string result;
  for (auto family : expected) {
    result += family;
    result += '\n';
  }
  return result;
}

TEST(MetricsGolden, AdnlWire) {
  // AdnlNetworkManagerImpl::collect
  ASSERT_EQ(families({
                "ton_adnl_wire_bytes counter",
                "ton_adnl_wire_packets counter",
                "ton_adnl_wire_syscalls counter",
                "ton_adnl_wire_dropped counter",
                "ton_adnl_wire_listening_sockets gauge",
            }),
            emitted_families([](Context ctx) {
              UdpWireStats wire;
              ctx.with_name("adnl").with_name("wire").collect(wire);
            }));
}

TEST(MetricsGolden, Adnl) {
  // AdnlPeerTableImpl::collect
  ASSERT_EQ(families({
                "ton_adnl_transport_inbound_packets counter",
                "ton_adnl_transport_decrypt_packets counter",
                "ton_adnl_transport_decrypt_bytes counter",
                "ton_adnl_transport_dropped counter",
                "ton_adnl_transport_local_ids gauge",
                "ton_adnl_transport_peers gauge",
                "ton_adnl_transport_peer_pairs gauge",
                "ton_adnl_transport_channels gauge",
                "ton_adnl_transport_static_nodes gauge",
                "ton_adnl_app_bytes counter",
                "ton_adnl_app_messages counter",
                "ton_adnl_app_dropped counter",
                "ton_adnl_query_duration_seconds histogram",
                "ton_adnl_query_failed counter",
                "ton_adnl_query_roundtrip_seconds histogram",
                "ton_adnl_query_roundtrip_failed counter",
            }),
            emitted_families([](Context ctx) {
              ::ton::adnl::AdnlPeerTableMetrics peer_table;
              ctx.with_name("adnl").collect(peer_table);
            }));
}

TEST(MetricsGolden, Rldp2) {
  // RldpIn::collect
  ASSERT_EQ(families({
                "ton_rldp2_wire_bytes counter",
                "ton_rldp2_wire_packets counter",
                "ton_rldp2_transport_dropped counter",
                "ton_rldp2_transport_transfers counter",
                "ton_rldp2_transport_connections gauge",
                "ton_rldp2_transport_queries_pending gauge",
                "ton_rldp2_app_bytes counter",
                "ton_rldp2_app_messages counter",
                "ton_rldp2_app_dropped counter",
                "ton_rldp2_query_roundtrip_seconds histogram",
                "ton_rldp2_query_roundtrip_failed counter",
                "ton_rldp2_message_delivery_seconds histogram",
                "ton_rldp2_message_delivery_failed counter",
            }),
            emitted_families([](Context ctx) {
              ::ton::rldp2::RldpConnMetrics aggregate;
              ::ton::rldp2::RldpMetrics metrics;
              auto rldp = ctx.with_name("rldp2");
              rldp.collect(aggregate.wire, "wire");
              rldp.collect(aggregate.transport_dropped, "transport_dropped");
              rldp.collect(metrics);
            }));
}

#ifdef TON_TEST_METRICS_QUIC
TEST(MetricsGolden, Quic) {
  // QuicSender::collect
  ASSERT_EQ(families({
                "ton_quic_wire_bytes counter",
                "ton_quic_wire_packets counter",
                "ton_quic_wire_syscalls counter",
                "ton_quic_wire_dropped counter",
                "ton_quic_wire_listening_sockets gauge",
                "ton_quic_transport_connections counter",
                "ton_quic_transport_connections_current gauge",
                "ton_quic_transport_bytes counter",
                "ton_quic_transport_packets counter",
                "ton_quic_transport_stream_bytes counter",
                "ton_quic_transport_bytes_lost counter",
                "ton_quic_transport_packets_lost counter",
                "ton_quic_transport_bytes_in_flight gauge",
                "ton_quic_transport_bytes_unacked gauge",
                "ton_quic_transport_bytes_unsent gauge",
                "ton_quic_transport_sids counter",
                "ton_quic_transport_sids_current gauge",
                "ton_quic_transport_mean_rtt_seconds gauge",
                "ton_quic_transport_dropped counter",
                "ton_quic_transport_handshakes counter",
                "ton_quic_app_bytes counter",
                "ton_quic_app_messages counter",
                "ton_quic_app_dropped counter",
                "ton_quic_query_roundtrip_seconds histogram",
                "ton_quic_query_roundtrip_failed counter",
                "ton_quic_message_delivery_seconds histogram",
                "ton_quic_message_delivery_failed counter",
            }),
            emitted_families([](Context ctx) {
              ::ton::quic::ServerStats server;
              App app;
              TlLatencyBucket query_roundtrip{"quic query roundtrip", "seconds"},
                  message_delivery{"quic message delivery", "seconds"};
              auto quic = ctx.with_name("quic");
              quic.collect(server);
              quic.collect(app, "app");
              quic.collect(query_roundtrip, "query_roundtrip");
              quic.collect(message_delivery, "message_delivery");
            }));
}
#endif

TEST(MetricsGolden, Overlay) {
  // OverlayManager::collect, over OverlayManager::broadcasts_ (overlay/overlay-manager.h).
  ASSERT_EQ(families({
                "ton_overlay_broadcast_bytes counter",
                "ton_overlay_broadcast_messages counter",
            }),
            emitted_families([](Context ctx) {
              Labeled<TlTrafficBucket, ::ton::metrics::Direction> broadcasts;
              ctx.with_name("overlay").collect(broadcasts, "broadcast");
            }));
}

}  // namespace
}  // namespace ton::metrics::test
