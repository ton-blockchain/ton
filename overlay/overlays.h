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
#pragma once

#include "adnl/adnl-node-id.hpp"
#include "adnl/adnl.h"
#include "auto/tl/ton_api.h"
#include "dht/dht.h"

#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"
#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"

#include <map>

namespace ton {

namespace overlay {

enum class OverlayType { Public, FixedMemberList, CertificatedMembers };

class OverlayIdShort {
 public:
  OverlayIdShort() {
  }
  explicit OverlayIdShort(td::Bits256 id) : id_(id) {
  }
  auto bits256_value() const {
    return id_;
  }
  auto pubkey_hash() const {
    return PublicKeyHash{id_};
  }

  auto tl() const {
    return id_;
  }
  bool operator<(const OverlayIdShort &with) const {
    return id_ < with.id_;
  }
  bool operator==(const OverlayIdShort &with) const {
    return id_ == with.id_;
  }
  bool operator!=(const OverlayIdShort &with) const {
    return id_ != with.id_;
  }

 private:
  td::Bits256 id_;
};

class OverlayIdFull {
 public:
  OverlayIdFull() {
  }
  OverlayIdFull clone() const {
    return OverlayIdFull{name_.clone()};
  }
  explicit OverlayIdFull(td::BufferSlice name) : name_(std::move(name)) {
  }
  auto pubkey() const {
    return PublicKey{pubkeys::Overlay{name_.clone()}};
  }

  OverlayIdShort compute_short_id() const {
    return OverlayIdShort{pubkey().compute_short_id().bits256_value()};
  }

 private:
  td::BufferSlice name_;
};

struct CertificateFlags {
  enum Values : td::uint32 { AllowFec = 1, Trusted = 2 };
};

struct OverlayMemberFlags {
  enum Values : td::uint32 { DoNotReceiveBroadcasts = 1 };
};

enum BroadcastCheckResult { Forbidden = 1, NeedCheck = 2, Allowed = 3 };

inline BroadcastCheckResult broadcast_check_result_max(BroadcastCheckResult l, BroadcastCheckResult r) {
  return static_cast<BroadcastCheckResult>(std::max(static_cast<td::int32>(l), static_cast<td::int32>(r)));
}
inline BroadcastCheckResult broadcast_check_result_min(BroadcastCheckResult l, BroadcastCheckResult r) {
  return static_cast<BroadcastCheckResult>(std::min(static_cast<td::int32>(l), static_cast<td::int32>(r)));
}

class OverlayPrivacyRules {
 public:
  OverlayPrivacyRules() {
  }
  OverlayPrivacyRules(td::uint32 size) : max_unath_size_(size) {
  }
  OverlayPrivacyRules(td::uint32 max_size, td::uint32 flags, std::map<PublicKeyHash, td::uint32> authorized_keys)
      : max_unath_size_(max_size), flags_(flags), authorized_keys_(std::move(authorized_keys)) {
  }

  BroadcastCheckResult check_rules(PublicKeyHash hash, td::uint32 size, bool is_fec) {
    auto it = authorized_keys_.find(hash);
    if (it == authorized_keys_.end()) {
      if (size > max_unath_size_) {
        return BroadcastCheckResult::Forbidden;
      }
      if (!(flags_ & CertificateFlags::AllowFec) && is_fec) {
        return BroadcastCheckResult::Forbidden;
      }
      return (flags_ & CertificateFlags::Trusted) ? BroadcastCheckResult::Allowed : BroadcastCheckResult::NeedCheck;
    } else {
      return it->second >= size ? BroadcastCheckResult::Allowed : BroadcastCheckResult::Forbidden;
    }
  }

 private:
  td::uint32 max_unath_size_{0};
  td::uint32 flags_{0};
  std::map<PublicKeyHash, td::uint32> authorized_keys_;
};

class Certificate {
 public:
  Certificate(PublicKeyHash issued_by, td::int32 expire_at, td::uint32 max_size, td::uint32 flags,
              td::BufferSlice signature);
  Certificate(PublicKey issued_by, td::int32 expire_at, td::uint32 max_size, td::uint32 flags,
              td::BufferSlice signature);
  Certificate() {
  }
  void set_signature(td::BufferSlice signature);
  void set_issuer(PublicKey issuer);
  td::BufferSlice to_sign(OverlayIdShort overlay_id, PublicKeyHash issued_to) const;

  BroadcastCheckResult check(PublicKeyHash node, OverlayIdShort overlay_id, td::int32 unix_time, td::uint32 size,
                             bool is_fec, bool skip_check_signature = false) const;
  tl_object_ptr<ton_api::overlay_Certificate> tl() const;
  const PublicKey &issuer() const;
  const PublicKeyHash issuer_hash() const;

  static td::Result<std::shared_ptr<Certificate>> create(tl_object_ptr<ton_api::overlay_Certificate> cert);
  static tl_object_ptr<ton_api::overlay_Certificate> empty_tl();

 private:
  td::Variant<PublicKey, PublicKeyHash> issued_by_;
  td::int32 expire_at_;
  td::uint32 max_size_;
  td::uint32 flags_;
  td::SharedSlice signature_;
};

class OverlayMemberCertificate {
 public:
  OverlayMemberCertificate() {
    expire_at_ = std::numeric_limits<td::int32>::max();
  }
  OverlayMemberCertificate(PublicKey signed_by, td::uint32 flags, td::int32 slot, td::int32 expire_at,
                         td::BufferSlice signature)
      : signed_by_(std::move(signed_by))
      , flags_(flags)
      , slot_(slot)
      , expire_at_(expire_at)
      , signature_(std::move(signature)) {
  }
  OverlayMemberCertificate(const OverlayMemberCertificate &other)
      : signed_by_(other.signed_by_)
      , flags_(other.flags_)
      , slot_(other.slot_)
      , expire_at_(other.expire_at_)
      , signature_(other.signature_.clone()) {
  }
  OverlayMemberCertificate(OverlayMemberCertificate &&) = default;
  OverlayMemberCertificate &operator=(OverlayMemberCertificate &&) = default;
  OverlayMemberCertificate &operator=(const OverlayMemberCertificate &other) {
    signed_by_ = other.signed_by_;
    flags_ = other.flags_;
    slot_ = other.slot_;
    expire_at_ = other.expire_at_;
    signature_ = other.signature_.clone();
    return *this;
  }
  explicit OverlayMemberCertificate(const ton_api::overlay_MemberCertificate *cert);
  td::Status check_signature(const adnl::AdnlNodeIdShort &node);

  bool is_expired() const {
    return expire_at_ < td::Clocks::system() - 3;
  }

  bool is_expired(double cur_time) const {
    return expire_at_ < cur_time - 3;
  }

  tl_object_ptr<ton_api::overlay_MemberCertificate> tl() const {
    if (empty()) {
      return create_tl_object<ton_api::overlay_emptyMemberCertificate>();
    }
    return create_tl_object<ton_api::overlay_memberCertificate>(signed_by_.tl(), flags_, slot_, expire_at_,
                                                                signature_.clone_as_buffer_slice());
  }

  const auto &issued_by() const {
    return signed_by_;
  }

  td::Slice signature() const {
    return signature_.as_slice();
  }

  td::BufferSlice to_sign_data(const adnl::AdnlNodeIdShort &node) const {
    return ton::create_serialize_tl_object<ton::ton_api::overlay_memberCertificateId>(node.tl(), flags_, slot_,
                                                                                      expire_at_);
  }

  bool empty() const {
    return signed_by_.empty();
  }

  bool is_newer(const OverlayMemberCertificate &other) const {
    return !empty() && expire_at_ > other.expire_at_;
  }

  auto slot() const {
    return slot_;
  }

  auto expire_at() const {
    return expire_at_;
  }

  void set_signature(td::Slice signature) {
    signature_ = td::SharedSlice(signature);
  }
  void set_signature(td::SharedSlice signature) {
    signature_ = std::move(signature);
  }

 private:
  PublicKey signed_by_;
  td::uint32 flags_;
  td::int32 slot_;
  td::int32 expire_at_ = std::numeric_limits<td::int32>::max();
  td::SharedSlice signature_;
};


struct OverlayOptions {
  bool announce_self_ = true;
  bool frequent_dht_lookup_ = false;
  td::uint32 local_overlay_member_flags_ = 0;
  td::int32 max_slaves_in_semiprivate_overlay_ = 5;
  td::uint32 max_peers_ = 20;
  td::uint32 max_neighbours_ = 5;
  td::uint32 nodes_to_send_ = 4;
  td::uint32 propagate_broadcast_to_ = 5;
  td::uint32 default_permanent_members_flags_ = 0;
  double broadcast_speed_multiplier_ = 1.0;
  bool private_ping_peers_ = false;
};

class Overlays : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual void receive_message(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, td::BufferSlice data) = 0;
    virtual void receive_query(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, td::BufferSlice data,
                               td::Promise<td::BufferSlice> promise) = 0;
    virtual void receive_broadcast(PublicKeyHash src, OverlayIdShort overlay_id, td::BufferSlice data) = 0;
    virtual void check_broadcast(PublicKeyHash src, OverlayIdShort overlay_id, td::BufferSlice data,
                                 td::Promise<td::Unit> promise) {
      promise.set_value(td::Unit());
    }
    virtual void get_stats_extra(td::Promise<std::string> promise) {
      promise.set_result("");
    }
    virtual ~Callback() = default;
  };

  static constexpr td::uint32 max_simple_broadcast_size() {
    return 768;
  }
  static constexpr td::uint32 max_message_size() {
    return adnl::Adnl::get_mtu() - 36;
  }
  static constexpr td::uint32 max_fec_broadcast_size() {
    return 16 << 20;
  }

  static constexpr td::uint32 BroadcastFlagAnySender() {
    return 1;
  }

  static constexpr td::uint32 overlay_peer_ttl() {
    return 600;
  }

  static td::actor::ActorOwn<Overlays> create(std::string db_root, td::actor::ActorId<keyring::Keyring> keyring,
                                              td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<dht::Dht> dht);

  virtual void update_dht_node(td::actor::ActorId<dht::Dht> dht) = 0;

  virtual void create_public_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                     std::unique_ptr<Callback> callback, OverlayPrivacyRules rules,
                                     td::string scope) = 0;
  virtual void create_public_overlay_ex(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                        std::unique_ptr<Callback> callback, OverlayPrivacyRules rules, td::string scope,
                                        OverlayOptions opts) = 0;
  virtual void create_semiprivate_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                          std::vector<adnl::AdnlNodeIdShort> nodes,
                                          std::vector<PublicKeyHash> root_public_keys,
                                          OverlayMemberCertificate certificate,
                                          std::unique_ptr<Callback> callback, OverlayPrivacyRules rules,
                                          td::string scope, OverlayOptions opts) = 0;
  virtual void create_private_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                      std::vector<adnl::AdnlNodeIdShort> nodes, std::unique_ptr<Callback> callback,
                                      OverlayPrivacyRules rules, std::string scope) = 0;
  virtual void create_private_overlay_ex(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                                         std::vector<adnl::AdnlNodeIdShort> nodes, std::unique_ptr<Callback> callback,
                                         OverlayPrivacyRules rules, std::string scope, OverlayOptions opts) = 0;
  virtual void delete_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id) = 0;

  virtual void send_query(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                          std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                          td::BufferSlice query) = 0;
  virtual void send_query_via(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                              std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                              td::BufferSlice query, td::uint64 max_answer_size,
                              td::actor::ActorId<adnl::AdnlSenderInterface> via) = 0;
  void send_multiple_messages(std::vector<adnl::AdnlNodeIdShort> dst, adnl::AdnlNodeIdShort src,
                              OverlayIdShort overlay_id, td::BufferSlice object) {
    for (auto &n : dst) {
      send_message(n, src, overlay_id, object.clone());
    }
  }

  virtual void send_message(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                            td::BufferSlice object) = 0;
  virtual void send_message_via(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                                td::BufferSlice object, td::actor::ActorId<adnl::AdnlSenderInterface> via) = 0;

  virtual void send_broadcast(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, td::BufferSlice object) = 0;
  virtual void send_broadcast_ex(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, PublicKeyHash send_as,
                                 td::uint32 flags, td::BufferSlice object) = 0;
  virtual void send_broadcast_fec(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, td::BufferSlice object) = 0;
  virtual void send_broadcast_fec_ex(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, PublicKeyHash send_as,
                                     td::uint32 flags, td::BufferSlice object) = 0;

  virtual void set_privacy_rules(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                 OverlayPrivacyRules rules) = 0;
  virtual void update_certificate(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, PublicKeyHash key,
                                  std::shared_ptr<Certificate> cert) = 0;

  virtual void update_member_certificate(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                         OverlayMemberCertificate certificate) = 0;
  virtual void update_root_member_list(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                                       std::vector<adnl::AdnlNodeIdShort> nodes,
                                       std::vector<PublicKeyHash> root_public_keys,
                                       OverlayMemberCertificate certificate) = 0;

  virtual void get_overlay_random_peers(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay, td::uint32 max_peers,
                                        td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) = 0;
  virtual void get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_overlaysStats>> promise) = 0;

  virtual void forget_peer(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay, adnl::AdnlNodeIdShort peer_id) = 0;
};

}  // namespace overlay

}  // namespace ton

namespace td {

inline StringBuilder &operator<<(StringBuilder &stream, const ton::overlay::OverlayIdShort &value) {
  return stream << value.bits256_value();
}

}  // namespace td
