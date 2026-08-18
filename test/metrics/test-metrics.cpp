/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <atomic>
#include <charconv>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>

#include "adnl/adnl-peer-table.hpp"
#include "adnl/adnl-sender-ex.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "metrics/actor-metrics.h"
#include "metrics/block-processing-metrics.h"
#include "metrics/chain-metrics.h"
#include "metrics/collectors.h"
#include "metrics/consensus-metrics.h"
#include "metrics/ext-message-pool-metrics.h"
#include "metrics/prometheus-exporter.h"
#include "metrics/tl-traffic-bucket.h"
#include "metrics/well-known.h"
#ifdef TON_TEST_METRICS_QUIC
#include "quic/metrics.h"
#endif
#include "rldp2/RldpConnection.h"
#include "rldp2/rldp-metrics.h"
#include "td/actor/actor.h"
#include "td/actor/core/Scheduler.h"
#include "td/utils/Random.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/Time.h"
#include "td/utils/as.h"
#include "td/utils/misc.h"
#include "td/utils/port/Poll.h"
#include "td/utils/port/ServerSocketFd.h"
#include "td/utils/port/SocketFd.h"
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

class TestAdnlSenderEx final : public adnl::AdnlSenderEx {
 public:
  void add_id(adnl::AdnlNodeIdShort) override {
  }
  void send_message(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }
  void send_query(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, std::string, td::Promise<td::BufferSlice>,
                  td::Timestamp, td::BufferSlice) override {
  }
  void send_query_ex(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, std::string, td::Promise<td::BufferSlice>,
                     td::Timestamp, td::BufferSlice, td::uint64) override {
  }
  void get_conn_ip_str(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, td::Promise<td::string>) override {
  }

  adnl::PeerMtu peer_mtu(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id) {
    return get_peer_mtu_inner(local_id, peer_id);
  }
  td::uint64 accepted_mtu(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id) {
    return get_peer_mtu(local_id, peer_id);
  }
  std::vector<std::pair<adnl::AdnlNodeIdShort, adnl::PeerMtu>> peers_mtu(adnl::AdnlNodeIdShort local_id) {
    return get_local_id_peers_mtu(local_id);
  }

 private:
  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort>, td::optional<adnl::AdnlNodeIdShort>) override {
  }
};

TEST(AdnlSenderEx, AggregatesPeerMtuAndTrustIndependently) {
  TestAdnlSenderEx sender;
  adnl::AdnlNodeIdShort local_id{td::Bits256::zero()};
  adnl::AdnlNodeIdShort peer_id{td::Bits256::ones()};

  sender.set_default_mtu(4'000);
  sender.set_local_id_mtu(local_id, 5'000);
  ASSERT_EQ(td::uint64{5'000}, sender.accepted_mtu(local_id, peer_id));
  ASSERT_EQ(td::uint64{0}, sender.peer_mtu(local_id, peer_id).mtu);
  ASSERT_TRUE(!sender.peer_mtu(local_id, peer_id).trusted);

  sender.add_peer_mtu(local_id, peer_id, 9'000, false);
  sender.add_peer_mtu(local_id, peer_id, 7'000, true);
  sender.add_peer_mtu(local_id, peer_id, 6'000, true);
  ASSERT_EQ(td::uint64{9'000}, sender.peer_mtu(local_id, peer_id).mtu);
  ASSERT_TRUE(sender.peer_mtu(local_id, peer_id).trusted);

  auto peers = sender.peers_mtu(local_id);
  ASSERT_EQ(size_t{1}, peers.size());
  ASSERT_EQ(peer_id, peers[0].first);
  ASSERT_EQ(td::uint64{9'000}, peers[0].second.mtu);
  ASSERT_TRUE(peers[0].second.trusted);

  sender.remove_peer_mtu(local_id, peer_id, 7'000, true);
  ASSERT_TRUE(sender.peer_mtu(local_id, peer_id).trusted);
  sender.remove_peer_mtu(local_id, peer_id, 6'000, true);
  ASSERT_EQ(td::uint64{9'000}, sender.peer_mtu(local_id, peer_id).mtu);
  ASSERT_TRUE(!sender.peer_mtu(local_id, peer_id).trusted);

  sender.remove_peer_mtu(local_id, peer_id, 9'000, false);
  ASSERT_EQ(td::uint64{0}, sender.peer_mtu(local_id, peer_id).mtu);
  ASSERT_TRUE(!sender.peer_mtu(local_id, peer_id).trusted);
}

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

std::string without_family(const std::string &out, std::string_view family) {
  std::string type_prefix = "# TYPE ";
  type_prefix.append(family);
  type_prefix += ' ';
  std::string result;
  for (size_t begin = 0; begin < out.size();) {
    auto end = out.find('\n', begin);
    auto size = (end == std::string::npos ? out.size() : end) - begin;
    std::string_view line{out.data() + begin, size};
    if (!line.starts_with(type_prefix) && !line.starts_with(family)) {
      result.append(line);
      result += '\n';
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return result;
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

TEST(Metrics, RecentMaxRetainsTwoBucketsAndMergesByGeneration) {
  RecentMax<10> left, right;
  left.observe_at(11.0, 2.0);   // generation 1
  left.observe_at(19.0, 3.0);   // same generation
  right.observe_at(21.0, 5.0);  // generation 2
  left += right;

  ASSERT_EQ(5.0, left.value_at(21.0));
  ASSERT_EQ(5.0, left.value_at(30.0));  // generations 2 and 3 are still visible
  ASSERT_EQ(0.0, left.value_at(40.0));  // both retained generations expired

  left.observe_at(9.0, 99.0);  // clock regression cannot displace newer same-parity data
  ASSERT_EQ(5.0, left.value_at(21.0));
}

TEST(Metrics, RecentMaxRejectsUnsafeClockAndInvalidSamples) {
  RecentMax<10> value;
  value.observe_at(std::numeric_limits<double>::max(), 100.0);
  value.observe_at(1.0, std::numeric_limits<double>::infinity());
  value.observe_at(1.0, std::numeric_limits<double>::quiet_NaN());
  value.observe_at(1.0, -1.0);
  ASSERT_EQ(0.0, value.value_at(1.0));
  ASSERT_EQ(0.0, value.value_at(std::numeric_limits<double>::max()));

  value.observe_at(11.0, 7.0);
  ASSERT_EQ(7.0, value.value_at(11.0));
}

TEST(Metrics, RecentMaxOmitsAbsentAndExpiredSamples) {
  RecentMax<10> value;
  EXPECT_EQ("# TYPE recent gauge\n", render(value, "recent"));

  value.observe_at(11.0, 7.0);
  // collect() uses the real monotonic clock, so exercise the optional state directly at a
  // deterministic time: absence is distinct from a legitimate retained zero.
  ASSERT_TRUE(value.recent_value_at(11.0).has_value());
  ASSERT_EQ(7.0, *value.recent_value_at(11.0));
  ASSERT_TRUE(!value.recent_value_at(40.0).has_value());
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
TEST(Metrics, QuicPeerMetricsSplitByTrust) {
  ::ton::metrics::Labeled<::ton::quic::PeerMetrics, ::ton::quic::Trust> peers;
  auto &trusted = peers.at(::ton::quic::Trust::trusted);
  auto &untrusted = peers.at(::ton::quic::Trust::untrusted);
  constexpr td::int32 unknown_magic = 0x11223344;

  trusted.app.record(::ton::metrics::Kind::query, ::ton::metrics::Direction::out, unknown_magic, 7);
  trusted.app.record_dropped(::ton::metrics::Direction::out, ::ton::metrics::Reason::internal);
  trusted.query_roundtrip.observe(unknown_magic, 0.02, false);
  trusted.message_delivery.observe(unknown_magic, 0.02, true);

  untrusted.app.record(::ton::metrics::Kind::query, ::ton::metrics::Direction::out, unknown_magic, 3);
  untrusted.app.record_dropped(::ton::metrics::Direction::out, ::ton::metrics::Reason::limited);
  untrusted.query_roundtrip.observe(unknown_magic, 0.02, true);
  untrusted.message_delivery.observe(unknown_magic, 0.02, false);

  auto out = render(peers, "quic");
  ASSERT_TRUE(has_line(
      out, "quic_app_bytes_total{trust=\"trusted\",kind=\"query\",direction=\"out\",tl=\"unknown\"} 7.000000"));
  ASSERT_TRUE(has_line(out,
                       "quic_app_bytes_total{trust=\"untrusted\",kind=\"query\",direction=\"out\",tl=\"unknown\"} "
                       "3.000000"));
  ASSERT_TRUE(has_line(out,
                       "quic_app_dropped_total{trust=\"trusted\",direction=\"out\",reason=\"internal\"} "
                       "1.000000"));
  ASSERT_TRUE(has_line(out, "quic_query_roundtrip_failed_total{trust=\"trusted\",tl=\"unknown\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "quic_message_delivery_failed_total{trust=\"untrusted\",tl=\"unknown\"} 1.000000"));
  ASSERT_EQ(1u, count_of(out, "# TYPE quic_app_bytes counter\n"));
  ASSERT_EQ(1u, count_of(out, "# TYPE quic_query_roundtrip_seconds histogram\n"));
  ASSERT_EQ(1u, count_of(out, "# TYPE quic_message_delivery_seconds histogram\n"));
}

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
                "ton_quic_transport_connections_ready gauge",
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
              Labeled<Gauge<td::uint64>, Direction, ::ton::quic::Trust> connections_ready;
              Labeled<::ton::quic::PeerMetrics, ::ton::quic::Trust> peer_metrics;
              auto quic = ctx.with_name("quic");
              quic.collect(server);
              quic.with_name("transport").collect(connections_ready, "connections_ready");
              quic.collect(peer_metrics);
            }));
}
#endif

TEST(MetricsGolden, Actor) {
  // PrometheusExporter::collect, over metrics::ActorMetrics. No scheduler runs here, so the
  // worker_threads and per-scheduler families take the null scheduler-group path and emit no
  // samples.
  ASSERT_EQ(families({
                "ton_actor_busy_ticks counter",
                "ton_actor_messages counter",
                "ton_actor_executions counter",
                "ton_actor_created counter",
                "ton_actor_alive gauge",
                "ton_actor_max_message_ticks gauge",
                "ton_actor_max_execute_ticks gauge",
                "ton_actor_max_batch_messages gauge",
                "ton_actor_max_queue_ticks gauge",
                "ton_actor_worker_busy_ticks counter",
                "ton_actor_worker_messages counter",
                "ton_actor_worker_threads gauge",
                "ton_actor_scheduler_threads gauge",
                "ton_actor_scheduler_local_queue_length gauge",
                "ton_actor_scheduler_workers_active gauge",
                "ton_actor_scheduler_current_execute_seconds gauge",
                "ton_actor_stats_enabled gauge",
                "ton_actor_ticks_per_second gauge",
            }),
            emitted_families([](Context ctx) {
              ActorMetrics actors;
              ctx.collect(actors, "actor");
            }));
}

TEST(Metrics, ActorTicksPerSecondHandlesCounterRegression) {
  EXPECT_EQ(::ton::metrics::detail::estimate_ticks_per_second(0.5, 100, 164), 128);
  EXPECT_EQ(::ton::metrics::detail::estimate_ticks_per_second(0.5, 100, 99), td::Clocks::ticks_per_second());
}

TEST(Metrics, ChainSnapshotRendersEachFamily) {
  ChainSnapshot snapshot{
      .masterchain_seqno = 1,
      .masterchain_block_age_seconds = 2.5,
      .shardclient_seqno = 3,
      .active_shards = 14,
      .collated_blocks = {.later = {.master = {.ok = 4, .error = 5}, .shard = {.ok = 6, .error = 7}},
                          .first = {.master = {.ok = 14, .error = 15}, .shard = {.ok = 16, .error = 17}}},
      .validated_blocks = {.master = {.ok = 8, .error = 9}, .shard = {.ok = 10, .error = 11}},
      .validator_groups = ChainSnapshot::Groups{.master = 12, .shard = 13},
  };
  EXPECT_EQ(
      "# TYPE masterchain_seqno gauge\n"
      "masterchain_seqno 1.000000\n"
      "# TYPE masterchain_block_age_seconds gauge\n"
      "masterchain_block_age_seconds 2.500000\n"
      "# TYPE shardclient_seqno gauge\n"
      "shardclient_seqno 3.000000\n"
      "# TYPE active_shards gauge\n"
      "active_shards 14.000000\n"
      "# TYPE collated_blocks counter\n"
      "collated_blocks_total{first=\"0\",chain=\"master\",result=\"ok\"} 4.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"master\",result=\"error\"} 5.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"shard\",result=\"ok\"} 6.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"shard\",result=\"error\"} 7.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"master\",result=\"ok\"} 14.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"master\",result=\"error\"} 15.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"shard\",result=\"ok\"} 16.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"shard\",result=\"error\"} 17.000000\n"
      "# TYPE validated_blocks counter\n"
      "validated_blocks_total{chain=\"master\",result=\"ok\"} 8.000000\n"
      "validated_blocks_total{chain=\"master\",result=\"error\"} 9.000000\n"
      "validated_blocks_total{chain=\"shard\",result=\"ok\"} 10.000000\n"
      "validated_blocks_total{chain=\"shard\",result=\"error\"} 11.000000\n"
      "# TYPE validator_groups gauge\n"
      "validator_groups{chain=\"master\"} 12.000000\n"
      "validator_groups{chain=\"shard\"} 13.000000\n",
      render(snapshot, ""));
}

TEST(Metrics, ChainSnapshotDefaultOmitsOptionalSamples) {
  EXPECT_EQ(
      "# TYPE masterchain_seqno gauge\n"
      "# TYPE masterchain_block_age_seconds gauge\n"
      "# TYPE shardclient_seqno gauge\n"
      "# TYPE active_shards gauge\n"
      "# TYPE collated_blocks counter\n"
      "collated_blocks_total{first=\"0\",chain=\"master\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"master\",result=\"error\"} 0.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"shard\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"shard\",result=\"error\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"master\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"master\",result=\"error\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"shard\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"shard\",result=\"error\"} 0.000000\n"
      "# TYPE validated_blocks counter\n"
      "validated_blocks_total{chain=\"master\",result=\"ok\"} 0.000000\n"
      "validated_blocks_total{chain=\"master\",result=\"error\"} 0.000000\n"
      "validated_blocks_total{chain=\"shard\",result=\"ok\"} 0.000000\n"
      "validated_blocks_total{chain=\"shard\",result=\"error\"} 0.000000\n"
      "# TYPE validator_groups gauge\n",
      render(ChainSnapshot{}, ""));
}

TEST(Metrics, ChainSnapshotDistinguishesAbsentFromZero) {
  ChainSnapshot snapshot{
      .masterchain_seqno = 0,
      .masterchain_block_age_seconds = std::nullopt,
      .shardclient_seqno = 0,
      .active_shards = 0,
      .collated_blocks = {},
      .validated_blocks = {},
      .validator_groups = ChainSnapshot::Groups{},
  };
  EXPECT_EQ(
      "# TYPE masterchain_seqno gauge\n"
      "masterchain_seqno 0.000000\n"
      "# TYPE masterchain_block_age_seconds gauge\n"
      "# TYPE shardclient_seqno gauge\n"
      "shardclient_seqno 0.000000\n"
      "# TYPE active_shards gauge\n"
      "active_shards 0.000000\n"
      "# TYPE collated_blocks counter\n"
      "collated_blocks_total{first=\"0\",chain=\"master\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"master\",result=\"error\"} 0.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"shard\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"0\",chain=\"shard\",result=\"error\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"master\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"master\",result=\"error\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"shard\",result=\"ok\"} 0.000000\n"
      "collated_blocks_total{first=\"1\",chain=\"shard\",result=\"error\"} 0.000000\n"
      "# TYPE validated_blocks counter\n"
      "validated_blocks_total{chain=\"master\",result=\"ok\"} 0.000000\n"
      "validated_blocks_total{chain=\"master\",result=\"error\"} 0.000000\n"
      "validated_blocks_total{chain=\"shard\",result=\"ok\"} 0.000000\n"
      "validated_blocks_total{chain=\"shard\",result=\"error\"} 0.000000\n"
      "# TYPE validator_groups gauge\n"
      "validator_groups{chain=\"master\"} 0.000000\n"
      "validator_groups{chain=\"shard\"} 0.000000\n",
      render(snapshot, ""));
}

TEST(Metrics, BlockProcessingMetricsRenderAndClamp) {
  BlockProcessingMetrics metrics;
  metrics.add_collation(BlockChain::master, BlockResult::ok, FirstInWindow::no, -1.0, {.real = 2.0, .cpu = -3.0}, -4.0);
  metrics.add_collation_phase(BlockChain::master, BlockResult::ok, CollationPhase::preinit, {.real = 5.0, .cpu = -6.0});
  metrics.add_validation(BlockChain::shard, BlockResult::error, 7.0, {.real = 8.0, .cpu = 9.0}, 10.0);
  metrics.add_validation_phase(BlockChain::shard, BlockResult::error, ValidationPhase::check_new_state,
                               {.real = -11.0, .cpu = 12.0});
  metrics.add_collation_external(BlockChain::master, BlockResult::ok, CollationExternalOutcome::included, 13);
  metrics.add_collation_external(BlockChain::shard, BlockResult::error, CollationExternalOutcome::rejected, 17);
  metrics.add_collation_work(BlockChain::shard, FirstInWindow::no, {.gas = 21});
  metrics.add_collation_work(BlockChain::master, FirstInWindow::yes, {.transactions = 3});
  metrics.add_collation_out_queue(BlockChain::shard, {.size = 4096, .cleaned = 12, .processed = 34, .skipped = 5});
  metrics.add_collation_storage_cache(BlockChain::shard, StorageCacheOutcome::hit, 7, 700);
  metrics.add_collation_storage_cache(BlockChain::master, StorageCacheOutcome::small, 1, 2);
  metrics.add_want_split(BlockChain::master);
  metrics.add_overload(BlockChain::master, 1);
  metrics.add_overload(BlockChain::master, 2);
  metrics.add_overload(BlockChain::shard, 3);
  metrics.add_overload(BlockChain::shard, 4);
  metrics.add_overload(BlockChain::shard, 99);

  EXPECT_EQ(
      "# TYPE block_processing_seconds counter\n"
      "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\"total\",clock="
      "\"elapsed\"} 0.000000\n"
      "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\"total\",clock="
      "\"real\"} 2.000000\n"
      "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\"total\",clock="
      "\"cpu\"} 0.000000\n"
      "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\"wait_externals\","
      "clock=\"elapsed\"} 0.000000\n"
      "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\"preinit\",clock="
      "\"real\"} 5.000000\n"
      "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\"preinit\",clock="
      "\"cpu\"} 0.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"total\",clock="
      "\"elapsed\"} 7.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"total\",clock="
      "\"real\"} 8.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"total\",clock="
      "\"cpu\"} 9.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"active\",clock="
      "\"elapsed\"} 10.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"waiting\",clock="
      "\"elapsed\"} 0.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"check_new_"
      "state\",clock=\"real\"} 0.000000\n"
      "block_processing_seconds_total{operation=\"validate\",chain=\"shard\",result=\"error\",phase=\"check_new_"
      "state\",clock=\"cpu\"} 12.000000\n"
      "# TYPE collation_ext_messages counter\n"
      "collation_ext_messages_total{chain=\"master\",result=\"ok\",outcome=\"filtered\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"ok\",outcome=\"skipped_backpressure\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"ok\",outcome=\"included\"} 13.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"ok\",outcome=\"rejected\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"error\",outcome=\"filtered\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"error\",outcome=\"skipped_backpressure\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"error\",outcome=\"included\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"master\",result=\"error\",outcome=\"rejected\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"ok\",outcome=\"filtered\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"ok\",outcome=\"skipped_backpressure\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"ok\",outcome=\"included\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"ok\",outcome=\"rejected\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"error\",outcome=\"filtered\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"error\",outcome=\"skipped_backpressure\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"error\",outcome=\"included\"} 0.000000\n"
      "collation_ext_messages_total{chain=\"shard\",result=\"error\",outcome=\"rejected\"} 17.000000\n"
      "# TYPE collation_elapsed_seconds counter\n"
      "collation_elapsed_seconds_total{chain=\"master\",first=\"0\"} 0.000000\n"
      "collation_elapsed_seconds_total{chain=\"master\",first=\"1\"} 0.000000\n"
      "collation_elapsed_seconds_total{chain=\"shard\",first=\"0\"} 0.000000\n"
      "collation_elapsed_seconds_total{chain=\"shard\",first=\"1\"} 0.000000\n"
      "# TYPE collation_transactions counter\n"
      "collation_transactions_total{chain=\"master\",first=\"0\"} 0.000000\n"
      "collation_transactions_total{chain=\"master\",first=\"1\"} 3.000000\n"
      "collation_transactions_total{chain=\"shard\",first=\"0\"} 0.000000\n"
      "collation_transactions_total{chain=\"shard\",first=\"1\"} 0.000000\n"
      "# TYPE collation_gas counter\n"
      "collation_gas_total{chain=\"master\"} 0.000000\n"
      "collation_gas_total{chain=\"shard\"} 21.000000\n"
      "# TYPE collation_block_bytes counter\n"
      "collation_block_bytes_total{chain=\"master\"} 0.000000\n"
      "collation_block_bytes_total{chain=\"shard\"} 0.000000\n"
      "# TYPE collation_collated_data_bytes counter\n"
      "collation_collated_data_bytes_total{chain=\"master\"} 0.000000\n"
      "collation_collated_data_bytes_total{chain=\"shard\"} 0.000000\n"
      "# TYPE collation_ext_messages_offered counter\n"
      "collation_ext_messages_offered_total{chain=\"master\"} 0.000000\n"
      "collation_ext_messages_offered_total{chain=\"shard\"} 0.000000\n"
      "# TYPE collation_out_queue_size gauge\n"
      "collation_out_queue_size{chain=\"master\"} 0.000000\n"
      "collation_out_queue_size{chain=\"shard\"} 4096.000000\n"
      "# TYPE collation_out_queue_cleaned counter\n"
      "collation_out_queue_cleaned_total{chain=\"master\"} 0.000000\n"
      "collation_out_queue_cleaned_total{chain=\"shard\"} 12.000000\n"
      "# TYPE collation_out_queue_processed counter\n"
      "collation_out_queue_processed_total{chain=\"master\"} 0.000000\n"
      "collation_out_queue_processed_total{chain=\"shard\"} 34.000000\n"
      "# TYPE collation_out_queue_skipped counter\n"
      "collation_out_queue_skipped_total{chain=\"master\"} 0.000000\n"
      "collation_out_queue_skipped_total{chain=\"shard\"} 5.000000\n"
      "# TYPE collation_storage_cache_lookups counter\n"
      "collation_storage_cache_lookups_total{chain=\"master\",outcome=\"hit\"} 0.000000\n"
      "collation_storage_cache_lookups_total{chain=\"master\",outcome=\"miss\"} 0.000000\n"
      "collation_storage_cache_lookups_total{chain=\"master\",outcome=\"small\"} 1.000000\n"
      "collation_storage_cache_lookups_total{chain=\"shard\",outcome=\"hit\"} 7.000000\n"
      "collation_storage_cache_lookups_total{chain=\"shard\",outcome=\"miss\"} 0.000000\n"
      "collation_storage_cache_lookups_total{chain=\"shard\",outcome=\"small\"} 0.000000\n"
      "# TYPE collation_storage_cache_cells counter\n"
      "collation_storage_cache_cells_total{chain=\"master\",outcome=\"hit\"} 0.000000\n"
      "collation_storage_cache_cells_total{chain=\"master\",outcome=\"miss\"} 0.000000\n"
      "collation_storage_cache_cells_total{chain=\"master\",outcome=\"small\"} 2.000000\n"
      "collation_storage_cache_cells_total{chain=\"shard\",outcome=\"hit\"} 700.000000\n"
      "collation_storage_cache_cells_total{chain=\"shard\",outcome=\"miss\"} 0.000000\n"
      "collation_storage_cache_cells_total{chain=\"shard\",outcome=\"small\"} 0.000000\n"
      "# TYPE collation_want_split counter\n"
      "collation_want_split_total{chain=\"master\"} 1.000000\n"
      "collation_want_split_total{chain=\"shard\"} 0.000000\n"
      "# TYPE collation_overload counter\n"
      "collation_overload_total{chain=\"master\",reason=\"block_limits\"} 1.000000\n"
      "collation_overload_total{chain=\"master\",reason=\"out_msg_queue\"} 1.000000\n"
      "collation_overload_total{chain=\"master\",reason=\"long_collation\"} 0.000000\n"
      "collation_overload_total{chain=\"master\",reason=\"dispatch_queue\"} 0.000000\n"
      "collation_overload_total{chain=\"master\",reason=\"unknown\"} 0.000000\n"
      "collation_overload_total{chain=\"shard\",reason=\"block_limits\"} 0.000000\n"
      "collation_overload_total{chain=\"shard\",reason=\"out_msg_queue\"} 0.000000\n"
      "collation_overload_total{chain=\"shard\",reason=\"long_collation\"} 1.000000\n"
      "collation_overload_total{chain=\"shard\",reason=\"dispatch_queue\"} 1.000000\n"
      "collation_overload_total{chain=\"shard\",reason=\"unknown\"} 1.000000\n",
      without_family(without_family(without_family(render(metrics, ""), "block_processing_duration_seconds"),
                                    "block_processing_duration_recent_max_seconds"),
                     "block_processing_phase_recent_max_seconds"));
}

TEST(Metrics, BlockProcessingMetricsRenderEveryPhase) {
  constexpr std::array collation_phases = {
      "preinit",
      "queue_cleanup",
      "prelim_storage_stat",
      "trx_tvm",
      "trx_storage_stat",
      "trx_other",
      "final_storage_stat",
      "enqueue_new_messages",
      "combine_account_transactions",
      "create_shard_state",
      "create_block",
      "create_collated_data",
      "create_block_candidate",
      "dispatch_queue",
      "import_internals",
      "import_externals",
      "process_new_msgs",
  };
  constexpr std::array validation_phases = {
      "unpack_block_candidate",
      "process_mc_state",
      "trx_tvm",
      "trx_storage_stat",
      "trx_other",
      "check_transactions_other",
      "unpack_state",
      "validate_block_tlb",
      "unpack_block_data",
      "precheck_account_updates",
      "precheck_account_transactions",
      "precheck_msg_queue",
      "unpack_dispatch_queue",
      "check_in_msg_descr",
      "check_out_msg_descr",
      "check_dispatch_queue",
      "check_processed_upto",
      "check_in_queue",
      "check_new_state",
  };

  BlockProcessingMetrics metrics;
  metrics.add_collation(BlockChain::master, BlockResult::ok, FirstInWindow::no, 1.0, {.real = 1.0, .cpu = 1.0}, 1.0);
  for (size_t i = 0; i < collation_phases.size(); ++i) {
    metrics.add_collation_phase(BlockChain::master, BlockResult::ok, static_cast<CollationPhase>(i),
                                {.real = 1.0, .cpu = 1.0});
  }
  metrics.add_validation(BlockChain::master, BlockResult::ok, 2.0, {.real = 1.0, .cpu = 1.0}, 1.0);
  for (size_t i = 0; i < validation_phases.size(); ++i) {
    metrics.add_validation_phase(BlockChain::master, BlockResult::ok, static_cast<ValidationPhase>(i),
                                 {.real = 1.0, .cpu = 1.0});
  }

  auto out = render(metrics, "");
  ASSERT_EQ(38u, count_of(out, "block_processing_seconds_total{operation=\"collate\""));
  ASSERT_EQ(43u, count_of(out, "block_processing_seconds_total{operation=\"validate\""));
  for (auto phase : collation_phases) {
    ASSERT_TRUE(has_line(
        out, PSTRING() << "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\",phase=\""
                       << phase << "\",clock=\"real\"} 1.000000"));
  }
  for (auto phase : validation_phases) {
    ASSERT_TRUE(has_line(
        out,
        PSTRING() << "block_processing_seconds_total{operation=\"validate\",chain=\"master\",result=\"ok\",phase=\""
                  << phase << "\",clock=\"cpu\"} 1.000000"));
  }
}

TEST(Metrics, BlockProcessingDurationHistogramMergesAndBoundsCardinality) {
  BlockProcessingMetrics total, delta;
  total.add_collation(BlockChain::master, BlockResult::ok, FirstInWindow::no, 0.003, {.real = 0.02, .cpu = 0.001}, 0.0);
  // Detailed phases remain cumulative counters and must not add distribution observations.
  total.add_collation_phase(BlockChain::master, BlockResult::ok, CollationPhase::preinit, {.real = 0.01, .cpu = 0.005});
  delta.add_collation(BlockChain::master, BlockResult::ok, FirstInWindow::no, 0.7, {.real = 1.2, .cpu = 0.4}, 0.0);
  delta.add_validation(BlockChain::shard, BlockResult::error, 130.0, {.real = 60.0, .cpu = -1.0}, 130.0);
  total += delta;

  auto out = render(total, "");
  ASSERT_EQ(1u, count_of(out, "# TYPE block_processing_duration_seconds histogram\n"));
  // Fixed cardinality: operation * chain * result * clock = 2 * 2 * 2 * 3 cells. There is no phase
  // and no window-slot axis on this family, so neither new detailed timers nor the first-in-window
  // split multiplies histogram series.
  ASSERT_EQ(24u, count_of(out, "block_processing_duration_seconds_count{"));
  ASSERT_EQ(24u * 17u, count_of(out, "block_processing_duration_seconds_bucket{"));
  ASSERT_EQ(12u, count_of(out, "block_processing_duration_seconds_count{operation=\"collate\""));
  ASSERT_EQ(12u, count_of(out, "block_processing_duration_seconds_count{operation=\"validate\""));

  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_count{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "clock=\"elapsed\"} 2.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_sum{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "clock=\"elapsed\"} 0.703000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_bucket{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "clock=\"elapsed\",le=\"0.005\"} 1.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_bucket{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "clock=\"elapsed\",le=\"1\"} 2.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_sum{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "clock=\"real\"} 1.220000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_sum{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "clock=\"cpu\"} 0.401000"));
  ASSERT_TRUE(
      has_line(out,
               "block_processing_duration_recent_max_seconds{operation=\"collate\",chain=\"master\",result=\"ok\","
               "clock=\"elapsed\"} 0.700000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_phase_recent_max_seconds{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "phase=\"preinit\",clock=\"real\"} 0.010000"));

  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_bucket{operation=\"validate\",chain=\"shard\","
                       "result=\"error\",clock=\"elapsed\",le=\"120\"} 0.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_bucket{operation=\"validate\",chain=\"shard\","
                       "result=\"error\",clock=\"elapsed\",le=\"+Inf\"} 1.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_bucket{operation=\"validate\",chain=\"shard\","
                       "result=\"error\",clock=\"real\",le=\"60\"} 1.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_sum{operation=\"validate\",chain=\"shard\",result=\"error\","
                       "clock=\"cpu\"} 0.000000"));

  // The pre-existing cumulative family is merged alongside the distributions.
  ASSERT_TRUE(has_line(out,
                       "block_processing_seconds_total{operation=\"collate\",chain=\"master\",result=\"ok\","
                       "phase=\"total\",clock=\"elapsed\"} 0.703000"));
}

TEST(Metrics, BlockProcessingDropsNonFiniteTimerSamplesIndependently) {
  BlockProcessingMetrics metrics;
  metrics.add_collation(BlockChain::shard, BlockResult::ok, FirstInWindow::yes,
                        std::numeric_limits<double>::quiet_NaN(),
                        {.real = std::numeric_limits<double>::infinity(), .cpu = 0.25}, 0.0);

  auto out = render(metrics, "");
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_count{operation=\"collate\",chain=\"shard\",result=\"ok\","
                       "clock=\"elapsed\"} 0.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_count{operation=\"collate\",chain=\"shard\",result=\"ok\","
                       "clock=\"real\"} 0.000000"));
  ASSERT_TRUE(has_line(out,
                       "block_processing_duration_seconds_count{operation=\"collate\",chain=\"shard\",result=\"ok\","
                       "clock=\"cpu\"} 1.000000"));
  ASSERT_TRUE(out.find("block_processing_seconds_total{operation=\"collate\",chain=\"shard\",result=\"ok\","
                       "phase=\"total\",clock=\"elapsed\"") == std::string::npos);
  ASSERT_TRUE(out.find("block_processing_seconds_total{operation=\"collate\",chain=\"shard\",result=\"ok\","
                       "phase=\"total\",clock=\"real\"") == std::string::npos);
  ASSERT_TRUE(has_line(out,
                       "block_processing_seconds_total{operation=\"collate\",chain=\"shard\",result=\"ok\","
                       "phase=\"total\",clock=\"cpu\"} 0.250000"));
  ASSERT_TRUE(has_line(out, "collation_elapsed_seconds_total{chain=\"shard\",first=\"1\"} 0.000000"));
}

TEST(Metrics, BlockProcessingSplitsFirstInWindowCountersOnly) {
  BlockProcessingMetrics metrics;
  metrics.add_collation(BlockChain::shard, BlockResult::ok, FirstInWindow::yes, 0.4, {.real = 0.4, .cpu = 0.2}, 0.0);
  metrics.add_collation(BlockChain::shard, BlockResult::ok, FirstInWindow::no, 0.02, {.real = 0.02, .cpu = 0.01}, 0.0);
  metrics.add_collation(BlockChain::shard, BlockResult::error, FirstInWindow::no, 0.3, {.real = 0.3, .cpu = 0.1}, 0.0);
  metrics.add_collation_work(BlockChain::shard, FirstInWindow::yes, {.transactions = 90, .gas = 900});
  metrics.add_collation_work(BlockChain::shard, FirstInWindow::no, {.transactions = 10, .gas = 100});
  metrics.add_validation(BlockChain::shard, BlockResult::ok, 0.05, {.real = 0.05, .cpu = 0.03}, 0.05);

  auto out = render(metrics, "ton");
  // The window-slot panel divides these two families by the collated-block counter, cell by cell.
  // Only successful attempts accumulate, so the failed 0.3 s collation stays out of the mean.
  ASSERT_TRUE(has_line(out, "ton_collation_elapsed_seconds_total{chain=\"shard\",first=\"1\"} 0.400000"));
  ASSERT_TRUE(has_line(out, "ton_collation_elapsed_seconds_total{chain=\"shard\",first=\"0\"} 0.020000"));
  ASSERT_TRUE(has_line(out, "ton_collation_transactions_total{chain=\"shard\",first=\"1\"} 90.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_transactions_total{chain=\"shard\",first=\"0\"} 10.000000"));
  // Everything else keeps the unsplit label set: the remaining workload counters sum both slots,
  // and the duration histogram and its recent-max twin pool them.
  ASSERT_TRUE(has_line(out, "ton_collation_gas_total{chain=\"shard\"} 1000.000000"));
  ASSERT_TRUE(
      has_line(out,
               "ton_block_processing_duration_seconds_count{operation=\"collate\",chain=\"shard\",result=\"ok\","
               "clock=\"elapsed\"} 2.000000"));
  ASSERT_TRUE(has_line(out,
                       "ton_block_processing_duration_recent_max_seconds{operation=\"collate\",chain=\"shard\","
                       "result=\"ok\",clock=\"elapsed\"} 0.400000"));
  ASSERT_TRUE(has_line(out,
                       "ton_block_processing_duration_seconds_count{operation=\"validate\",chain=\"shard\","
                       "result=\"ok\",clock=\"elapsed\"} 1.000000"));
  // Two families, four cells each: nothing else in this exposition carries the axis.
  ASSERT_EQ(8u, count_of(out, "first=\""));
}

TEST(Metrics, BlockProcessingOutQueueKeepsDepthAndSumsProgress) {
  BlockProcessingMetrics total, delta;
  total.add_collation_out_queue(BlockChain::shard, {.size = 900, .cleaned = 3, .processed = 40, .skipped = 1});
  total.add_collation_out_queue(BlockChain::shard, {.size = 700, .cleaned = 5, .processed = 60, .skipped = 2});
  delta.add_collation_out_queue(BlockChain::shard, {.size = 1500, .cleaned = 1, .processed = 10, .skipped = 0});
  delta.add_collation_out_queue(BlockChain::master, {.size = 4, .cleaned = 0, .processed = 0, .skipped = 0});
  total += delta;

  auto out = render(total, "ton");
  // The depth is the last observation per chain, and merging keeps the worst of the two snapshots.
  ASSERT_TRUE(has_line(out, "ton_collation_out_queue_size{chain=\"shard\"} 1500.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_out_queue_size{chain=\"master\"} 4.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_out_queue_cleaned_total{chain=\"shard\"} 9.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_out_queue_processed_total{chain=\"shard\"} 110.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_out_queue_skipped_total{chain=\"shard\"} 3.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_out_queue_cleaned_total{chain=\"master\"} 0.000000"));
}

TEST(Metrics, BlockProcessingStorageCacheCountsLookupsAndCells) {
  BlockProcessingMetrics total, delta;
  total.add_collation_storage_cache(BlockChain::shard, StorageCacheOutcome::hit, 4, 4000);
  total.add_collation_storage_cache(BlockChain::shard, StorageCacheOutcome::miss, 1, 900);
  total.add_collation_storage_cache(BlockChain::shard, StorageCacheOutcome::small, 6, 30);
  delta.add_collation_storage_cache(BlockChain::shard, StorageCacheOutcome::hit, 2, 2000);
  delta.add_collation_storage_cache(BlockChain::master, StorageCacheOutcome::miss, 3, 300);
  total += delta;

  auto out = render(total, "ton");
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_lookups_total{chain=\"shard\",outcome=\"hit\"} 6.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_lookups_total{chain=\"shard\",outcome=\"miss\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_lookups_total{chain=\"shard\",outcome=\"small\"} 6.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_cells_total{chain=\"shard\",outcome=\"hit\"} 6000.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_cells_total{chain=\"shard\",outcome=\"miss\"} 900.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_cells_total{chain=\"master\",outcome=\"miss\"} 300.000000"));
  ASSERT_TRUE(has_line(out, "ton_collation_storage_cache_lookups_total{chain=\"master\",outcome=\"hit\"} 0.000000"));
}

TEST(MetricsGolden, BlockProcessing) {
  ASSERT_EQ(families({
                "ton_block_processing_seconds counter",
                "ton_block_processing_duration_seconds histogram",
                "ton_block_processing_duration_recent_max_seconds gauge",
                "ton_block_processing_phase_recent_max_seconds gauge",
                "ton_collation_ext_messages counter",
                "ton_collation_elapsed_seconds counter",
                "ton_collation_transactions counter",
                "ton_collation_gas counter",
                "ton_collation_block_bytes counter",
                "ton_collation_collated_data_bytes counter",
                "ton_collation_ext_messages_offered counter",
                "ton_collation_out_queue_size gauge",
                "ton_collation_out_queue_cleaned counter",
                "ton_collation_out_queue_processed counter",
                "ton_collation_out_queue_skipped counter",
                "ton_collation_storage_cache_lookups counter",
                "ton_collation_storage_cache_cells counter",
                "ton_collation_want_split counter",
                "ton_collation_overload counter",
            }),
            emitted_families([](Context ctx) { ctx.collect(BlockProcessingMetrics{}); }));
}

TEST(Metrics, ConsensusRoundFoldsConsecutiveMarks) {
  ConsensusMetrics metrics;
  // collate 0.02, publish 0.003, validate_wait 0.001, validate 0.04, notarize_vote 0.002,
  // notarize_cert 0.03, finalize_vote 0.002, finalize_cert 0.03, apply 0.01
  metrics.observe_round(BlockChain::shard, {1.000, 1.020, 1.023, 1.024, 1.064, 1.066, 1.096, 1.098, 1.128, 1.138});

  auto out = render(metrics, "ton");
  ASSERT_TRUE(
      has_line(out, "ton_consensus_stage_seconds_bucket{chain=\"shard\",stage=\"collate\",le=\"0.025\"} 1.000000"));
  ASSERT_TRUE(
      has_line(out, "ton_consensus_stage_seconds_bucket{chain=\"shard\",stage=\"collate\",le=\"0.005\"} 0.000000"));
  ASSERT_TRUE(
      has_line(out, "ton_consensus_stage_seconds_bucket{chain=\"shard\",stage=\"validate\",le=\"0.1\"} 1.000000"));
  ASSERT_TRUE(
      has_line(out, "ton_consensus_stage_seconds_bucket{chain=\"shard\",stage=\"validate\",le=\"0.025\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_sum{chain=\"shard\",stage=\"apply\"} 0.010000"));
  // Every stage of a complete round is observed exactly once, and only on its own chain.
  ASSERT_EQ(9u, count_of(out, "ton_consensus_stage_seconds_count{chain=\"shard\",stage=\""));
  ASSERT_EQ(9u, count_of(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\""));
  ASSERT_EQ(0u, count_of(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"collate\"} 1.000000"));
}

TEST(Metrics, ConsensusRoundSkipsStagesWithoutBothMarks) {
  ConsensusMetrics metrics;
  // A validator that did not collate has no collate marks; this one also never voted to finalize.
  metrics.observe_round(BlockChain::master,
                        {std::nullopt, std::nullopt, 2.0, 2.001, 2.05, 2.051, 2.08, std::nullopt, std::nullopt, 2.2});

  auto out = render(metrics, "ton");
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"collate\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"publish\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"validate\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"notarize_cert\"} 1.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"finalize_vote\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_count{chain=\"master\",stage=\"apply\"} 0.000000"));
}

TEST(Metrics, ConsensusMetricsClampAndMerge) {
  ConsensusMetrics reported, total;
  reported.observe_stage(BlockChain::shard, ConsensusStage::notarize_cert, -0.5);  // cert seen before our own vote
  reported.observe_stage(BlockChain::shard, ConsensusStage::apply, 1e9);           // corrupt/outlier duration input
  reported.observe_slot_gap(BlockChain::shard, 1e9);
  reported.observe_slot_gap(BlockChain::shard, 0.3);
  reported.observe_slot_lead(BlockChain::shard, -0.2);  // corrupt negative duration input
  reported.observe_slot_mark(BlockChain::shard, ConsensusMark::collate_finish,
                             -0.8);  // finished before the slot began
  reported.add_round(BlockChain::shard, ConsensusRoundOutcome::empty);
  total += reported;
  total += reported;

  auto out = render(total, "ton");
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_sum{chain=\"shard\",stage=\"notarize_cert\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_stage_seconds_sum{chain=\"shard\",stage=\"apply\"} 7200.000000"));
  ASSERT_TRUE(has_line(
      out, "ton_consensus_stage_seconds_bucket{chain=\"shard\",stage=\"notarize_cert\",le=\"0.001\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_gap_seconds_bucket{chain=\"shard\",le=\"0.5\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_gap_seconds_sum{chain=\"shard\"} 7200.600000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_sum{chain=\"shard\"} 0.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_count{chain=\"shard\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_gap_recent_max_seconds{chain=\"shard\"} 3600.000000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"before\"} 1.600000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_count{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"after\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_rounds_total{chain=\"shard\",outcome=\"empty\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_rounds_total{chain=\"master\",outcome=\"skipped\"} 0.000000"));
}

TEST(Metrics, ConsensusHandoffObservesBothSides) {
  ConsensusMetrics metrics;
  // A waiting handoff: 0.4 s of dead time after the accept, so no head start. Then a pipelined one:
  // the next slot had been collating for 0.25 s when the parent was accepted, so no dead time. Both
  // handoffs land in both families, and the mean signed handoff is 0.4 / 2 - 0.25 / 2 = 0.075 s.
  metrics.observe_slot_gap(BlockChain::master, 0.4);
  metrics.observe_slot_lead(BlockChain::master, 0.0);
  metrics.observe_slot_gap(BlockChain::master, 0.0);
  metrics.observe_slot_lead(BlockChain::master, 0.25);

  auto out = render(metrics, "ton");
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_gap_seconds_count{chain=\"master\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_count{chain=\"master\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_gap_seconds_sum{chain=\"master\"} 0.400000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_sum{chain=\"master\"} 0.250000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_bucket{chain=\"master\",le=\"0.25\"} 2.000000"));
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_bucket{chain=\"master\",le=\"0.1\"} 1.000000"));
  // The other chain still emits both families, all zeros.
  ASSERT_TRUE(has_line(out, "ton_consensus_slot_lead_seconds_count{chain=\"shard\"} 0.000000"));
}

TEST(Metrics, ConsensusSlotMarksPreserveBothSidesOfSlotZero) {
  ConsensusMetrics metrics;
  metrics.observe_slot_mark(BlockChain::shard, ConsensusMark::collate_finish, -0.9);
  metrics.observe_slot_mark(BlockChain::shard, ConsensusMark::collate_finish, 0.0);
  metrics.observe_slot_mark(BlockChain::shard, ConsensusMark::collate_finish, 0.3);
  metrics.observe_slot_mark(BlockChain::master, ConsensusMark::collate_finish, 0.12);

  auto out = render(metrics, "ton");
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_count{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"before\"} 3.000000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"before\"} 0.900000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"after\"} 0.300000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_bucket{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"after\",le=\"0\"} 2.000000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_bucket{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"after\",le=\"0.001\"} 2.000000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_bucket{chain=\"shard\",mark=\"collate_finish\","
                       "side=\"after\",le=\"0.5\"} 3.000000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_seconds_sum{chain=\"master\",mark=\"collate_finish\","
                       "side=\"after\"} 0.120000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_recent_max_seconds{chain=\"shard\","
                       "mark=\"collate_finish\",side=\"before\"} 0.900000"));
  ASSERT_TRUE(has_line(out,
                       "ton_consensus_collator_slot_mark_recent_max_seconds{chain=\"shard\","
                       "mark=\"collate_finish\",side=\"after\"} 0.300000"));
}

TEST(Metrics, ConsensusExportsOnlyTheConsumedSlotMarks) {
  ConsensusMetrics metrics;
  // The collector walks every round mark; only the four the dashboards read reach an exposition.
  for (size_t mark = 0; mark < LabelDomainOf<ConsensusMark>::size; ++mark) {
    metrics.observe_slot_mark(BlockChain::shard, static_cast<ConsensusMark>(mark), 0.2);
  }

  auto out = render(metrics, "ton");
  for (auto kept : {"collate_start", "collate_finish", "finalize_cert", "apply"}) {
    ASSERT_TRUE(has_line(out, PSTRING() << "ton_consensus_collator_slot_mark_seconds_count{chain=\"shard\",mark=\""
                                        << kept << "\",side=\"after\"} 1.000000"));
  }
  for (auto dropped : {"candidate_received", "validation_start", "validation_finish", "notarize_vote", "notarize_cert",
                       "finalize_vote"}) {
    ASSERT_EQ(0u, count_of(out, PSTRING() << "mark=\"" << dropped << "\""));
  }
  // 2 chains * 4 marks * 2 sides, and the observation is not silently folded into another cell.
  ASSERT_EQ(16u, count_of(out, "ton_consensus_collator_slot_mark_seconds_count{"));
  ASSERT_EQ(16u * 12u, count_of(out, "ton_consensus_collator_slot_mark_seconds_bucket{"));
}

TEST(MetricsGolden, Consensus) {
  // ValidatorManagerImpl::collect_chain_metrics
  ASSERT_EQ(families({
                "ton_consensus_stage_seconds histogram",
                "ton_consensus_slot_gap_seconds histogram",
                "ton_consensus_slot_lead_seconds histogram",
                "ton_consensus_slot_gap_recent_max_seconds gauge",
                "ton_consensus_slot_lead_recent_max_seconds gauge",
                "ton_consensus_collator_slot_mark_seconds histogram",
                "ton_consensus_collator_slot_mark_recent_max_seconds gauge",
                "ton_consensus_stage_recent_max_seconds gauge",
                "ton_consensus_rounds counter",
            }),
            emitted_families([](Context ctx) { ctx.collect(ConsensusMetrics{}); }));
}

TEST(Metrics, ExtMessagePoolSnapshotRendersEachFamily) {
  ExtMessagePoolSnapshot snapshot{
      .pending_ext_messages = 3,
      .oldest_ext_message_age_seconds = 4.5,
      .check_ok = 5,
      .check_error = 7,
      .applied_master = 41,
      .applied_shard = 43,
  };
  for (size_t i = 0; i < snapshot.admission.size(); ++i) {
    snapshot.admission[i] = 11 + i;
  }
  for (size_t i = 0; i < snapshot.removed.size(); ++i) {
    snapshot.removed[i] = 31 + i;
  }
  auto out = render(snapshot, "");
  EXPECT_EQ(
      "# TYPE mempool_ext_messages gauge\n"
      "mempool_ext_messages 3.000000\n"
      "# TYPE mempool_oldest_ext_message_age_seconds gauge\n"
      "mempool_oldest_ext_message_age_seconds 4.500000\n"
      "# TYPE mempool_ext_check counter\n"
      "mempool_ext_check_total{result=\"ok\"} 5.000000\n"
      "mempool_ext_check_total{result=\"error\"} 7.000000\n"
      "# TYPE mempool_ext_admission counter\n"
      "mempool_ext_admission_total{outcome=\"accepted\"} 11.000000\n"
      "mempool_ext_admission_total{outcome=\"not_ready\"} 12.000000\n"
      "mempool_ext_admission_total{outcome=\"too_large\"} 13.000000\n"
      "mempool_ext_admission_total{outcome=\"backpressure\"} 14.000000\n"
      "mempool_ext_admission_total{outcome=\"invalid\"} 15.000000\n"
      "mempool_ext_admission_total{outcome=\"state_unavailable\"} 16.000000\n"
      "mempool_ext_admission_total{outcome=\"vm_rejected\"} 17.000000\n"
      "mempool_ext_admission_total{outcome=\"rate_limited\"} 18.000000\n"
      "mempool_ext_admission_total{outcome=\"pool_full\"} 19.000000\n"
      "mempool_ext_admission_total{outcome=\"address_full\"} 20.000000\n"
      "mempool_ext_admission_total{outcome=\"duplicate\"} 21.000000\n"
      "mempool_ext_admission_total{outcome=\"internal_error\"} 22.000000\n"
      "mempool_ext_admission_total{outcome=\"reprioritized\"} 23.000000\n"
      "# TYPE mempool_ext_removed counter\n"
      "mempool_ext_removed_total{reason=\"applied\"} 31.000000\n"
      "mempool_ext_removed_total{reason=\"expired\"} 32.000000\n"
      "mempool_ext_removed_total{reason=\"rejected_final\"} 33.000000\n"
      "mempool_ext_removed_total{reason=\"filtered\"} 34.000000\n"
      "mempool_ext_removed_total{reason=\"pool_pressure\"} 35.000000\n"
      "# TYPE applied_ext_messages counter\n"
      "applied_ext_messages_total{chain=\"master\"} 41.000000\n"
      "applied_ext_messages_total{chain=\"shard\"} 43.000000\n",
      out);
}

TEST(Metrics, ActorCollectorIncludesLiveBusyTimeAndOmitsOwnLiveness) {
  namespace actor_core = td::actor::core;
  class MetricsActor final : public td::actor::Actor {};

  auto was_debug_enabled = actor_core::need_debug();
  actor_core::set_debug(true);
  SCOPE_EXIT {
    actor_core::set_debug(was_debug_enabled);
  };
  auto group = std::make_shared<actor_core::SchedulerGroupInfo>(1);
  actor_core::Scheduler scheduler{group, actor_core::SchedulerId{0}, 1};
  scheduler.start();

  ActorMetrics actors;
  std::string during;
  std::string after;
  scheduler.run_in_context([&] {
    MetricsActor actor;
    auto stat = actor_core::ActorTypeStatManager::get_actor_type_stat(
        actor_core::ActorTypeStatImpl::get_unique_id<MetricsActor>(), &actor);
    stat.created();
    stat.start_execute();
    {
      auto message = stat.create_message_timer();
    }
    stat.finish_execute();
    stat.destroyed();

    {
      // Simulate collection inside the exporter actor. Busy time includes the live scope, while
      // liveness omits the collector itself.
      auto exporter_execution = actor_core::SchedulerContext::get().get_debug().start("metrics-exporter");
      Sink sink;
      Context(sink).collect(actors, "ton_actor");
      during = std::move(sink).build().render();
    }
    Sink sink;
    Context(sink).collect(actors, "ton_actor");
    after = std::move(sink).build().render();
  });

  ASSERT_TRUE(has_line(during, "ton_actor_scheduler_workers_active{scheduler=\"0\"} 0.000000"));
  ASSERT_TRUE(has_line(during, "ton_actor_scheduler_current_execute_seconds{scheduler=\"0\",actor=\"\"} 0.000000"));
  ASSERT_TRUE(during.find("ton_actor_worker_busy_ticks_total{worker=\"io\"} 0.000000\n") == std::string::npos);
  ASSERT_TRUE(during.find("ton_actor_worker_busy_ticks_total{worker=\"io\"} ") != std::string::npos);
  ASSERT_TRUE(after.find("ton_actor_worker_busy_ticks_total{worker=\"io\"} 0.000000\n") == std::string::npos);
  ASSERT_TRUE(after.find("ton_actor_worker_busy_ticks_total{worker=\"io\"} ") != std::string::npos);
  auto type = actor_core::ActorTypeStatManager::get_class_name(typeid(MetricsActor).name());
  ASSERT_TRUE(has_line(after, PSTRING() << "ton_actor_messages_total{type=\"" << type << "\"} 1.000000"));
  ASSERT_TRUE(has_line(after, PSTRING() << "ton_actor_executions_total{type=\"" << type << "\"} 1.000000"));
  ASSERT_TRUE(after.find(PSTRING() << "ton_actor_max_message_ticks{type=\"" << type << "\",window=\"recent\"} ") !=
              std::string::npos);
  ASSERT_TRUE(after.find("window=\"10m\"") == std::string::npos);
  ASSERT_TRUE(has_line(after, "ton_actor_worker_messages_total{worker=\"io\"} 1.000000"));
  ASSERT_TRUE(has_line(after, "ton_actor_worker_messages_total{worker=\"cpu\"} 0.000000"));
  ASSERT_TRUE(has_line(after, "ton_actor_worker_threads{worker=\"io\"} 1.000000"));
  ASSERT_TRUE(has_line(after, "ton_actor_worker_threads{worker=\"cpu\"} 1.000000"));
  ASSERT_TRUE(has_line(after, "ton_actor_scheduler_threads{scheduler=\"0\"} 2.000000"));
  ASSERT_TRUE(has_line(after, "ton_actor_scheduler_local_queue_length{scheduler=\"0\"} 0.000000"));
  ASSERT_TRUE(after.find("worker=\"other\"") == std::string::npos);
  ASSERT_EQ(1u, count_of(during, "ton_actor_scheduler_workers_active{scheduler=\"0\"}"));
  ASSERT_EQ(1u, count_of(during, "ton_actor_scheduler_current_execute_seconds{scheduler=\"0\""));

  scheduler.stop();
  ASSERT_TRUE(!scheduler.run(0));
  actor_core::Scheduler::close_scheduler_group(*group);
}

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

// ===== Collect-at-scrape exporter =====
//
// A scrape runs the collectors and is answered from that run, so these tests drive the exporter's
// scheduler by hand and scrape it over a real socket the way Prometheus does. What they assert is
// the order of scrapes and gathers, never a duration.

class ProbeCollector final : public td::actor::Actor {
 public:
  // Read from the test thread while the collector runs on the scheduler's.
  using Calls = std::shared_ptr<std::atomic<int>>;

  explicit ProbeCollector(Calls calls) : calls_(std::move(calls)) {
  }

  // Emits one recognizable family, then keeps the gather in flight for as long as the test wants
  // the scrapes sharing it to pile up, and fails it if the test asked for a failing collector.
  td::actor::Task<> collect(Context ctx) {
    calls_->fetch_add(1, std::memory_order_relaxed);
    ctx.collect(probe_, "stub_probe");
    while (blocked_) {
      co_await td::actor::coro_sleep(td::Timestamp::in(0.01));
    }
    if (failing_) {
      co_return td::Status::Error("the probe collector refused");
    }
    co_return {};
  }

  void set_blocked(bool blocked) {
    blocked_ = blocked;
  }

  void set_failing(bool failing) {
    failing_ = failing;
  }

 private:
  Counter probe_{7};
  Calls calls_;
  bool blocked_ = false;
  bool failing_ = false;
};

// A collector that registers after the endpoint is already bound, the way validator-engine's
// subsystems do. Its family is exactly what a gather started before the node declared itself ready
// would have left out of the exposition.
class LateCollector final : public td::actor::Actor {
 public:
  td::actor::Task<> collect(Context ctx) {
    ctx.collect(late_, "stub_late");
    co_return {};
  }

 private:
  Counter late_{3};
};

struct Reply {
  int code = 0;
  std::string head;
  std::string body;
};

td::Result<std::string> decode_chunked(std::string_view raw) {
  std::string body;
  while (true) {
    auto eol = raw.find("\r\n");
    if (eol == std::string_view::npos) {
      return td::Status::Error("truncated chunk header");
    }
    size_t size = 0;
    if (std::from_chars(raw.data(), raw.data() + eol, size, 16).ec != std::errc{}) {
      return td::Status::Error("malformed chunk header");
    }
    raw.remove_prefix(eol + 2);
    if (size == 0) {
      return body;  // the trailer that follows carries nothing this test needs
    }
    if (raw.size() < size + 2) {
      return td::Status::Error("truncated chunk");
    }
    body.append(raw.substr(0, size));
    raw.remove_prefix(size + 2);
  }
}

// Whether everything the response announced has arrived, without waiting for the peer to close.
bool response_complete(const std::string &raw) {
  constexpr std::string_view content_length = "Content-Length: ";
  auto split = raw.find("\r\n\r\n");
  if (split == std::string::npos) {
    return false;
  }
  std::string_view head{raw.data(), split};
  std::string_view body{raw.data() + split + 4, raw.size() - split - 4};
  if (head.find("Transfer-Encoding: Chunked") != std::string_view::npos) {
    return body.starts_with("0\r\n\r\n") || body.find("\r\n0\r\n\r\n") != std::string_view::npos;
  }
  auto at = head.find(content_length);
  if (at == std::string_view::npos) {
    return false;
  }
  size_t length = 0;
  std::from_chars(head.data() + at + content_length.size(), head.data() + head.size(), length);
  return body.size() >= length;
}

td::Result<Reply> parse_response(std::string raw) {
  auto split = raw.find("\r\n\r\n");
  if (split == std::string::npos) {
    return td::Status::Error(PSLICE() << "malformed response: " << raw);
  }
  Reply reply{.head = raw.substr(0, split), .body = raw.substr(split + 4)};
  auto code_at = reply.head.find(' ');
  if (code_at == std::string::npos) {
    return td::Status::Error(PSLICE() << "malformed status line: " << reply.head);
  }
  reply.code = td::to_integer<int>(td::Slice(reply.head).substr(code_at + 1));
  if (reply.head.find("Transfer-Encoding: Chunked") != std::string::npos) {
    TRY_RESULT_ASSIGN(reply.body, decode_chunked(reply.body));
  }
  return reply;
}

// Delivery window for requests already written to their sockets: writing to a socket is not the
// same as the exporter actor having taken the request. Nothing is waited *for* here — a caller of
// this is pinning the gather, so nothing can be answered meanwhile — and whether the scrapes really
// were concurrent is asserted afterwards, from the number of gathers they cost.
constexpr double kDeliveryWindow = 0.05;

// `count` scrapes over separate connections, every request issued before any answer is read back, so
// the exporter has them all outstanding at once. Responses are produced by actors on this thread, so
// the scheduler is pumped between polls; `outstanding` runs once the requests have been delivered,
// which is where a test pinning the gather lets it go.
td::Result<std::vector<Reply>> http_get_n(const td::IPAddress &addr, td::Slice target, size_t count,
                                          const std::function<void()> &pump, const std::function<void()> &outstanding,
                                          double timeout = 5.0) {
  struct Scrape {
    td::SocketFd fd;
    size_t sent = 0;
    std::string raw;
    bool done = false;
  };

  std::vector<Scrape> scrapes;
  scrapes.reserve(count);
  td::Poll poll;
  poll.init();
  SCOPE_EXIT {
    for (auto &scrape : scrapes) {
      poll.unsubscribe_before_close(scrape.fd.get_poll_info().get_pollable_fd_ref());
    }
    poll.clear();
  };
  for (size_t i = 0; i < count; i++) {
    TRY_RESULT(fd, td::SocketFd::open(addr));
    scrapes.push_back({.fd = std::move(fd)});
    poll.subscribe(scrapes.back().fd.get_poll_info().extract_pollable_fd(nullptr), td::PollFlags::ReadWrite());
  }

  std::string request = PSTRING() << "GET " << target << " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  auto deadline = td::Timestamp::in(timeout);
  std::optional<td::Timestamp> delivered;
  bool released = false;
  while (true) {
    if (deadline.is_in_past()) {
      return td::Status::Error("the scrape timed out");
    }
    poll.run(5);
    pump();

    size_t sent = 0, done = 0;
    for (auto &scrape : scrapes) {
      if (scrape.sent < request.size()) {
        if (td::can_close(scrape.fd) || scrape.fd.get_poll_info().get_flags_local().has_pending_error()) {
          TRY_STATUS(scrape.fd.get_pending_error());  // the listener is not up yet: fail fast
          return td::Status::Error("the connection was refused");
        }
        if (td::can_write(scrape.fd)) {
          TRY_RESULT(size, scrape.fd.write(td::Slice(request).substr(scrape.sent)));
          scrape.sent += size;
        }
        if (scrape.sent < request.size()) {
          continue;
        }
      }
      sent++;
      while (td::can_read(scrape.fd)) {
        char buffer[4096];
        TRY_RESULT(size, scrape.fd.read(td::MutableSlice(buffer, sizeof(buffer))));
        if (size == 0) {
          break;
        }
        scrape.raw.append(buffer, size);
      }
      scrape.done = response_complete(scrape.raw);
      if (!scrape.done && td::can_close(scrape.fd)) {
        return td::Status::Error(PSLICE() << "the connection closed on an incomplete response: " << scrape.raw);
      }
      done += scrape.done;
    }
    if (done == count) {
      break;
    }
    if (!released && sent == count) {
      if (!delivered.has_value()) {
        delivered = td::Timestamp::in(kDeliveryWindow);
      } else if (delivered->is_in_past()) {
        released = true;
        outstanding();
      }
    }
  }

  std::vector<Reply> replies;
  for (auto &scrape : scrapes) {
    TRY_RESULT(reply, parse_response(std::move(scrape.raw)));
    replies.push_back(std::move(reply));
  }
  return replies;
}

// One scrape over a real connection, the way Prometheus makes it.
td::Result<Reply> http_get(const td::IPAddress &addr, td::Slice target, const std::function<void()> &pump,
                           double timeout = 5.0) {
  TRY_RESULT(replies, http_get_n(addr, target, 1, pump, [] {}, timeout));
  return std::move(replies[0]);
}

// A port nothing is listening on: bound, released, and handed to the exporter (which sets
// SO_REUSEADDR, so the immediate rebind is safe).
td::int32 free_port() {
  for (int attempt = 0; attempt < 100; attempt++) {
    auto port = static_cast<td::int32>(20000 + td::Random::fast(0, 20000));
    if (td::ServerSocketFd::open(port, td::CSlice("127.0.0.1")).is_ok()) {
      return port;
    }
  }
  LOG(FATAL) << "found no free port to run the exporter on";
  UNREACHABLE();
}

class ExporterUnderTest {
 public:
  // Everything but the readiness test registers its collectors before binding and is ready at once,
  // which is the state every other contract below is about.
  explicit ExporterUnderTest(bool ready = true) {
    addr_.init_ipv4_port("127.0.0.1", free_port()).ensure();
    scheduler_.run_in_context([&] {
      collector_ = td::actor::create_actor<ProbeCollector>("probe", collections_);
      exporter_ = ::ton::PrometheusExporter::create("ton");
      td::actor::send_closure(exporter_.get(), &::ton::PrometheusExporter::add<ProbeCollector>, collector_.get(),
                              &ProbeCollector::collect);
      td::actor::send_closure(exporter_.get(), &::ton::PrometheusExporter::listen, addr_);
      if (ready) {
        td::actor::send_closure(exporter_.get(), &::ton::PrometheusExporter::ready);
      }
    });
  }

  ~ExporterUnderTest() {
    // A collector still holding a gather has to let go before the actors are killed, or the teardown
    // destroys a live task.
    set_blocked(false);
    pump(0.1);
    scheduler_.run_in_context([&] {
      exporter_.reset();
      collector_.reset();
      late_collector_.reset();
    });
    pump(0.1);
    // scheduler_ is destroyed last and runs until every actor is gone.
  }

  void set_blocked(bool blocked) {
    scheduler_.run_in_context(
        [&] { td::actor::send_closure(collector_.get(), &ProbeCollector::set_blocked, blocked); });
  }

  void set_failing(bool failing) {
    scheduler_.run_in_context(
        [&] { td::actor::send_closure(collector_.get(), &ProbeCollector::set_failing, failing); });
  }

  void add_late_collector() {
    scheduler_.run_in_context([&] {
      late_collector_ = td::actor::create_actor<LateCollector>("late");
      td::actor::send_closure(exporter_.get(), &::ton::PrometheusExporter::add<LateCollector>, late_collector_.get(),
                              &LateCollector::collect);
    });
  }

  void declare_ready() {
    scheduler_.run_in_context([&] { td::actor::send_closure(exporter_.get(), &::ton::PrometheusExporter::ready); });
  }

  td::Result<Reply> scrape() {
    return http_get(addr_, "/metrics", [this] { scheduler_.run(0.005); });
  }

  // `count` scrapes outstanding at the same time; `outstanding` runs once the exporter has them all.
  td::Result<std::vector<Reply>> scrape_together(size_t count, const std::function<void()> &outstanding) {
    return http_get_n(addr_, "/metrics", count, [this] { scheduler_.run(0.005); }, outstanding);
  }

  // Scrapes until `accept` likes the reply, driving the scheduler between attempts so that the
  // listener bind can happen.
  td::Result<Reply> scrape_until(const std::function<bool(const Reply &)> &accept, double timeout = 20.0) {
    auto deadline = td::Timestamp::in(timeout);
    auto last = td::Status::Error("no scrape was attempted");
    while (!deadline.is_in_past()) {
      auto r_reply = scrape();
      if (r_reply.is_error()) {
        last = r_reply.move_as_error();
      } else if (accept(r_reply.ok())) {
        return r_reply.move_as_ok();
      } else {
        last = td::Status::Error(PSLICE() << "unacceptable reply: " << r_reply.ok().head);
      }
      scheduler_.run(0.01);
    }
    return std::move(last);
  }

  void pump(double seconds) {
    auto until = td::Timestamp::in(seconds);
    while (!until.is_in_past()) {
      scheduler_.run(0.01);
    }
  }

  // Runs the scheduler until `ready`, or until the deadline says it never will.
  bool pump_until(const std::function<bool()> &ready, double timeout) {
    auto deadline = td::Timestamp::in(timeout);
    while (!deadline.is_in_past() && !ready()) {
      scheduler_.run(0.01);
    }
    return ready();
  }

  // No gather starts within a window many times the actor hop that starting one takes — the bounded
  // form a negative claim has to take.
  bool no_gather_starts() {
    auto before = collections();
    return !pump_until([&] { return collections() > before; }, 0.05);
  }

  // How many times a collector has been asked for its metrics, i.e. how many gathers have started.
  int collections() const {
    return collections_->load(std::memory_order_relaxed);
  }

 private:
  ProbeCollector::Calls collections_ = std::make_shared<std::atomic<int>>(0);
  td::actor::Scheduler scheduler_{{1}};
  td::IPAddress addr_;
  td::actor::ActorOwn<ProbeCollector> collector_;
  td::actor::ActorOwn<LateCollector> late_collector_;
  td::actor::ActorOwn<::ton::PrometheusExporter> exporter_;
};

bool answered_ok(const Reply &reply) {
  return reply.code == 200;
}

TEST(MetricsExporter, ScrapeIsAnsweredFromItsOwnGather) {
  ExporterUnderTest exporter;
  auto reply = exporter.scrape_until(answered_ok).move_as_ok();

  ASSERT_TRUE(reply.head.find("Content-Type: application/openmetrics-text") != std::string::npos);
  ASSERT_TRUE(has_line(reply.body, "ton_stub_probe_total 7.000000"));
  // One gather produced this response, so the scraper gets one whole exposition: the terminator
  // appears once and last.
  ASSERT_TRUE(reply.body.ends_with("# EOF\n"));
  ASSERT_EQ(1u, count_of(reply.body, "# EOF"));

  // And the next scrape is fresh data rather than these bytes again: it pays for its own gather.
  auto before = exporter.collections();
  auto again = exporter.scrape().move_as_ok();
  ASSERT_EQ(200, again.code);
  ASSERT_EQ(before + 1, exporter.collections());
}

TEST(MetricsExporter, NothingIsGatheredUntilTheCollectorSetIsDeclaredComplete) {
  ExporterUnderTest exporter{/* ready = */ false};

  // The endpoint is bound while the node is still bringing subsystems up, so a scrape can land here.
  // It must be told "not ready" and must not start a gather: that gather would render whichever
  // collectors happened to have registered by then, and the families missing from it would appear
  // one scrape later and read as rate() artifacts.
  ASSERT_EQ(503, exporter.scrape_until([](const Reply &reply) { return reply.code == 503; }).move_as_ok().code);
  ASSERT_TRUE(exporter.no_gather_starts());

  exporter.add_late_collector();  // exactly the registration such a gather would have missed
  ASSERT_EQ(503, exporter.scrape().move_as_ok().code);
  ASSERT_TRUE(exporter.no_gather_starts());
  ASSERT_EQ(0, exporter.collections());

  exporter.declare_ready();
  // The seal makes the collector set final, so the first exposition this node ever serves is
  // complete by construction — the late collector's family included — and this scrape waits for it.
  auto served = exporter.scrape().move_as_ok();
  ASSERT_EQ(200, served.code);
  ASSERT_TRUE(has_line(served.body, "ton_stub_probe_total 7.000000"));
  ASSERT_TRUE(has_line(served.body, "ton_stub_late_total 3.000000"));
  ASSERT_EQ(1, exporter.collections());
}

TEST(MetricsExporter, ConcurrentScrapesShareOneGather) {
  ExporterUnderTest exporter;
  exporter.scrape_until(answered_ok).ensure();
  auto gathers = exporter.collections();

  // The gather the first of the four starts stays in flight until all four are outstanding, which
  // is the state the single flight is about.
  exporter.set_blocked(true);
  auto replies = exporter.scrape_together(4, [&] { exporter.set_blocked(false); }).move_as_ok();

  // Four answered scrapes against one gather: every one of them was waiting on it when it landed —
  // a scrape that had arrived after it would have paid for a gather of its own.
  ASSERT_EQ(gathers + 1, exporter.collections());
  ASSERT_EQ(size_t{4}, replies.size());
  for (auto &reply : replies) {
    ASSERT_EQ(200, reply.code);
    ASSERT_TRUE(has_line(reply.body, "ton_stub_probe_total 7.000000"));
    ASSERT_TRUE(has_line(reply.body, "ton_exporter_collections_total 2.000000"));
    ASSERT_EQ(replies[0].body, reply.body);  // one gather, one rendering, one body for all of them
  }
}

TEST(MetricsExporter, AFailedGatherAnswersEveryWaiter) {
  ExporterUnderTest exporter;
  exporter.scrape_until(answered_ok).ensure();

  // A collector that errors out fails the gather three scrapes are waiting on. Every one of them
  // has to be answered: an unanswered waiter would hold the flight and wedge the endpoint for good.
  exporter.set_failing(true);
  exporter.set_blocked(true);
  auto replies = exporter.scrape_together(3, [&] { exporter.set_blocked(false); }).move_as_ok();
  ASSERT_EQ(size_t{3}, replies.size());
  for (auto &reply : replies) {
    ASSERT_EQ(500, reply.code);
  }

  // The flight was released with them, so the next scrape gathers again rather than hanging.
  exporter.set_failing(false);
  auto before = exporter.collections();
  auto recovered = exporter.scrape().move_as_ok();
  ASSERT_EQ(200, recovered.code);
  ASSERT_TRUE(has_line(recovered.body, "ton_stub_probe_total 7.000000"));
  ASSERT_EQ(before + 1, exporter.collections());
}

}  // namespace
}  // namespace ton::metrics::test
