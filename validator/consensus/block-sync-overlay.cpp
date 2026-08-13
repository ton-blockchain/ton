/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <map>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "overlay/overlays.h"
#include "td/utils/Status.h"
#include "td/utils/logging.h"
#include "ton/ton-io.hpp"

#include "bus.h"

namespace ton::validator::consensus {

namespace tl {

using blockSyncOverlayId = ton_api::consensus_blockSyncOverlayId;
using broadcastExtra = ton_api::consensus_broadcastExtra;

}  // namespace tl

namespace {

class BlockSyncOverlayImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  static bool should_be_spawned(const Bus& bus) {
    return bus.config.enable_block_sync();
  }

  void start_up() override {
    auto& bus = *owning_bus();
    overlays_ = bus.overlays;
    local_adnl_id_ = bus.local_adnl_id;
    adnl_sender_ = bus.adnl_sender;

    std::map<PublicKeyHash, td::uint32> authorized_keys;
    td::uint32 max_broadcast_size = bus.config.max_block_size + bus.config.max_collated_data_size + (1 << 20);
    for (const auto& peer : bus.validator_set) {
      broadcast_src_to_peer_[peer.adnl_id.pubkey_hash()] = peer;
      authorized_keys.emplace(peer.adnl_id.pubkey_hash(), max_broadcast_size);
    }
    for (const adnl::AdnlNodeIdShort& collator_id : bus.all_collators) {
      authorized_keys.emplace(collator_id.pubkey_hash(), max_broadcast_size);
    }

    td::actor::send_closure(adnl_sender_, &adnl::AdnlSenderEx::add_id, local_adnl_id_);

    auto overlay_seed = create_tl_object<tl::blockSyncOverlayId>(bus.session_id);
    auto overlay_full_id = overlay::OverlayIdFull{serialize_tl_object(overlay_seed, true)};
    overlay_id_ = overlay_full_id.compute_short_id();

    overlay::OverlayOptions options;
    options.name_ = PSTRING() << "blocksync" << bus.shard << "." << bus.cc_seqno;
    options.private_ping_peers_ = true;
    options.twostep_broadcast_sender_ = adnl_sender_;
    options.send_twostep_broadcast_ = true;
    options.allow_old_broadcasts_ = false;
    options.twostep_intermediate_nodes_ = bus.all_current_validators;

    td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay_ex, local_adnl_id_,
                            std::move(overlay_full_id), bus.all_overlay_nodes, make_callback(),
                            overlay::OverlayPrivacyRules{0, 0, std::move(authorized_keys)},
                            PSTRING() << R"({ "type": "blocksync", "shard": ")" << bus.shard << R"(", "cc_seqno": )"
                                      << bus.cc_seqno << R"( })",
                            std::move(options));
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_adnl_id_, overlay_id_);
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateGenerated> event) {
    td::BufferSlice extra;
    if (event->candidate->delegation) {
      extra = create_serialize_tl_object<tl::broadcastExtra>(1, event->candidate->id.slot,
                                                             event->candidate->delegation->to_tl());
    } else {
      // extra = create_serialize_tl_object<tl::broadcastExtra>(0, event->candidate->id.slot, nullptr);
      extra = create_serialize_tl_object<tl::broadcastExtraLegacy>(event->candidate->id.slot);
    }
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_with_extra, local_adnl_id_, overlay_id_,
                            local_adnl_id_.pubkey_hash(), 0, event->candidate->serialize_for_broadcast(),
                            std::move(extra));
  }

 private:
  std::unique_ptr<overlay::Overlays::Callback> make_callback() {
    class Callback final : public overlay::Overlays::Callback {
     public:
      explicit Callback(td::actor::ActorId<BlockSyncOverlayImpl> owner) : owner_(owner) {
      }

      void receive_broadcast_with_extra(PublicKeyHash src, overlay::OverlayIdShort, td::BufferSlice data,
                                        td::BufferSlice extra) override {
        td::actor::send_closure(owner_, &BlockSyncOverlayImpl::on_overlay_broadcast, src, std::move(data),
                                std::move(extra));
      }

      void precheck_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::Bits256 broadcast_id,
                              td::BufferSlice extra, bool signature_checked, td::Promise<> promise) override {
        td::actor::send_closure(owner_, &BlockSyncOverlayImpl::precheck_broadcast, src, broadcast_id, std::move(extra),
                                signature_checked, std::move(promise));
      }

      void check_broadcast(PublicKeyHash, overlay::OverlayIdShort, td::BufferSlice,
                           td::Promise<td::Unit> promise) override {
        promise.set_value(td::Unit());
      }

     private:
      td::actor::ActorId<BlockSyncOverlayImpl> owner_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  void on_overlay_broadcast(PublicKeyHash src, td::BufferSlice data, td::BufferSlice extra) {
    auto& bus = *owning_bus();
    if (src == local_adnl_id_.pubkey_hash()) {
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

  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::AdnlSenderEx> adnl_sender_;
  overlay::OverlayIdShort overlay_id_;
  adnl::AdnlNodeIdShort local_adnl_id_;
  std::map<PublicKeyHash, PeerValidator> broadcast_src_to_peer_;
};

}  // namespace

void BlockSyncOverlay::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockSyncOverlayImpl>("BlockSyncOverlay");
}

}  // namespace ton::validator::consensus
