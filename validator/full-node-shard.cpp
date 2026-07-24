/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "adnl/utils.hpp"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api_json.h"
#include "common/delay.h"
#include "impl/out-msg-queue-proof.hpp"
#include "net/download-archive-slice.hpp"
#include "net/download-block-new.hpp"
#include "net/download-next-blocks.hpp"
#include "net/download-proof.hpp"
#include "net/download-state.hpp"
#include "net/get-next-key-blocks.hpp"
#include "td/utils/JsonBuilder.h"
#include "td/utils/Random.h"
#include "td/utils/SharedSlice.h"
#include "td/utils/buffer.h"
#include "td/utils/overloaded.h"
#include "tl/tl_json.h"
#include "ton/ton-io.hpp"
#include "ton/ton-shard.h"
#include "ton/ton-tl.hpp"

#include "checksum.h"
#include "full-node-queries.hpp"
#include "full-node-serializer.hpp"
#include "full-node-shard.hpp"
#include "overlays.h"

namespace ton {

namespace validator {

namespace fullnode {

namespace {

constexpr const char *k_called_from_public = "public";
constexpr size_t k_ed25519_signature_size = 64;

}  // namespace

Neighbour Neighbour::zero = Neighbour{adnl::AdnlNodeIdShort::zero()};

void Neighbour::update_proto_version(ton_api::tonNode_capabilities &q) {
  version_major = q.version_major_;
  version_minor = q.version_minor_;
  flags = q.flags_;
}

void Neighbour::query_success(double t) {
  unreliability--;
  if (unreliability < 0) {
    unreliability = 0;
  }
  update_roundtrip(t);
}

void Neighbour::query_failed() {
  unreliability++;
}

void Neighbour::update_roundtrip(double t) {
  roundtrip = (t + roundtrip) * 0.5;
}

void FullNodeShardImpl::create_overlay() {
  class Callback : public overlay::Overlays::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::receive_message, src, std::move(data));
    }
    void receive_query(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::receive_query, src, std::move(data), std::move(promise));
    }
    void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::receive_broadcast, src, std::move(data));
    }
    void check_broadcast(PublicKeyHash src, overlay::OverlayIdShort overlay_id, td::BufferSlice data,
                         td::Promise<td::Unit> promise) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::check_broadcast, src, std::move(data), std::move(promise));
    }
    void get_stats_extra(td::Promise<std::string> promise) override {
      td::actor::send_closure(node_, &FullNodeShardImpl::get_stats_extra, std::move(promise));
    }
    Callback(td::actor::ActorId<FullNodeShardImpl> node) : node_(node) {
    }

   private:
    td::actor::ActorId<FullNodeShardImpl> node_;
  };
  overlay::OverlayOptions opts;
  opts.name_ = "shard" + shard_.to_str();
  opts.announce_self_ = active_;
  opts.broadcast_speed_multiplier_ = opts_.public_broadcast_speed_multiplier_;
  opts.enable_plumtree_broadcast_ = true;
  opts.is_original_sender_ = is_original_sender_;
  opts.plumtree_broadcast_sender_ = quic_;
  td::actor::send_closure(overlays_, &overlay::Overlays::create_public_overlay_ex, adnl_id_, overlay_id_full_.clone(),
                          std::make_unique<Callback>(actor_id(this)), rules_,
                          PSTRING() << "{ \"type\": \"shard\", \"shard_id\": " << get_shard()
                                    << ", \"workchain_id\": " << get_workchain() << " }",
                          opts);

  // Do not register full-node shard ADNL IDs with legacy RLDP.
  // RLDP2 remains the only inbound full-node transport.
  td::actor::send_closure(rldp2_, &rldp2::Rldp::add_id, adnl_id_);
  if (cert_) {
    td::actor::send_closure(overlays_, &overlay::Overlays::update_certificate, adnl_id_, overlay_id_, local_id_, cert_);
  }
  auto adnl_source = full_node_adnl_source();
  if (!adnl_source.is_zero() && adnl_source != local_id_ && adnl_source_cert_) {
    td::actor::send_closure(overlays_, &overlay::Overlays::update_certificate, adnl_id_, overlay_id_, adnl_source,
                            adnl_source_cert_);
  }
}

void FullNodeShardImpl::check_broadcast(PublicKeyHash src, td::BufferSlice broadcast, td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, message,
                     fetch_tl_object<ton_api::tonNode_externalMessageBroadcast>(std::move(broadcast), true));
  if (opts_.config_.ext_messages_broadcast_disabled_) {
    promise.set_error(td::Status::Error("rebroadcasting external messages is disabled"));
    promise = [](td::Result<td::Unit>) {};
  }
  process_external_message_broadcast(*message, std::move(promise));
}

void FullNodeShardImpl::process_external_message_broadcast(ton_api::tonNode_externalMessageBroadcast &message,
                                                           td::Promise<td::Unit> promise) {
  if (!active_) {
    return promise.set_error(td::Status::Error("cannot process broadcast: shard is not active"));
  }
  auto hash = td::sha256_bits256(message.message_->data_);
  if (!processed_ext_msg_broadcasts_.insert(hash).second) {
    return promise.set_error(td::Status::Error("duplicate external message broadcast"));
  }
  if (my_ext_msg_broadcasts_.contains(hash)) {
    // Don't process messages that were sent by us
    promise.set_result(td::Unit());
    return;
  }
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_external_message_broadcast,
                          std::move(message.message_->data_), 0, std::move(promise));
}

void FullNodeShardImpl::remove_neighbour(adnl::AdnlNodeIdShort id) {
  neighbours_.erase(id);
}

void FullNodeShardImpl::update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, adnl_id_, overlay_id_);
  adnl_id_ = adnl_id;
  adnl_source_cert_.reset();
  create_overlay();
  promise.set_value(td::Unit{});
}

void FullNodeShardImpl::set_active(bool active) {
  bool changed = false;
  if (!shard_.is_masterchain() && active_ != active) {
    active_ = active;
    changed = true;
  }
  if (!changed) {
    return;
  }
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, adnl_id_, overlay_id_);
  create_overlay();
}

void FullNodeShardImpl::receive_query(adnl::AdnlNodeIdShort src, td::BufferSlice query,
                                      td::Promise<td::BufferSlice> promise) {
  if (!active_) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_message, src, adnl_id_, overlay_id_,
                            create_serialize_tl_object<ton_api::tonNode_forgetPeer>());
    promise.set_error(td::Status::Error("shard is inactive"));
    return;
  }
  td::actor::send_closure(full_node_, &FullNode::handle_query, std::move(query), src, QuerySource::public_overlay,
                          std::move(promise));
}

void FullNodeShardImpl::receive_message(adnl::AdnlNodeIdShort src, td::BufferSlice data) {
  auto B = fetch_tl_object<ton_api::tonNode_forgetPeer>(std::move(data), true);
  if (B.is_error()) {
    return;
  }
  VLOG(full_node, DEBUG) << "Got tonNode.forgetPeer from " << src;
  neighbours_.erase(src);
  td::actor::send_closure(overlays_, &overlay::Overlays::forget_peer, adnl_id_, overlay_id_, src);
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_externalMessageBroadcast &query) {
  process_external_message_broadcast(query, [](td::Result<td::Unit>) {});
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_newBlockCandidateBroadcast &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src,
                                          ton_api::tonNode_newBlockCandidateBroadcastCompressed &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src,
                                          ton_api::tonNode_newBlockCandidateBroadcastCompressedV2 &query) {
  process_block_candidate_broadcast(src, query);
}

void FullNodeShardImpl::process_block_candidate_broadcast(PublicKeyHash src, ton_api::tonNode_Broadcast &query) {
  BlockIdExt block_id;
  CatchainSeqno cc_seqno;
  td::uint32 validator_set_hash;
  td::BufferSlice data;
  auto S = deserialize_block_candidate_broadcast(query, block_id, cc_seqno, validator_set_hash, data,
                                                 overlay::Overlays::max_fec_broadcast_size(), k_called_from_public);
  if (S.is_error()) {
    VLOG(full_node, WARNING) << "received bad block candidate from " << src << " : " << S;
    return;
  }
  if (data.size() > FullNode::max_block_size()) {
    VLOG(full_node, WARNING) << "received block candidate with too big size from " << src;
    return;
  }
  if (td::sha256_bits256(data.as_slice()) != block_id.file_hash) {
    VLOG(full_node, WARNING) << "received block candidate with incorrect file hash from " << src;
    return;
  }
  VLOG(full_node, DEBUG) << "Received newBlockCandidate from " << src << ": " << block_id;
  td::actor::send_closure(full_node_, &FullNode::process_block_candidate_broadcast, block_id, cc_seqno,
                          validator_set_hash, std::move(data), BroadcastSource::public_overlay, false);
}

void FullNodeShardImpl::process_broadcast(PublicKeyHash src, ton_api::tonNode_blockFinalityBroadcast &query) {
  auto block_id = create_block_id(query.id_);
  BlockFinalityBroadcast finality{block_id, block::BlockSignatureSet::fetch(query.signature_set_)};

  VLOG(full_node, DEBUG) << "Received blockFinalityBroadcast in public overlay from " << src << ": " << block_id;
  td::actor::send_closure(full_node_, &FullNode::process_block_finality_broadcast, std::move(finality),
                          BroadcastSource::public_overlay, false);
}

void FullNodeShardImpl::receive_broadcast(PublicKeyHash src, td::BufferSlice broadcast) {
  if (!active_) {
    return;
  }
  auto B = fetch_tl_object<ton_api::tonNode_Broadcast>(std::move(broadcast), true);
  if (B.is_error()) {
    return;
  }

  ton_api::downcast_call(*B.move_as_ok().get(), [src, Self = this](auto &obj) { Self->process_broadcast(src, obj); });
}

void FullNodeShardImpl::send_external_message(td::BufferSlice data) {
  td::Bits256 hash = td::sha256_bits256(data);
  if (processed_ext_msg_broadcasts_.count(hash)) {
    return;
  }
  my_ext_msg_broadcasts_.insert(hash);
  auto B = create_serialize_tl_object<ton_api::tonNode_externalMessageBroadcast>(
      create_tl_object<ton_api::tonNode_externalMessage>(std::move(data)));
  auto source = choose_outbound_source(static_cast<td::uint32>(B.size()),
                                       B.size() > overlay::Overlays::max_simple_broadcast_size());
  if (B.size() <= overlay::Overlays::max_simple_broadcast_size()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_ex, adnl_id_, overlay_id_, source, 0,
                            std::move(B));
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, adnl_id_, overlay_id_, source, 0,
                            std::move(B));
  }
}

void FullNodeShardImpl::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                             td::BufferSlice data) {
  auto B = serialize_block_candidate_broadcast(block_id, cc_seqno, validator_set_hash, data, true,
                                               k_called_from_public);  // compression enabled
  if (B.is_error()) {
    VLOG(full_node, WARNING) << "failed to serialize Plumtree block candidate broadcast: " << B.move_as_error();
    return;
  }
  VLOG(full_node, DEBUG) << "Sending Plumtree newBlockCandidate: " << block_id;
  auto payload = B.move_as_ok();
  auto source = choose_outbound_source(static_cast<td::uint32>(payload.size()), true);
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_plumtree_fec, adnl_id_, overlay_id_, source,
                          overlay::Overlays::BroadcastFlagAnySender(), std::move(payload));
}

void FullNodeShardImpl::send_block_finality_broadcast(BlockFinalityBroadcast finality) {
  VLOG(full_node, DEBUG) << "Sending Plumtree blockFinalityBroadcast in public overlay: " << finality.block_id;
  auto broadcast_id = get_tl_object_sha_bits256(create_tl_object<ton_api::tonNode_finalityBroadcastId>(
      create_tl_block_id(finality.block_id), finality.sig_set->is_final()));
  auto payload = create_serialize_tl_object<ton_api::tonNode_blockFinalityBroadcast>(
      create_tl_block_id(finality.block_id), finality.sig_set->tl());
  auto source = choose_outbound_source(static_cast<td::uint32>(payload.size()), true);
  td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_plumtree, adnl_id_, overlay_id_, source,
                          overlay::Overlays::BroadcastFlagAnySender(), broadcast_id, std::move(payload));
}

void FullNodeShardImpl::alarm() {
  if (reload_neighbours_at_ && reload_neighbours_at_.is_in_past()) {
    reload_neighbours();
    reload_neighbours_at_ = td::Timestamp::in(td::Random::fast(10.0, 30.0));
  }
  if (ping_neighbours_at_ && ping_neighbours_at_.is_in_past()) {
    ping_neighbours();
    ping_neighbours_at_ = td::Timestamp::in(td::Random::fast(0.5, 1.0));
  }
  if (update_certificate_at_ && update_certificate_at_.is_in_past()) {
    if (!sign_cert_by_.is_zero()) {
      sign_new_certificate(sign_cert_by_);
      update_certificate_at_ = td::Timestamp::in(30.0);
    } else {
      update_certificate_at_ = td::Timestamp::never();
    }
  }
  if (cleanup_processed_ext_msg_at_ && cleanup_processed_ext_msg_at_.is_in_past()) {
    processed_ext_msg_broadcasts_.clear();
    my_ext_msg_broadcasts_.clear();
    cleanup_processed_ext_msg_at_ = td::Timestamp::in(60.0);
  }
  alarm_timestamp().relax(update_certificate_at_);
  alarm_timestamp().relax(reload_neighbours_at_);
  alarm_timestamp().relax(ping_neighbours_at_);
  alarm_timestamp().relax(cleanup_processed_ext_msg_at_);
}

void FullNodeShardImpl::start_up() {
  auto X =
      create_hash_tl_object<ton_api::tonNode_shardPublicOverlayId>(get_workchain(), get_shard(), zero_state_file_hash_);
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();
  rules_ = overlay::OverlayPrivacyRules{overlay::Overlays::max_fec_broadcast_size()};

  create_overlay();

  reload_neighbours_at_ = td::Timestamp::now();
  ping_neighbours_at_ = td::Timestamp::now();
  cleanup_processed_ext_msg_at_ = td::Timestamp::now();
  alarm_timestamp().relax(td::Timestamp::now());
}

void FullNodeShardImpl::tear_down() {
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, adnl_id_, overlay_id_);
}

void FullNodeShardImpl::sign_new_certificate(PublicKeyHash sign_by) {
  if (sign_by.is_zero()) {
    return;
  }

  ton::overlay::Certificate cert{
      sign_by, static_cast<td::int32>(td::Clocks::system() + 3600), overlay::Overlays::max_fec_broadcast_size(),
      overlay::CertificateFlags::Trusted | overlay::CertificateFlags::AllowFec, td::BufferSlice{}};
  auto to_sign = cert.to_sign(overlay_id_, local_id_);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), cert = std::move(cert), local_id = local_id_](
                                          td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
    if (R.is_error()) {
      // ignore
      VLOG(full_node, WARNING) << "failed to create certificate: failed to sign: " << R.move_as_error();
    } else {
      auto p = R.move_as_ok();
      cert.set_signature(std::move(p.first));
      cert.set_issuer(p.second);
      td::actor::send_closure(SelfId, &FullNodeShardImpl::signed_new_certificate, std::move(cert), local_id);
    }
  });
  td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_add_get_public_key, sign_by, std::move(to_sign),
                          std::move(P));
}

void FullNodeShardImpl::signed_new_certificate(overlay::Certificate cert, PublicKeyHash local_id) {
  if (local_id != local_id_) {
    return;
  }
  LOG(WARNING) << "updated certificate";
  cert_ = std::make_shared<overlay::Certificate>(std::move(cert));
  td::actor::send_closure(overlays_, &overlay::Overlays::update_certificate, adnl_id_, overlay_id_, local_id_, cert_);
}

PublicKeyHash FullNodeShardImpl::full_node_adnl_source() const {
  return adnl_id_.is_zero() ? PublicKeyHash::zero() : adnl_id_.pubkey_hash();
}

bool FullNodeShardImpl::has_valid_certificate_for_source(const PublicKeyHash &source,
                                                         const std::shared_ptr<ton::overlay::Certificate> &cert,
                                                         td::uint32 payload_size, bool is_fec) const {
  if (!cert) {
    return false;
  }
  return cert->check(source, overlay_id_, static_cast<td::int32>(td::Clocks::system()), payload_size, is_fec) ==
             overlay::BroadcastCheckResult::Allowed &&
         rules_.is_authorized_key(cert->issuer_hash());
}

PublicKeyHash FullNodeShardImpl::choose_outbound_source(td::uint32 payload_size, bool is_fec) const {
  auto adnl_source = full_node_adnl_source();
  if (!adnl_source.is_zero() && adnl_source != local_id_ &&
      has_valid_certificate_for_source(adnl_source, adnl_source_cert_, payload_size, is_fec)) {
    return adnl_source;
  }
  return local_id_;
}

void FullNodeShardImpl::sign_overlay_certificate(PublicKeyHash signed_key, td::uint32 expire_at, td::uint32 max_size,
                                                 td::Promise<td::BufferSlice> promise) {
  auto sign_by = sign_cert_by_;
  if (sign_by.is_zero()) {
    promise.set_error(td::Status::Error("Node has no key with signing authority"));
    return;
  }

  ton::overlay::Certificate cert{sign_by, static_cast<td::int32>(expire_at), max_size,
                                 overlay::CertificateFlags::Trusted | overlay::CertificateFlags::AllowFec,
                                 td::BufferSlice{}};
  auto to_sign = cert.to_sign(overlay_id_, signed_key);

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), expire_at = expire_at, max_size = max_size,
       promise = std::move(promise)](td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("failed to create certificate: failed to sign: "));
        } else {
          auto p = R.move_as_ok();
          auto c = ton::create_serialize_tl_object<ton::ton_api::overlay_certificate>(
              p.second.tl(), static_cast<td::int32>(expire_at), max_size, std::move(p.first));
          promise.set_value(std::move(c));
        }
      });
  td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_add_get_public_key, sign_by, std::move(to_sign),
                          std::move(P));
}

void FullNodeShardImpl::import_overlay_certificate(PublicKeyHash signed_key,
                                                   std::shared_ptr<ton::overlay::Certificate> cert,
                                                   td::Promise<td::Unit> promise) {
  if (!cert) {
    promise.set_error(td::Status::Error("empty certificate"));
    return;
  }
  if (cert->signature().size() != k_ed25519_signature_size) {
    promise.set_error(td::Status::Error(PSTRING() << "bad certificate signature size: " << cert->signature().size()));
    return;
  }
  auto check = cert->check(signed_key, overlay_id_, static_cast<td::int32>(td::Clocks::system()),
                           overlay::Overlays::max_fec_broadcast_size(), true,
                           /* skip_check_signature = */ false);
  if (check != overlay::BroadcastCheckResult::Allowed) {
    promise.set_error(td::Status::Error("certificate is not valid for this shard overlay"));
    return;
  }
  if (!rules_.is_authorized_key(cert->issuer_hash())) {
    promise.set_error(td::Status::Error(PSTRING() << "certificate issuer is not authorized for this shard overlay: "
                                                  << cert->issuer_hash()));
    return;
  }
  auto adnl_source = full_node_adnl_source();
  bool usable_for_local_source = signed_key == local_id_;
  bool usable_for_adnl_source = !adnl_source.is_zero() && signed_key == adnl_source;
  LOG(INFO) << "shard overlay cert accepted shard=" << shard_ << " signed_key=" << signed_key
            << " local_id=" << local_id_ << " adnl_source=" << adnl_source << " issuer=" << cert->issuer_hash()
            << " usable_for_local_source=" << usable_for_local_source
            << " usable_for_adnl_source=" << usable_for_adnl_source;
  if (usable_for_local_source) {
    cert_ = cert;
  }
  if (usable_for_adnl_source) {
    adnl_source_cert_ = cert;
  }
  td::actor::send_closure(overlays_, &ton::overlay::Overlays::update_certificate, adnl_id_, overlay_id_, signed_key,
                          cert);
  promise.set_value(td::Unit());
}

td::actor::Task<QuerySender> FullNodeShardImpl::get_query_sender() {
  class QuerySenderImpl : public QuerySenderInterface {
   public:
    QuerySenderImpl(adnl::AdnlNodeIdShort peer_id, adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                    td::actor::ActorId<overlay::Overlays> overlays,
                    td::actor::ActorId<adnl::AdnlSenderInterface> adnl_sender,
                    td::actor::ActorId<FullNodeShardImpl> parent, std::pair<td::uint32, td::uint32> proto_version)
        : peer_id_(peer_id)
        , local_id_(local_id)
        , overlay_id_(overlay_id)
        , overlays_(std::move(overlays))
        , adnl_sender_(std::move(adnl_sender))
        , parent_(std::move(parent))
        , proto_version_(proto_version) {
    }

    void send_query(td::BufferSlice query, td::Timestamp timeout, td::uint64 max_answer_size,
                    td::Promise<td::BufferSlice> promise) const override {
      td::actor::send_closure(overlays_, &overlay::Overlays::send_query_via, peer_id_, local_id_, overlay_id_, "q",
                              std::move(promise), timeout, std::move(query), max_answer_size, adnl_sender_);
    }

    void query_finished(td::Status S) const override {
      td::actor::send_closure(parent_, &FullNodeShardImpl::update_neighbour_stats, peer_id_, timer_.elapsed(),
                              S.is_ok() || S.code() == ErrorCode::notready || S.code() == ErrorCode::cancelled);
    }

    std::string to_str() const override {
      return PSTRING() << "peer " << peer_id_ << " in public overlay";
    }

    std::pair<td::uint32, td::uint32> get_proto_version() const override {
      return proto_version_;
    }

   private:
    adnl::AdnlNodeIdShort peer_id_;
    adnl::AdnlNodeIdShort local_id_;
    overlay::OverlayIdShort overlay_id_;
    td::actor::ActorId<overlay::Overlays> overlays_;
    td::actor::ActorId<adnl::AdnlSenderInterface> adnl_sender_;
    td::actor::ActorId<FullNodeShardImpl> parent_;
    std::pair<td::uint32, td::uint32> proto_version_;

    td::Timer timer_;
  };
  auto &peer = choose_neighbour();
  auto peer_id = peer.adnl_id;
  if (peer_id.is_zero()) {
    auto peers =
        co_await td::actor::ask(overlays_, &overlay::Overlays::get_overlay_random_peers, adnl_id_, overlay_id_, 1);
    if (peers.empty()) {
      co_return td::Status::Error(ErrorCode::notready, "no nodes");
    }
    peer_id = peers[0];
  }
  co_return std::make_shared<QuerySenderImpl>(peer_id, adnl_id_, overlay_id_, overlays_, rldp2_, actor_id(this),
                                              peer.version());
}

void FullNodeShardImpl::update_validators(std::vector<PublicKeyHash> public_key_hashes, PublicKeyHash local_hash) {
  bool update_cert = false;
  bool recreate_overlay = false;
  bool is_original_sender = !local_hash.is_zero();
  if (is_original_sender_ != is_original_sender) {
    is_original_sender_ = is_original_sender;
    recreate_overlay = true;
  }
  if (!local_hash.is_zero() && local_hash != sign_cert_by_) {
    update_cert = true;
  }
  sign_cert_by_ = local_hash;

  std::map<PublicKeyHash, td::uint32> authorized_keys;
  for (auto &key : public_key_hashes) {
    authorized_keys.emplace(key, overlay::Overlays::max_fec_broadcast_size());
  }

  rules_ = overlay::OverlayPrivacyRules{overlay::Overlays::max_fec_broadcast_size(),
                                        overlay::CertificateFlags::AllowFec, std::move(authorized_keys)};
  if (recreate_overlay) {
    td::actor::send_closure(overlays_, &ton::overlay::Overlays::delete_overlay, adnl_id_, overlay_id_);
    create_overlay();
  } else {
    td::actor::send_closure(overlays_, &overlay::Overlays::set_privacy_rules, adnl_id_, overlay_id_, rules_);
  }

  if (update_cert) {
    sign_new_certificate(sign_cert_by_);
    update_certificate_at_ = td::Timestamp::in(30.0);
    alarm_timestamp().relax(update_certificate_at_);
  }
}

void FullNodeShardImpl::reload_neighbours() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::vector<adnl::AdnlNodeIdShort>> R) {
    if (R.is_error()) {
      return;
    }
    auto vec = R.move_as_ok();
    if (vec.size() == 0) {
      return;
    } else {
      td::actor::send_closure(SelfId, &FullNodeShardImpl::got_neighbours, std::move(vec));
    }
  });
  td::actor::send_closure(overlays_, &overlay::Overlays::get_overlay_random_peers, adnl_id_, overlay_id_,
                          max_neighbours(), std::move(P));
}

void FullNodeShardImpl::got_neighbours(std::vector<adnl::AdnlNodeIdShort> vec) {
  bool ex = false;

  for (auto &el : vec) {
    auto it = neighbours_.find(el);
    if (it != neighbours_.end()) {
      continue;
    }
    if (neighbours_.size() == max_neighbours()) {
      adnl::AdnlNodeIdShort a = adnl::AdnlNodeIdShort::zero();
      adnl::AdnlNodeIdShort b = adnl::AdnlNodeIdShort::zero();
      td::uint32 cnt = 0;
      double u = 0;
      for (auto &n : neighbours_) {
        if (n.second.unreliability > u) {
          u = n.second.unreliability;
          a = n.first;
        }
        if (td::Random::fast(0, cnt++) == 0) {
          b = n.first;
        }
      }

      if (u > stop_unreliability()) {
        neighbours_.erase(a);
      } else {
        neighbours_.erase(b);
        ex = true;
      }
    }
    neighbours_.emplace(el, Neighbour{el});
    if (ex) {
      break;
    }
  }
}

const Neighbour &FullNodeShardImpl::choose_neighbour(td::uint32 required_version_major,
                                                     td::uint32 required_version_minor) const {
  if (neighbours_.size() == 0) {
    return Neighbour::zero;
  }
  auto is_eligible = [&](const Neighbour &n) {
    return n.version_major > required_version_major ||
           (n.version_major == required_version_major && n.version_minor >= required_version_minor);
  };

  double min_unreliability = 1e9;
  for (auto &[_, x] : neighbours_) {
    if (!is_eligible(x)) {
      continue;
    }
    min_unreliability = std::min(min_unreliability, x.unreliability);
  }
  const Neighbour *best = nullptr;
  td::uint32 sum = 0;

  for (auto &[_, x] : neighbours_) {
    if (!is_eligible(x)) {
      continue;
    }
    auto unr = static_cast<td::uint32>(x.unreliability - min_unreliability);

    if (x.version_major < FullNode::PROTO_VERSION_MAJOR) {
      unr += 4;
    } else if (x.version_major == FullNode::PROTO_VERSION_MAJOR && x.version_minor < FullNode::PROTO_VERSION_MINOR) {
      unr += 2;
    }

    auto f = static_cast<td::uint32>(fail_unreliability());

    if (unr <= f) {
      auto w = 1 << (f - unr);
      sum += w;
      if (td::Random::fast(0, sum - 1) <= w - 1) {
        best = &x;
      }
    }
  }
  if (best) {
    return *best;
  }
  return Neighbour::zero;
}

void FullNodeShardImpl::update_neighbour_stats(adnl::AdnlNodeIdShort adnl_id, double t, bool success) {
  auto it = neighbours_.find(adnl_id);
  if (it != neighbours_.end()) {
    if (success) {
      it->second.query_success(t);
    } else {
      it->second.query_failed();
    }
  }
}

void FullNodeShardImpl::got_neighbour_capabilities(adnl::AdnlNodeIdShort adnl_id, double t, td::BufferSlice data) {
  auto it = neighbours_.find(adnl_id);
  if (it == neighbours_.end()) {
    return;
  }
  auto F = fetch_tl_object<ton_api::tonNode_capabilities>(std::move(data), true);
  if (F.is_error()) {
    it->second.query_failed();
  } else {
    it->second.update_proto_version(*F.ok());
    it->second.query_success(t);
  }
}

void FullNodeShardImpl::ping_neighbours() {
  if (neighbours_.size() == 0) {
    return;
  }
  td::uint32 max_cnt = 6;
  if (max_cnt > neighbours_.size()) {
    max_cnt = td::narrow_cast<td::uint32>(neighbours_.size());
  }
  auto it = neighbours_.lower_bound(last_pinged_neighbour_);
  while (max_cnt > 0) {
    if (it == neighbours_.end()) {
      it = neighbours_.begin();
    }

    auto P = td::PromiseCreator::lambda(
        [SelfId = actor_id(this), start_time = td::Time::now(), id = it->first](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            td::actor::send_closure(SelfId, &FullNodeShardImpl::update_neighbour_stats, id,
                                    td::Time::now() - start_time, false);
          } else {
            td::actor::send_closure(SelfId, &FullNodeShardImpl::got_neighbour_capabilities, id,
                                    td::Time::now() - start_time, R.move_as_ok());
          }
        });
    td::BufferSlice q = create_serialize_tl_object<ton_api::tonNode_getCapabilities>();
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, it->first, adnl_id_, overlay_id_,
                            "get_prepare_block", std::move(P), td::Timestamp::in(1.0), std::move(q));

    last_pinged_neighbour_ = it->first;
    it++;
    max_cnt--;
  }
}

void FullNodeShardImpl::get_stats_extra(td::Promise<std::string> promise) {
  auto res = create_tl_object<ton_api::engine_validator_shardOverlayStats>();
  res->shard_ = shard_.to_str();
  res->active_ = active_;
  for (const auto &p : neighbours_) {
    const auto &n = p.second;
    auto f = create_tl_object<ton_api::engine_validator_shardOverlayStats_neighbour>();
    f->id_ = n.adnl_id.bits256_value().to_hex();
    f->verison_major_ = n.version_major;
    f->version_minor_ = n.version_minor;
    f->flags_ = n.flags;
    f->roundtrip_ = n.roundtrip;
    f->unreliability_ = n.unreliability;
    res->neighbours_.push_back(std::move(f));
  }
  promise.set_result(td::json_encode<std::string>(td::ToJson(*res), true));
}

FullNodeShardImpl::FullNodeShardImpl(ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id,
                                     FileHash zero_state_file_hash, FullNodeOptions opts,
                                     td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                                     td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic,
                                     td::actor::ActorId<overlay::Overlays> overlays,
                                     td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                                     td::actor::ActorId<FullNode> full_node, bool active)
    : shard_(shard)
    , local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp2_(rldp2)
    , quic_(quic)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , full_node_(full_node)
    , active_(active)
    , opts_(opts) {
}

td::actor::ActorOwn<FullNodeShard> FullNodeShard::create(
    ShardIdFull shard, PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
    FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<ValidatorManagerInterface> validator_manager,
    td::actor::ActorId<FullNode> full_node, bool active) {
  return td::actor::create_actor<FullNodeShardImpl>(PSTRING() << "tonnode" << shard, shard, local_id, adnl_id,
                                                    zero_state_file_hash, opts, keyring, adnl, rldp2, quic, overlays,
                                                    validator_manager, full_node, active);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
