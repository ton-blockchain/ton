/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl-sender-ex.h"
#include "adnl/adnl-test-network.h"
#include "dht/dht.h"
#include "keys/keys.hpp"
#include "overlay/overlays.h"
#include "td/actor/TestScheduler.h"
#include "tl-utils/common-utils.hpp"
#include "td/utils/tests.h"

namespace ton::overlay::test {
namespace {

struct ReceivedPackets {
  std::vector<std::string> messages;
  std::vector<std::string> queries;
};

struct QueryResultSink {
  std::optional<td::BufferSlice> answer;
  std::optional<td::Status> error;
};

class RecordingCallback final : public Overlays::Callback {
 public:
  explicit RecordingCallback(std::shared_ptr<ReceivedPackets> packets) : packets_(std::move(packets)) {
  }

  void receive_message(adnl::AdnlNodeIdShort, OverlayIdShort, td::BufferSlice data) override {
    packets_->messages.push_back(data.as_slice().str());
  }

  void receive_query(adnl::AdnlNodeIdShort, OverlayIdShort, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    packets_->queries.push_back(data.as_slice().str());
    promise.set_value(td::BufferSlice("buffered-query-answer"));
  }

 private:
  std::shared_ptr<ReceivedPackets> packets_;
};

class OverlayManagerHarness {
 public:
  explicit OverlayManagerHarness(OverlayManagerBufferLimits limits = {.max_packets = 8, .max_data_size = 4096})
      : network_("tmp-dir-test-overlay-manager") {
    auto& sender_node = network_.add_node();
    auto& receiver_node = network_.add_node();
    network_.connect(sender_node, receiver_node);

    sender_adnl_short_ = sender_node.adnl_short;
    receiver_adnl_short_ = receiver_node.adnl_short;
    peers_ = {sender_adnl_short_, receiver_adnl_short_};

    sender_ = Overlays::create(sender_node.db_root, sender_node.keyring.get(), sender_node.adnl.get(),
                               td::actor::ActorId<dht::Dht>{});
    receiver_ = Overlays::create(receiver_node.db_root, receiver_node.keyring.get(), receiver_node.adnl.get(),
                                 td::actor::ActorId<dht::Dht>{}, limits);

    // FIXME: Otherwise Overlays doesn't register ADNL callbacks for overlay packets.
    create_receiver_overlay("sentinel", std::make_shared<ReceivedPackets>());
  }

  ~OverlayManagerHarness() {
    sender_.reset();
    receiver_.reset();
  }

  OverlayIdShort create_sender_overlay(td::Slice name) {
    auto id = make_overlay_id(name);
    auto id_short = id.compute_short_id();
    td::actor::send_closure(sender_, &Overlays::create_private_overlay, sender_adnl_short_, std::move(id), peers_,
                            std::make_unique<RecordingCallback>(std::make_shared<ReceivedPackets>()),
                            OverlayPrivacyRules(0, 0, {}), "");
    return id_short;
  }

  void create_receiver_overlay(td::Slice name, std::shared_ptr<ReceivedPackets> packets) {
    td::actor::send_closure(receiver_, &Overlays::create_private_overlay, receiver_adnl_short_, make_overlay_id(name),
                            peers_, std::make_unique<RecordingCallback>(std::move(packets)),
                            OverlayPrivacyRules(0, 0, {}), "");
  }

  void send_message(OverlayIdShort overlay, td::Slice payload) {
    td::actor::send_closure(sender_, &Overlays::send_message, receiver_adnl_short_, sender_adnl_short_, overlay,
                            td::BufferSlice(payload));
  }

  std::shared_ptr<QueryResultSink> send_query(OverlayIdShort overlay, td::Slice payload) {
    auto sink = std::make_shared<QueryResultSink>();
    td::actor::send_closure(sender_, &Overlays::send_query, receiver_adnl_short_, sender_adnl_short_, overlay, "test",
                            td::PromiseCreator::lambda([sink](td::Result<td::BufferSlice> result) {
                              if (result.is_ok()) {
                                sink->answer = result.move_as_ok();
                              } else {
                                sink->error = result.move_as_error();
                              }
                            }),
                            td::Timestamp::in(10.0), td::BufferSlice(payload));
    return sink;
  }

 private:
  static OverlayIdFull make_overlay_id(td::Slice name) {
    return OverlayIdFull{create_serialize_tl_object<ton_api::pub_overlay>(td::BufferSlice(name))};
  }

  adnl::TestNetwork network_;
  td::actor::ActorOwn<Overlays> sender_;
  td::actor::ActorOwn<Overlays> receiver_;
  adnl::AdnlNodeIdShort sender_adnl_short_;
  adnl::AdnlNodeIdShort receiver_adnl_short_;
  std::vector<adnl::AdnlNodeIdShort> peers_;
};

class NoopAdnlSenderEx final : public adnl::AdnlSenderEx {
 public:
  void send_message(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, td::BufferSlice) override {
  }
  void send_query(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, std::string, td::Promise<td::BufferSlice> promise,
                  td::Timestamp, td::BufferSlice) override {
    promise.set_error(td::Status::Error("NoopAdnlSenderEx::send_query"));
  }
  void send_query_ex(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, std::string,
                     td::Promise<td::BufferSlice> promise, td::Timestamp, td::BufferSlice, td::uint64) override {
    promise.set_error(td::Status::Error("NoopAdnlSenderEx::send_query_ex"));
  }
  void get_conn_ip_str(adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort, td::Promise<td::string> promise) override {
    promise.set_value("noop");
  }
  void add_id(adnl::AdnlNodeIdShort) override {
  }

 private:
  void on_mtu_updated(td::optional<adnl::AdnlNodeIdShort>, td::optional<adnl::AdnlNodeIdShort>) override {
  }
};

class BroadcastOnlyCallback final : public Overlays::Callback {
 public:
  void receive_broadcast(PublicKeyHash, OverlayIdShort, td::BufferSlice) override {
  }
};

static OverlayIdFull make_overlay_id(td::Slice name) {
  return OverlayIdFull{create_serialize_tl_object<ton_api::pub_overlay>(td::BufferSlice(name))};
}

static td::uint64 get_unauthorized_broadcast_count(
    const tl_object_ptr<ton_api::engine_validator_overlaysStats>& stats, OverlayIdShort overlay_id,
    adnl::AdnlNodeIdShort local_id) {
  for (const auto& overlay_stats : stats->overlays_) {
    if (overlay_stats->overlay_id_ != overlay_id.bits256_value() || overlay_stats->adnl_id_ != local_id.bits256_value()) {
      continue;
    }
    for (const auto& broadcasts_stats : overlay_stats->broadcasts_) {
      if (broadcasts_stats->source_.is_zero()) {
        return broadcasts_stats->count_;
      }
    }
  }
  return 0;
}

static td::BufferSlice make_twostep_fec_broadcast(PrivateKey& src_key, adnl::AdnlNodeIdShort src_adnl_id,
                                                  adnl::AdnlNodeIdShort bcast_src_adnl_id, td::uint32 date,
                                                  td::uint32 seqno, td::uint32 data_size, td::uint32 part_size,
                                                  td::Bits256 data_hash, td::BufferSlice extra) {
  const td::uint32 flags = 0;
  PublicKey src_public = src_key.compute_public_key();
  PublicKeyHash src_keyhash = src_public.compute_short_id();
  td::BufferSlice part(part_size);
  td::Random::secure_bytes(part.as_slice());
  td::Bits256 broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
      flags, date, src_keyhash.bits256_value(), bcast_src_adnl_id.bits256_value(), data_hash,
      static_cast<td::int32>(data_size), static_cast<td::int32>(part_size), extra.clone()));

  td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec_toSign>(
      broadcast_id, static_cast<td::int32>(seqno), part.clone());
  auto decryptor = src_key.create_decryptor().move_as_ok();
  td::BufferSlice signature = decryptor->sign(to_sign.as_slice()).move_as_ok();

  return create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec>(
      flags, date, src_public.tl(), src_adnl_id.bits256_value(), Certificate::empty_tl(), data_hash,
      static_cast<td::int32>(data_size), static_cast<td::int32>(seqno), std::move(part), std::move(extra),
      std::move(signature));
}

TEST(Overlays, BuffersUnknownOverlayPacketsUntilCreation) {
  // Covers tracker Kernel-31 commit a4c2dc4e2:
  // incoming messages and queries for an unknown overlay must be buffered and replayed
  // once that overlay is created on the same local ADNL id.
  td::actor::TestScheduler ts;
  ts.run([&]() -> td::actor::Task<td::Unit> {
    OverlayManagerHarness harness;
    auto overlay = harness.create_sender_overlay("test-overlay");

    // Send a message and a query before the receiver overlay exists.
    harness.send_message(overlay, "buffered-message");
    auto query_sink = harness.send_query(overlay, "buffered-query");

    // Nothing delivered yet.
    co_await ts.wait_sync_work();
    EXPECT(!query_sink->answer.has_value());
    EXPECT(!query_sink->error.has_value());

    // Create the receiver overlay — buffered packets should be flushed.
    auto packets = std::make_shared<ReceivedPackets>();
    harness.create_receiver_overlay("test-overlay", packets);

    co_await ts.wait_sync_work();
    EXPECT_EQ(packets->messages, std::vector<std::string>{"buffered-message"});
    EXPECT_EQ(packets->queries, std::vector<std::string>{"buffered-query"});
    ASSERT_TRUE(query_sink->answer.has_value());
    EXPECT_EQ(query_sink->answer->as_slice(), "buffered-query-answer");

    // Verify the non-buffered path works too.
    auto live_sink = harness.send_query(overlay, "live-query");

    co_await ts.wait_sync_work();
    ASSERT_TRUE(live_sink->answer.has_value());
    EXPECT_EQ(live_sink->answer->as_slice(), "buffered-query-answer");
    EXPECT_EQ(packets->queries, std::vector<std::string>({"buffered-query", "live-query"}));

    co_return {};
  });
}

TEST(Overlays, EvictsOldestBufferedPacketsOnOverflow) {
  // When the buffer is full, the oldest packets (across all overlays) are evicted.
  // This test sends messages to two unknown overlays, overflows the buffer, then
  // creates the overlays one at a time and verifies that only recent messages survive.
  td::actor::TestScheduler ts;
  ts.run([&]() -> td::actor::Task<td::Unit> {
    OverlayManagerHarness harness({.max_packets = 8, .max_data_size = 64 * 1024});
    auto overlay_a = harness.create_sender_overlay("overlay-a");
    auto overlay_b = harness.create_sender_overlay("overlay-b");

    // Send 5 messages to overlay A, then 5 to overlay B (10 total, limit 8). a-0 and a-1
    // get evicted to make room for b-3 and b-4.
    for (int i = 0; i < 5; ++i) {
      harness.send_message(overlay_a, PSTRING() << "a-" << i);
    }
    for (int i = 0; i < 5; ++i) {
      harness.send_message(overlay_b, PSTRING() << "b-" << i);
    }

    auto packets_a = std::make_shared<ReceivedPackets>();
    co_await ts.wait_sync_work();
    harness.create_receiver_overlay("overlay-a", packets_a);

    co_await ts.wait_sync_work();
    EXPECT_EQ(packets_a->messages, (std::vector<std::string>{"a-2", "a-3", "a-4"}));

    // Buffer now has 5 old b-* messages. Send 5 more → 10 total, 2 evicted (b-0, b-1).
    for (int i = 5; i < 10; ++i) {
      harness.send_message(overlay_b, PSTRING() << "b-" << i);
    }

    auto packets_b = std::make_shared<ReceivedPackets>();
    co_await ts.wait_sync_work();
    harness.create_receiver_overlay("overlay-b", packets_b);

    co_await ts.wait_sync_work();
    EXPECT_EQ(packets_b->messages, (std::vector<std::string>{"b-2", "b-3", "b-4", "b-5", "b-6", "b-7", "b-8", "b-9"}));

    co_return {};
  });
}

TEST(Overlays, TwostepFecEvictionMarksOldestAsDelivered) {
  // Re-sending the oldest twostep-FEC packet after cache overflow must be treated as duplicate.
  // We verify this through unauthorized broadcast stats:
  // - 21 unique broadcasts increment count to 21
  // - resending the evicted oldest one must NOT increment count to 22
  td::actor::TestScheduler ts;
  ts.run([&]() -> td::actor::Task<td::Unit> {
    adnl::TestNetwork network("tmp-dir-test-overlay-twostep-gc");
    auto& sender_node = network.add_node();
    auto& receiver_node = network.add_node();
    network.connect(sender_node, receiver_node);

    std::vector<adnl::AdnlNodeIdShort> peers = {sender_node.adnl_short, receiver_node.adnl_short};

    auto sender_manager = Overlays::create(sender_node.db_root, sender_node.keyring.get(), sender_node.adnl.get(),
                                           td::actor::ActorId<dht::Dht>{});
    auto receiver_manager = Overlays::create(receiver_node.db_root, receiver_node.keyring.get(), receiver_node.adnl.get(),
                                             td::actor::ActorId<dht::Dht>{});
    auto twostep_sender = td::actor::create_actor<NoopAdnlSenderEx>("twostep-noop-sender");

    OverlayPrivacyRules rules(1 << 20, CertificateFlags::AllowFec | CertificateFlags::Trusted, {});

    auto overlay_id_full = make_overlay_id("twostep-cache-cap-test");
    auto overlay_id = overlay_id_full.compute_short_id();
    td::actor::send_closure(sender_manager, &Overlays::create_private_overlay, sender_node.adnl_short, overlay_id_full.clone(),
                            peers, std::make_unique<BroadcastOnlyCallback>(), rules, "");

    OverlayOptions receiver_opts;
    receiver_opts.twostep_broadcast_sender_ = twostep_sender.get();
    td::actor::send_closure(receiver_manager, &Overlays::create_private_overlay_ex, receiver_node.adnl_short,
                            overlay_id_full.clone(), peers, std::make_unique<BroadcastOnlyCallback>(), rules, "",
                            receiver_opts);

    co_await ts.wait_sync_work();

    PrivateKey src_key{privkeys::Ed25519::random()};
    td::Bits256 bcast_src_bits = td::Bits256::zero();
    bcast_src_bits.as_slice().copy_from(td::Slice("twostep-gc-sender..............000001", 32));
    adnl::AdnlNodeIdShort bcast_src_adnl_id{bcast_src_bits};
    td::uint32 date = static_cast<td::uint32>(td::Clocks::system());

    static constexpr td::uint32 data_size = 512;
    static constexpr td::uint32 part_size = 128;
    static constexpr td::uint32 seqno = 0;

    std::vector<td::Bits256> data_hashes;
    data_hashes.reserve(21);
    for (size_t i = 0; i < 21; ++i) {
      td::Bits256 data_hash = td::sha256_bits256(PSTRING() << "twostep-gc-data-" << i);
      data_hashes.push_back(data_hash);
      auto packet = make_twostep_fec_broadcast(src_key, sender_node.adnl_short, bcast_src_adnl_id, date, seqno, data_size,
                                               part_size, data_hash, td::BufferSlice());
      td::actor::send_closure(sender_manager, &Overlays::send_message, receiver_node.adnl_short, sender_node.adnl_short,
                              overlay_id, std::move(packet));
    }
    co_await ts.wait_sync_work();

    auto [stats_task_before, stats_promise_before] =
        td::actor::StartedTask<tl_object_ptr<ton_api::engine_validator_overlaysStats>>::make_bridge();
    td::actor::send_closure(receiver_manager, &Overlays::get_stats, std::move(stats_promise_before));
    auto stats_before = co_await std::move(stats_task_before);
    EXPECT_EQ(get_unauthorized_broadcast_count(stats_before, overlay_id, receiver_node.adnl_short), 21);

    auto oldest_packet = make_twostep_fec_broadcast(src_key, sender_node.adnl_short, bcast_src_adnl_id, date, seqno, data_size,
                                                    part_size, data_hashes.front(), td::BufferSlice());
    td::actor::send_closure(sender_manager, &Overlays::send_message, receiver_node.adnl_short, sender_node.adnl_short,
                            overlay_id, std::move(oldest_packet));
    co_await ts.wait_sync_work();

    auto [stats_task_after, stats_promise_after] =
        td::actor::StartedTask<tl_object_ptr<ton_api::engine_validator_overlaysStats>>::make_bridge();
    td::actor::send_closure(receiver_manager, &Overlays::get_stats, std::move(stats_promise_after));
    auto stats_after = co_await std::move(stats_task_after);
    EXPECT_EQ(get_unauthorized_broadcast_count(stats_after, overlay_id, receiver_node.adnl_short), 21);

    twostep_sender.reset();
    sender_manager.reset();
    receiver_manager.reset();
    co_return {};
  });
}

}  // namespace
}  // namespace ton::overlay::test
