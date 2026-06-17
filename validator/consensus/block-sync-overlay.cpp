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

  void start_up() override {
    auto& bus = *owning_bus();
    overlays_ = bus.overlays;
    local_adnl_id_ = bus.local_id.adnl_id;
    adnl_sender_ = bus.adnl_sender;

    std::map<PublicKeyHash, td::uint32> authorized_keys;
    td::uint32 max_broadcast_size = bus.config.max_block_size + bus.config.max_collated_data_size + (1 << 20);
    for (const auto& peer : bus.validator_set) {
      adnl_pubkey_to_peer_[peer.adnl_id.pubkey_hash()] = peer;
      authorized_keys.emplace(peer.adnl_id.pubkey_hash(), max_broadcast_size);
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

    td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay_ex, local_adnl_id_,
                            std::move(overlay_full_id), bus.overlay_members, make_callback(),
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
    td::BufferSlice extra = create_serialize_tl_object<tl::broadcastExtra>(event->candidate->id.slot);
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_with_extra, local_adnl_id_, overlay_id_,
                            local_adnl_id_.pubkey_hash(), 0, event->candidate->serialize(), std::move(extra));
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
    if (src == local_adnl_id_.pubkey_hash()) {
      return;
    }

    auto parsed_extra = fetch_tl_object<tl::broadcastExtra>(extra, true).move_as_ok();

    auto& bus = *owning_bus();
    auto it = adnl_pubkey_to_peer_.find(src);
    if (it == adnl_pubkey_to_peer_.end()) {
      LOG(WARNING) << "Block sync overlay broadcast from non-validator " << src;
      return;
    }
    auto maybe_candidate = Candidate::deserialize(std::move(data), bus, it->second.idx, parsed_extra->slot_);

    if (maybe_candidate.is_error()) {
      LOG(WARNING) << "MISBEHAVIOR: Failed to deserialize block candidate broadcast: "
                   << maybe_candidate.move_as_error();
      return;
    }
    owning_bus().publish<CandidateReceived>(maybe_candidate.move_as_ok());
  }

  td::actor::Task<> precheck_broadcast(PublicKeyHash src, td::Bits256 broadcast_id, td::BufferSlice extra,
                                       bool signature_checked) {
    auto parsed_extra = fetch_tl_object<tl::broadcastExtra>(extra, true);
    if (parsed_extra.is_error()) {
      co_return parsed_extra.move_as_error_prefix("Precheck failed: Failed to parse broadcast extra: ");
    }

    auto& bus = *owning_bus();
    auto it = adnl_pubkey_to_peer_.find(src);
    if (it == adnl_pubkey_to_peer_.end()) {
      co_return td::Status::Error("Precheck failed: Broadcast is not from a validator of the current group");
    }
    td::uint32 slot = parsed_extra.move_as_ok()->slot_;
    if (it->second.idx != bus.collator_schedule->expected_collator_for(slot)) {
      co_return td::Status::Error("Precheck failed: Broadcast is not from the expected collator");
    }

    co_return co_await owning_bus()
        .publish<PrecheckCandidateBroadcast>(slot, broadcast_id, signature_checked)
        .trace("Precheck failed");
  }

  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::AdnlSenderEx> adnl_sender_;
  overlay::OverlayIdShort overlay_id_;
  adnl::AdnlNodeIdShort local_adnl_id_;
  std::map<PublicKeyHash, PeerValidator> adnl_pubkey_to_peer_;
};

}  // namespace

void BlockSyncOverlay::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<BlockSyncOverlayImpl>("BlockSyncOverlay");
}

}  // namespace ton::validator::consensus
