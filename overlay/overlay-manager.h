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

#include <map>

#include "td/actor/actor.h"
#include "td/db/KeyValueAsync.h"

#include "adnl/adnl.h"
#include "dht/dht.h"

#include "overlays.h"
#include "overlay-id.hpp"

namespace ton {

namespace overlay {

constexpr int VERBOSITY_NAME(OVERLAY_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(OVERLAY_NOTICE) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(OVERLAY_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(OVERLAY_DEBUG) = verbosity_DEBUG + 1;
constexpr int VERBOSITY_NAME(OVERLAY_EXTRA_DEBUG) = verbosity_DEBUG + 10;

class Overlay;

class OverlayManager : public Overlays {
 public:
  OverlayManager(std::string db_root, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
                 td::actor::ActorId<dht::Dht> dht);
  void start_up() override;
  void save_to_db(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, std::vector<OverlayNode> nodes);

  void update_dht_node(td::actor::ActorId<dht::Dht> dht) override;

  void create_public_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                             std::unique_ptr<Callback> callback, OverlayPrivacyRules rules, td::string scope) override;
  void create_private_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdFull overlay_id,
                              std::vector<adnl::AdnlNodeIdShort> nodes, std::unique_ptr<Callback> callback,
                              OverlayPrivacyRules rules) override;
  void delete_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id) override;
  void send_query(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice query) override {
    send_query_via(dst, src, overlay_id, std::move(name), std::move(promise), timeout, std::move(query),
                   adnl::Adnl::huge_packet_max_size(), adnl_);
  }
  void send_query_via(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, std::string name,
                      td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice query,
                      td::uint64 max_answer_size, td::actor::ActorId<adnl::AdnlSenderInterface> via) override;
  void send_message(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                    td::BufferSlice object) override {
    send_message_via(dst, src, overlay_id, std::move(object), adnl_);
  }
  void send_message_via(adnl::AdnlNodeIdShort dst, adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id,
                        td::BufferSlice object, td::actor::ActorId<adnl::AdnlSenderInterface> via) override;

  void send_broadcast(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, td::BufferSlice object) override;
  void send_broadcast_ex(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, PublicKeyHash send_as, td::uint32 flags,
                         td::BufferSlice object) override;
  void send_broadcast_fec(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, td::BufferSlice object) override;
  void send_broadcast_fec_ex(adnl::AdnlNodeIdShort src, OverlayIdShort overlay_id, PublicKeyHash send_as,
                             td::uint32 flags, td::BufferSlice object) override;

  void set_privacy_rules(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, OverlayPrivacyRules rules) override;
  void update_certificate(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id, PublicKeyHash key,
                          std::shared_ptr<Certificate> cert) override;

  void get_overlay_random_peers(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay, td::uint32 max_peers,
                                td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) override;

  void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise);
  void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data);

  void register_overlay(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                        td::actor::ActorOwn<Overlay> overlay);
  void get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_overlaysStats>> promise) override;

  struct PrintId {};

  PrintId print_id() const {
    return PrintId{};
  }

 private:
  std::map<adnl::AdnlNodeIdShort, std::map<OverlayIdShort, td::actor::ActorOwn<Overlay>>> overlays_;

  std::string db_root_;

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<dht::Dht> dht_node_;

  using DbType = td::KeyValueAsync<td::Bits256, td::BufferSlice>;
  DbType db_;

  class AdnlCallback : public adnl::Adnl::Callback {
   public:
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &OverlayManager::receive_message, src, dst, std::move(data));
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &OverlayManager::receive_query, src, dst, std::move(data), std::move(promise));
    }
    AdnlCallback(td::actor::ActorId<OverlayManager> id) : id_(id) {
    }

   private:
    td::actor::ActorId<OverlayManager> id_;
  };
};

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const OverlayManager::PrintId &id) {
  sb << "[overlaymanager]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const OverlayManager &manager) {
  sb << manager.print_id();
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const OverlayManager *manager) {
  sb << manager->print_id();
  return sb;
}

}  // namespace overlay

}  // namespace ton
