/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <map>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "overlay/overlays.h"
#include "rldp2/rldp-utils.h"
#include "td/utils/Status.h"
#include "td/utils/logging.h"

#include "bus.h"

namespace ton::validator::consensus {

namespace tl {

using requestError = ton_api::consensus_requestError;
using RequestErrorRef = tl_object_ptr<requestError>;

}  // namespace tl

namespace {

class PrivateOverlayImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto& bus = *owning_bus();
    overlays_ = bus.overlays;
    rldp2_ = bus.rldp2;
    local_id_ = bus.local_id;

    std::vector<adnl::AdnlNodeIdShort> overlay_nodes;
    std::vector<td::Bits256> overlay_nodes_tl;
    std::map<PublicKeyHash, td::uint32> authorized_keys;

    for (const auto& peer : bus.validator_set) {
      adnl_id_to_peer_[peer.adnl_id] = peer;
      short_id_to_peer_[peer.short_id] = peer;
      overlay_nodes.push_back(peer.adnl_id);
      overlay_nodes_tl.push_back(peer.short_id.bits256_value());
      authorized_keys.emplace(peer.short_id, overlay::Overlays::max_fec_broadcast_size());
    }

    td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, local_id_.adnl_id);
    rldp_limit_guard_ = rldp2::PeersMtuLimitGuard(rldp2_, local_id_.adnl_id, overlay_nodes,
                                                  bus.config.max_block_size + bus.config.max_collated_data_size + 1024);

    auto overlay_seed = create_tl_object<tl::overlayId>(bus.session_id, std::move(overlay_nodes_tl));
    auto overlay_full_id = overlay::OverlayIdFull{serialize_tl_object(overlay_seed, true)};
    overlay_id_ = overlay_full_id.compute_short_id();

    overlay::OverlayOptions options;
    options.broadcast_speed_multiplier_ = bus.validator_opts->get_catchain_broadcast_speed_multiplier();
    options.private_ping_peers_ = true;
    options.twostep_broadcast_sender_ = rldp2_;
    options.send_twostep_broadcast_ = true;

    td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay_ex, local_id_.adnl_id,
                            std::move(overlay_full_id), std::move(overlay_nodes), make_callback(),
                            overlay::OverlayPrivacyRules{0, 0, std::move(authorized_keys)},
                            R"({ "type": "nullConsensus" })", std::move(options));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_id_.adnl_id, overlay_id_);
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OutgoingProtocolMessage> message) {
    auto send_to_peer = [&](const adnl::AdnlNodeIdShort& adnl_id) {
      if (adnl_id == local_id_.adnl_id) {
        return;
      }
      td::actor::send_closure(overlays_, &overlay::Overlays::send_message, adnl_id, local_id_.adnl_id, overlay_id_,
                              message->message.data.clone());
    };

    if (message->recipient) {
      CHECK(local_id_.idx != message->recipient);
      send_to_peer(message->recipient->get_using(*owning_bus()).adnl_id);
    } else {
      for (const auto& [adnl_id, _] : adnl_id_to_peer_) {
        send_to_peer(adnl_id);
      }
    }
  }

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle, std::shared_ptr<OutgoingOverlayRequest> message) {
    auto [awaiter, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    auto dst = message->destination.get_using(*owning_bus()).adnl_id;
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, dst, local_id_.adnl_id, overlay_id_, "",
                            std::move(promise), message->timeout, std::move(message->request.data));
    auto response = co_await std::move(awaiter);
    if (fetch_tl_object<tl::requestError>(response, true).is_ok()) {
      co_return td::Status::Error("Peer returned an error");
    }
    co_return ProtocolMessage{std::move(response)};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateGenerated> event) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_.adnl_id, overlay_id_,
                            local_id_.short_id, 0, event->candidate->serialize());
  }

 private:
  std::unique_ptr<overlay::Overlays::Callback> make_callback() {
    class Callback final : public overlay::Overlays::Callback {
     public:
      explicit Callback(td::actor::ActorId<PrivateOverlayImpl> owner) : owner_(owner) {
      }

      void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort, td::BufferSlice data) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::on_overlay_message, src, std::move(data));
      }

      void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::on_query, src, std::move(data), std::move(promise));
      }

      void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort, td::BufferSlice data) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::on_overlay_broadcast, src, std::move(data));
      }

      void check_broadcast(PublicKeyHash, overlay::OverlayIdShort, td::BufferSlice,
                           td::Promise<td::Unit> promise) override {
        promise.set_value(td::Unit());
      }

     private:
      td::actor::ActorId<PrivateOverlayImpl> owner_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  void on_overlay_message(adnl::AdnlNodeIdShort src_adnl_id, td::BufferSlice data) {
    auto peer = adnl_id_to_peer_.at(src_adnl_id);
    owning_bus().publish<IncomingProtocolMessage>(peer.idx, std::move(data));
  }

  void on_overlay_broadcast(PublicKeyHash src, td::BufferSlice data) {
    if (src == local_id_.short_id) {
      return;
    }

    auto& bus = *owning_bus();
    auto peer = short_id_to_peer_.at(src);
    auto maybe_candidate = RawCandidate::deserialize(std::move(data), bus, peer.idx);

    if (!maybe_candidate.is_ok()) {
      // FIXME: If we actually collected signed broadcast parts, we could have produced a
      //        MisbehaviorProof here.
      LOG(WARNING) << "MISBEHAVIOR: Failed to deserialize block candidate broadcast: "
                   << maybe_candidate.move_as_error();
      return;
    }

    // FIXME: We should first check with consensus if slot makes sense and candidate is expected and
    //        only then publish stats target.
    owning_bus().publish<StatsTargetReached>(StatsTargetReached::CandidateReceived, maybe_candidate.ok()->id.slot);
    owning_bus().publish<CandidateReceived>(maybe_candidate.move_as_ok());
  }

  void on_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
    auto peer = adnl_id_to_peer_.at(src);
    auto request = std::make_shared<IncomingOverlayRequest>(peer.idx, std::move(data));

    auto task = [](BusHandle bus, auto message, auto promise) -> td::actor::Task<> {
      auto response = co_await bus.publish(std::move(message)).wrap();
      if (response.is_ok()) {
        promise.set_value(response.move_as_ok().data);
      } else {
        LOG(WARNING) << "Failed to process overlay request from " << message->source << ": "
                     << response.move_as_error();
        promise.set_value(create_serialize_tl_object<tl::requestError>());
      }
      co_return {};
    };
    task(owning_bus(), request, std::move(promise)).start().detach();
  }

  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  overlay::OverlayIdShort overlay_id_;
  rldp2::PeersMtuLimitGuard rldp_limit_guard_;
  PeerValidator local_id_;
  std::map<adnl::AdnlNodeIdShort, PeerValidator> adnl_id_to_peer_;
  std::map<PublicKeyHash, PeerValidator> short_id_to_peer_;
};

}  // namespace

void PrivateOverlay::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<PrivateOverlayImpl>("PrivateOverlay");
}

}  // namespace ton::validator::consensus
