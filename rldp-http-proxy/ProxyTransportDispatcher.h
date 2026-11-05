/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.

    Copyright 2019-2020 Telegram Systems LLP
*/
#pragma once

#include "adnl/adnl.h"
#include "auto/tl/ton_api.h"
#include "td/utils/LRUCache.h"
#include "tl-utils/common-utils.hpp"

class ProxyTransportDispatcher : public ton::adnl::AdnlSenderInterface {
 public:
  static constexpr td::uint64 CAPABILITY_RLDP2 = 1;
  static constexpr td::uint64 CAPABILITY_QUIC = 2;
  static constexpr td::uint64 CAPABILITIES = CAPABILITY_RLDP2 | CAPABILITY_QUIC;

  ProxyTransportDispatcher(td::actor::ActorId<ton::adnl::AdnlSenderInterface> rldp,
                           td::actor::ActorId<ton::adnl::AdnlSenderInterface> quic,
                           td::actor::ActorId<ton::adnl::AdnlSenderInterface> adnl)
      : rldp_(rldp), quic_(quic), adnl_(adnl) {
  }

  void send_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    td::actor::send_closure(dispatch(src, dst), &ton::adnl::AdnlSenderInterface::send_message, src, dst,
                            std::move(data));
  }

  void send_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    td::actor::send_closure(dispatch(src, dst), &ton::adnl::AdnlSenderInterface::send_query, src, dst, std::move(name),
                            std::move(promise), timeout, std::move(data));
  }

  void send_query_ex(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override {
    td::actor::send_closure(dispatch(src, dst), &ton::adnl::AdnlSenderInterface::send_query_ex, src, dst,
                            std::move(name), std::move(promise), timeout, std::move(data), max_answer_size);
  }

  void get_conn_ip_str(ton::adnl::AdnlNodeIdShort local_id, ton::adnl::AdnlNodeIdShort peer,
                       td::Promise<td::string> promise) override {
    td::actor::send_closure(rldp_, &ton::adnl::AdnlSenderInterface::get_conn_ip_str, local_id, peer,
                            std::move(promise));
  }

  void update_peer_capabilities(ton::adnl::AdnlNodeIdShort local_id, ton::adnl::AdnlNodeIdShort peer,
                                td::uint64 capabilities) {
    auto &state = peer_capabilities_.get({local_id, peer});
    state.capabilities = capabilities;
    state.received = true;
  }

  void ask_peer_capabilities(ton::adnl::AdnlNodeIdShort local_id, ton::adnl::AdnlNodeIdShort peer) {
    auto &state = peer_capabilities_.get({local_id, peer});
    if (state.received || !state.retry_at.is_in_past()) {
      return;
    }
    state.retry_at = td::Timestamp::in(30.0);
    td::actor::send_closure(
        adnl_, &ton::adnl::AdnlSenderInterface::send_query, local_id, peer, "HTTP capabilities",
        [SelfId = actor_id(this), local_id, peer](td::Result<td::BufferSlice> result) {
          if (result.is_error()) {
            return;
          }
          auto capabilities = ton::fetch_tl_object<ton::ton_api::http_proxy_capabilities>(result.move_as_ok(), true);
          if (capabilities.is_ok()) {
            td::actor::send_closure(SelfId, &ProxyTransportDispatcher::update_peer_capabilities, local_id, peer,
                                    static_cast<td::uint64>(capabilities.ok()->capabilities_));
          }
        },
        td::Timestamp::in(3.0),
        ton::create_serialize_tl_object<ton::ton_api::http_proxy_getCapabilities>(CAPABILITIES));
  }

 private:
  td::actor::ActorId<ton::adnl::AdnlSenderInterface> rldp_;
  td::actor::ActorId<ton::adnl::AdnlSenderInterface> quic_;
  td::actor::ActorId<ton::adnl::AdnlSenderInterface> adnl_;

  struct PeerCapabilities {
    td::uint64 capabilities = 0;
    bool received = false;
    td::Timestamp retry_at = td::Timestamp::now();
  };
  td::LRUCache<std::pair<ton::adnl::AdnlNodeIdShort, ton::adnl::AdnlNodeIdShort>, PeerCapabilities> peer_capabilities_{
      10000};

  td::actor::ActorId<ton::adnl::AdnlSenderInterface> dispatch(ton::adnl::AdnlNodeIdShort src,
                                                              ton::adnl::AdnlNodeIdShort dst) {
    ask_peer_capabilities(src, dst);
    if (peer_capabilities_.get({src, dst}).capabilities & CAPABILITY_QUIC) {
      LOG(DEBUG) << "Selected HTTP transport: QUIC";
      return quic_;
    }
    LOG(DEBUG) << "Selected HTTP transport: RLDP2";
    return rldp_;
  }
};
