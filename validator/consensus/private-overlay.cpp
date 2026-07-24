/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <map>
#include <random>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "overlay/overlays.h"
#include "td/utils/Random.h"
#include "td/utils/Status.h"
#include "td/utils/logging.h"
#include "ton/ton-io.hpp"

#include "bus.h"

namespace ton::validator::consensus {

namespace tl {

using requestError = ton_api::consensus_requestError;
using RequestErrorRef = tl_object_ptr<requestError>;

}  // namespace tl

namespace {

class PrivateOverlayImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto& bus = *owning_bus();

    overlays_ = bus.overlays;
    local_adnl_id_ = bus.local_adnl_id;
    adnl_sender_ = bus.adnl_sender;

    std::vector<td::Bits256> overlay_nodes_tl;
    std::map<PublicKeyHash, td::uint32> authorized_keys;

    if (bus.config.validator_key_was_a_bad_idea()) {
      local_broadcast_id_ = bus.local_adnl_id.pubkey_hash();
    } else if (bus.is_validator()) {
      local_broadcast_id_ = bus.local_id->short_id;
    }

    td::uint32 max_broadcast_size = bus.config.max_block_size + bus.config.max_collated_data_size + (1 << 20);
    for (const auto& peer : bus.validator_set) {
      adnl_id_to_peer_[peer.adnl_id] = peer;
      overlay_nodes_tl.push_back(peer.short_id.bits256_value());
      if (bus.config.validator_key_was_a_bad_idea()) {
        broadcast_src_to_peer_[peer.adnl_id.pubkey_hash()] = peer;
        authorized_keys.emplace(peer.adnl_id.pubkey_hash(), max_broadcast_size);
      } else {
        broadcast_src_to_peer_[peer.short_id] = peer;
        authorized_keys.emplace(peer.short_id, max_broadcast_size);
      }
    }
    if (bus.config.enable_collators()) {
      for (const auto& peer : bus.all_overlay_nodes) {
        authorized_keys.emplace(peer.pubkey_hash(), max_broadcast_size);
      }
    }

    td::actor::send_closure(adnl_sender_, &adnl::AdnlSenderEx::add_id, local_adnl_id_);

    auto overlay_seed = create_tl_object<tl::overlayId>(bus.session_id, std::move(overlay_nodes_tl));
    auto overlay_full_id = overlay::OverlayIdFull{serialize_tl_object(overlay_seed, true)};
    overlay_id_ = overlay_full_id.compute_short_id();

    overlay::OverlayOptions options;
    options.name_ = PSTRING() << "valgroup" << bus.shard << "." << bus.cc_seqno;
    options.private_ping_peers_ = true;
    options.twostep_broadcast_sender_ = adnl_sender_;
    options.send_twostep_broadcast_ = true;
    options.allow_old_broadcasts_ = false;

    overlay_nodes_ = bus.all_overlay_nodes;
    td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay_ex, local_adnl_id_,
                            std::move(overlay_full_id), overlay_nodes_, make_callback(),
                            overlay::OverlayPrivacyRules{0, 0, std::move(authorized_keys)},
                            PSTRING() << R"({ "type": "consensus", "shard": ")" << bus.shard << R"(", "cc_seqno": )"
                                      << bus.cc_seqno << R"( })",
                            std::move(options));

    for (auto node : overlay_nodes_) {
      if (node != local_adnl_id_) {
        other_overlay_nodes_.push_back(node);
      }
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_adnl_id_, overlay_id_);
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const OutgoingProtocolMessage> message) {
    auto send_to_peer = [&](const adnl::AdnlNodeIdShort& adnl_id) {
      CHECK(adnl_id != local_adnl_id_);
      td::actor::send_closure(overlays_, &overlay::Overlays::send_message_via, adnl_id, local_adnl_id_, overlay_id_,
                              message->message.data.clone(), adnl_sender_);
    };

    auto broadcast_all_fn = [&](const OutgoingProtocolMessage::BroadcastToAll&) {
      for (const auto& adnl_id : other_overlay_nodes_) {
        send_to_peer(adnl_id);
      }
    };

    auto broadcast_validators_fn = [&](const OutgoingProtocolMessage::BroadcastToValidators&) {
      for (const auto& [adnl_id, _] : adnl_id_to_peer_) {
        if (adnl_id != local_adnl_id_) {
          send_to_peer(adnl_id);
        }
      }
    };

    auto gossip_fn = [&](const OutgoingProtocolMessage::BroadcastToRandom& r) {
      std::vector<adnl::AdnlNodeIdShort> selected_peers;
      std::sample(other_overlay_nodes_.begin(), other_overlay_nodes_.end(), std::back_inserter(selected_peers),
                  std::min(r.count, other_overlay_nodes_.size()), gossip_rng_);

      for (auto peer : selected_peers) {
        send_to_peer(peer);
      }
    };

    auto send_to_single_peer_fn = [&](const OutgoingProtocolMessage::SendToPeer& p) { send_to_peer(p.peer); };

    std::visit(td::overloaded(broadcast_all_fn, broadcast_validators_fn, gossip_fn, send_to_single_peer_fn),
               message->recipient);
  }

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle, std::shared_ptr<OutgoingOverlayRequest> message) {
    auto destination = message->destination;
    if (!destination) {
      CHECK(!other_overlay_nodes_.empty());
      size_t node_idx = td::Random::fast(0, static_cast<int>(other_overlay_nodes_.size()) - 1);
      destination = other_overlay_nodes_[node_idx];
    }

    auto [awaiter, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
    // FIXME: Pass max response size from the caller.
    td::actor::send_closure(
        overlays_, &overlay::Overlays::send_query_via, *destination, local_adnl_id_, overlay_id_, "",
        std::move(promise), message->timeout, std::move(message->request.data),
        owning_bus()->config.max_block_size + owning_bus()->config.max_collated_data_size + (1 << 20), adnl_sender_);
    auto response = co_await std::move(awaiter);
    if (fetch_tl_object<tl::requestError>(response, true).is_ok()) {
      co_return td::Status::Error("Peer returned an error");
    }
    co_return ProtocolMessage{std::move(response)};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateGenerated> event) {
    auto& bus = *owning_bus();

    CHECK(local_broadcast_id_);
    td::BufferSlice extra;
    if (bus.config.enable_collators()) {
      if (event->candidate->delegation) {
        extra = create_serialize_tl_object<tl::broadcastExtra>(1, event->candidate->id.slot,
                                                               event->candidate->delegation->to_tl());
      } else {
        extra = create_serialize_tl_object<tl::broadcastExtra>(0, event->candidate->id.slot, nullptr);
      }
    } else {
      extra = create_serialize_tl_object<tl::broadcastExtraLegacy>(event->candidate->id.slot);
    }
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_with_extra, local_adnl_id_, overlay_id_,
                            *local_broadcast_id_, 0, event->candidate->serialize_for_broadcast(), std::move(extra));
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

      void receive_broadcast_with_extra(PublicKeyHash src, overlay::OverlayIdShort, td::BufferSlice data,
                                        td::BufferSlice extra) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::on_overlay_broadcast, src, std::move(data),
                                std::move(extra));
      }

      void precheck_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::Bits256 broadcast_id,
                              td::BufferSlice extra, bool signature_checked, td::Promise<> promise) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::precheck_broadcast, src, broadcast_id, std::move(extra),
                                signature_checked, std::move(promise));
      }

     private:
      td::actor::ActorId<PrivateOverlayImpl> owner_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  void on_overlay_message(adnl::AdnlNodeIdShort src_adnl_id, td::BufferSlice data) {
    auto peer = adnl_id_to_peer_.find(src_adnl_id);
    std::optional<PeerValidatorId> source_validator;
    if (peer != adnl_id_to_peer_.end()) {
      source_validator = peer->second.idx;
    }

    owning_bus().publish<IncomingProtocolMessage>(source_validator, src_adnl_id, std::move(data));
  }

  void on_overlay_broadcast(PublicKeyHash src, td::BufferSlice data, td::BufferSlice extra) {
    auto& bus = *owning_bus();

    if (src == local_broadcast_id_) {
      return;
    }

    auto parsed_extra = parse_broadcast_extra(extra).move_as_ok();

    Candidate::Signer signer;
    if (parsed_extra.delegation) {
      signer = std::move(*parsed_extra.delegation);
    } else {
      auto it = broadcast_src_to_peer_.find(src);
      if (it == broadcast_src_to_peer_.end()) {
        LOG(WARNING) << "Dropping candidate broadcast from unknown source " << src;
        return;
      }
      signer = it->second.idx;
    }
    auto maybe_candidate =
        Candidate::deserialize_from_broadcast(std::move(data), std::move(signer), bus, parsed_extra.slot);

    if (maybe_candidate.is_error()) {
      // FIXME: If we actually collected signed broadcast parts, we could have produced a
      //        MisbehaviorProof here.
      LOG(WARNING) << "MISBEHAVIOR: Failed to deserialize block candidate broadcast: "
                   << maybe_candidate.move_as_error();
      return;
    }
    auto candidate = maybe_candidate.move_as_ok();

    if (!candidate->is_empty()) {
      const BlockCandidate& block = std::get<BlockCandidate>(candidate->block);
      td::actor::send_closure(bus.manager, &ManagerFacade::cache_block_candidate, block.clone());
    }

    owning_bus().publish<CandidateReceived>(std::move(candidate));
  }

  td::actor::Task<> precheck_broadcast(PublicKeyHash src, td::Bits256 broadcast_id, td::BufferSlice extra,
                                       bool signature_checked) {
    auto parsed_extra = CO_TRY(parse_broadcast_extra(extra).trace("Precheck failed: Failed to parse broadcast extra"));
    auto& bus = *owning_bus();
    auto expected_leader = bus.collator_schedule->expected_collator_for(parsed_extra.slot);
    if (parsed_extra.delegation.has_value()) {
      if (parsed_extra.delegation->collator_key.compute_short_id() != src) {
        co_return td::Status::Error("Precheck failed: Delegation collator key does not match the broadcast source");
      }
      if (signature_checked) {
        CO_TRY(check_delegation(*parsed_extra.delegation, expected_leader.get_using(bus), parsed_extra.slot, bus)
                   .trace("Precheck failed"));
      }
    } else {
      auto it = broadcast_src_to_peer_.find(src);
      if (it == broadcast_src_to_peer_.end()) {
        co_return td::Status::Error("Precheck failed: Broadcast is from an unknown source");
      }
      if (it->second.idx != expected_leader) {
        co_return td::Status::Error("Precheck failed: Broadcast is not from the expected collator");
      }
    }

    co_return co_await owning_bus()
        .publish<PrecheckCandidateBroadcast>(parsed_extra.slot, broadcast_id, signature_checked)
        .trace("Precheck failed");
  }

  td::actor::Task<td::BufferSlice> on_query(adnl::AdnlNodeIdShort src, td::BufferSlice data) {
    auto peer = adnl_id_to_peer_.find(src);
    auto peer_idx = peer != adnl_id_to_peer_.end() ? std::optional{peer->second.idx} : std::nullopt;

    td::int32 tag = data.size() >= 4 ? td::as<td::int32>(data.data()) : 0;
    const char* name;
    td::Result<ProtocolMessage> response;
    switch (tag) {
      case ton_api::consensus_simplex_requestCandidate::ID: {
        auto request = std::make_shared<IncomingCandidateRequest>(peer_idx, src, std::move(data));
        response = co_await owning_bus().publish(std::move(request)).wrap();
        name = "candidate";
        break;
      }
      case tl::pleaseCollatePrepare::ID:
      case tl::pleaseCollate::ID: {
        auto request = std::make_shared<IncomingCollatorRequest>(peer_idx, src, std::move(data));
        response = co_await owning_bus().publish(std::move(request)).wrap();
        name = "collator";
        break;
      }
      default:
        response = td::Status::Error(ErrorCode::protoviolation, PSTRING() << "unknown request id=" << tag);
        name = "unknown";
    }

    if (response.is_ok()) {
      co_return std::move(response.move_as_ok().data);
    }
    LOG(WARNING) << "Failed to process " << name << " request from " << src << " (" << peer_idx << ")" << ": "
                 << response.move_as_error();
    co_return create_serialize_tl_object<tl::requestError>();
  }

  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::AdnlSenderEx> adnl_sender_;
  overlay::OverlayIdShort overlay_id_;
  adnl::AdnlNodeIdShort local_adnl_id_;
  std::vector<adnl::AdnlNodeIdShort> overlay_nodes_;
  std::vector<adnl::AdnlNodeIdShort> other_overlay_nodes_;

  struct ParsedExtra {
    td::uint32 slot;
    std::optional<Delegation> delegation;
  };

  static td::Result<ParsedExtra> parse_broadcast_extra(td::Slice extra) {
    TRY_RESULT(parsed, fetch_tl_object<tl::BroadcastExtra>(extra, true));
    ParsedExtra result;
    auto legacy_fn = [&](tl::broadcastExtraLegacy& legacy) {
      result = ParsedExtra{static_cast<td::uint32>(legacy.slot_), std::nullopt};
    };
    auto current_fn = [&](tl::broadcastExtra& current) {
      result = ParsedExtra{static_cast<td::uint32>(current.slot_), Delegation::from_tl(std::move(current.delegation_))};
    };
    ton_api::downcast_call(*parsed, td::overloaded(legacy_fn, current_fn));
    return result;
  }

  std::map<adnl::AdnlNodeIdShort, PeerValidator> adnl_id_to_peer_;
  std::map<PublicKeyHash, PeerValidator> broadcast_src_to_peer_;
  std::optional<PublicKeyHash> local_broadcast_id_;

  std::mt19937 gossip_rng_ = td::Random::fast_gen();
};

}  // namespace

void PrivateOverlay::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<PrivateOverlayImpl>("PrivateOverlay");
}

}  // namespace ton::validator::consensus
