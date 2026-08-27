/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <memory>
#include <string>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl-sender-ex.h"
#include "adnl/adnl-test-network.h"
#include "common/checksum.h"
#include "dht/dht.h"
#include "keys/encryptor.h"
#include "keys/keys.hpp"
#include "overlay/overlays.h"
#include "td/actor/actor.h"
#include "td/utils/Random.h"
#include "td/utils/Time.h"
#include "td/utils/tests.h"
#include "tl-utils/common-utils.hpp"

namespace ton::overlay::test {
namespace {

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

class CountingBroadcastCallback final : public Overlays::Callback {
 public:
  explicit CountingBroadcastCallback(std::shared_ptr<size_t> delivered_count) : delivered_count_(std::move(delivered_count)) {
  }

  void receive_broadcast(PublicKeyHash, OverlayIdShort, td::BufferSlice) override {
    ++(*delivered_count_);
  }

 private:
  std::shared_ptr<size_t> delivered_count_;
};

static void run_scheduler_for(td::actor::Scheduler& scheduler, double seconds) {
  auto deadline = td::Timestamp::in(seconds);
  while (scheduler.run(1)) {
    if (deadline.is_in_past()) {
      break;
    }
  }
}

static OverlayIdFull make_overlay_id(td::Slice name) {
  return OverlayIdFull{create_serialize_tl_object<ton_api::pub_overlay>(td::BufferSlice(name))};
}

static td::BufferSlice make_twostep_fec_broadcast(PrivateKey& src_key, adnl::AdnlNodeIdShort src_adnl_id,
                                                  adnl::AdnlNodeIdShort bcast_src_adnl_id, td::uint32 date,
                                                  td::uint32 seqno, td::uint32 data_size,
                                                  td::Bits256 data_hash, td::BufferSlice part) {
  const td::uint32 flags = 0;
  PublicKey src_public = src_key.compute_public_key();
  PublicKeyHash src_keyhash = src_public.compute_short_id();

  td::Bits256 broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::overlay_broadcastTwostep_id>(
      static_cast<td::int32>(flags), static_cast<td::int32>(date), src_keyhash.bits256_value(),
      bcast_src_adnl_id.bits256_value(), data_hash, static_cast<td::int32>(part.size())));

  td::BufferSlice to_sign = create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec_toSign>(
      broadcast_id, static_cast<td::int32>(data_size), static_cast<td::int32>(seqno), part.clone());
  auto decryptor = src_key.create_decryptor().move_as_ok();
  td::BufferSlice signature = decryptor->sign(to_sign.as_slice()).move_as_ok();

  return create_serialize_tl_object<ton_api::overlay_broadcastTwostepFec>(
      static_cast<td::int32>(flags), static_cast<td::int32>(date), src_public.tl(), src_adnl_id.bits256_value(),
      Certificate::empty_tl(), data_hash, static_cast<td::int32>(data_size), static_cast<td::int32>(seqno),
      std::move(part), std::move(signature));
}

TEST(Overlays, TwostepFecEvictionMarksOldestAsDelivered) {
  td::actor::Scheduler scheduler({2});

  std::unique_ptr<adnl::TestNetwork> network;
  td::actor::ActorOwn<Overlays> sender_manager;
  td::actor::ActorOwn<Overlays> receiver_manager;
  td::actor::ActorOwn<NoopAdnlSenderEx> twostep_sender;

  adnl::AdnlNodeIdShort sender_id;
  adnl::AdnlNodeIdShort receiver_id;
  OverlayIdShort overlay_id;

  auto delivered_count = std::make_shared<size_t>(0);

  scheduler.run_in_context([&] {
    network = std::make_unique<adnl::TestNetwork>("tmp-dir-test-overlay-twostep-gc");
    auto& sender_node = network->add_node();
    auto& receiver_node = network->add_node();
    network->connect(sender_node, receiver_node);

    sender_id = sender_node.adnl_short;
    receiver_id = receiver_node.adnl_short;
    std::vector<adnl::AdnlNodeIdShort> peers = {sender_id, receiver_id};

    sender_manager =
        Overlays::create(sender_node.db_root, sender_node.keyring.get(), sender_node.adnl.get(), td::actor::ActorId<dht::Dht>{});
    receiver_manager = Overlays::create(receiver_node.db_root, receiver_node.keyring.get(), receiver_node.adnl.get(),
                                        td::actor::ActorId<dht::Dht>{});
    twostep_sender = td::actor::create_actor<NoopAdnlSenderEx>("twostep-noop-sender");

    OverlayPrivacyRules rules(1 << 20, CertificateFlags::AllowFec | CertificateFlags::Trusted, {});
    auto overlay_id_full = make_overlay_id("twostep-cache-cap-test");
    overlay_id = overlay_id_full.compute_short_id();

    td::actor::send_closure(sender_manager, &Overlays::create_private_overlay, sender_id, overlay_id_full.clone(), peers,
                            std::make_unique<CountingBroadcastCallback>(std::make_shared<size_t>(0)), rules, "");

    OverlayOptions receiver_opts;
    receiver_opts.twostep_broadcast_sender_ = twostep_sender.get();
    td::actor::send_closure(receiver_manager, &Overlays::create_private_overlay_ex, receiver_id, overlay_id_full.clone(), peers,
                            std::make_unique<CountingBroadcastCallback>(delivered_count), rules, "", receiver_opts);
  });

  run_scheduler_for(scheduler, 0.5);

  PrivateKey src_key{privkeys::Ed25519::random()};
  td::Bits256 bcast_src_bits = sha256_bits256(td::Slice("twostep-gc-src-adnl"));
  adnl::AdnlNodeIdShort bcast_src_adnl_id{bcast_src_bits};
  td::uint32 date = static_cast<td::uint32>(td::Clocks::system());
  static constexpr td::uint32 data_size = 256;
  static constexpr td::uint32 part_size = 256;

  td::BufferSlice oldest_part(part_size);
  td::Random::secure_bytes(oldest_part.as_slice());
  td::Bits256 oldest_hash = sha256_bits256(oldest_part.as_slice());

  scheduler.run_in_context([&] {
    for (size_t i = 0; i < 21; ++i) {
      td::Bits256 data_hash;
      td::BufferSlice part(part_size);
      td::uint32 seqno = 0;

      if (i == 0) {
        data_hash = oldest_hash;
        part = oldest_part.clone();
      } else {
        td::Random::secure_bytes(part.as_slice());
        data_hash = sha256_bits256(part.as_slice());
      }

      auto packet = make_twostep_fec_broadcast(src_key, sender_id, bcast_src_adnl_id, date, seqno, data_size, data_hash,
                                               std::move(part));
      td::actor::send_closure(sender_manager, &Overlays::send_message, receiver_id, sender_id, overlay_id, std::move(packet));
    }
  });

  run_scheduler_for(scheduler, 0.8);

  // Re-send oldest seqno=0 after overflow: should be dropped as duplicate if eviction registered delivered id.
  scheduler.run_in_context([&] {
    auto packet0 = make_twostep_fec_broadcast(src_key, sender_id, bcast_src_adnl_id, date, 0, data_size, oldest_hash,
                                              oldest_part.clone());
    td::actor::send_closure(sender_manager, &Overlays::send_message, receiver_id, sender_id, overlay_id,
                            std::move(packet0));
  });

  run_scheduler_for(scheduler, 0.8);
  EXPECT_EQ(*delivered_count, static_cast<size_t>(21));

  scheduler.run_in_context([&] {
    twostep_sender.reset();
    sender_manager.reset();
    receiver_manager.reset();
    network.reset();
  });
  run_scheduler_for(scheduler, 0.2);
}

}  // namespace
}  // namespace ton::overlay::test
