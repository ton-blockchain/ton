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
#include "adnl/adnl-test-network.h"
#include "dht/dht.h"
#include "overlay/overlays.h"
#include "td/actor/TestScheduler.h"
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

}  // namespace
}  // namespace ton::overlay::test
