#include <utility>

#include "adnl/adnl-peer-table.h"
#include "auto/tl/ton_api.hpp"
#include "td/actor/coro_utils.h"
#include "td/utils/overloaded.h"

#include "quic-sender.h"

namespace ton::quic {
QuicSender::QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::actor::ActorId<keyring::Keyring> keyring)
    : adnl_(std::move(adnl)), keyring_(std::move(keyring)) {
}

void QuicSender::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  find_out_connection({src, dst}, [data = std::move(data), src, dst](td::Result<OutboundConnection*> R) mutable {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error_prefix(PSTRING()
                                           << "dropping message " << src << '>' << dst << " because connection failed");
      return;
    }
    auto client = R.move_as_ok()->client.get();
    td::actor::send_closure(client, &QuicClient::open_stream,
                            [data = std::move(data), client, src, dst](td::Result<QuicStreamID> res) mutable {
                              if (res.is_error()) {
                                LOG(ERROR) << res.move_as_error_prefix(PSTRING() << "dropping message " << src << '>'
                                                                                 << dst << " because stream failed");
                                return;
                              }
                              auto sid = res.move_as_ok();
                              td::BufferSlice wire_data =
                                  create_serialize_tl_object<ton_api::quic_message>(std::move(data));
                              td::actor::send_closure(client, &QuicClient::send_stream_data, sid, std::move(wire_data));
                              td::actor::send_closure(client, &QuicClient::send_stream_end, sid);
                            });
  });
}

void QuicSender::send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                            td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) {
  find_out_connection({src, dst}, [data = std::move(data), promise = std::move(promise), src,
                                   dst](td::Result<OutboundConnection*> R) mutable {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error_prefix(PSTRING()
                                           << "dropping query " << src << '>' << dst << " because connection failed");
      return;
    }
    auto conn = R.move_as_ok();
    auto client = conn->client.get();
    td::actor::send_closure(client, &QuicClient::open_stream,
                            [data = std::move(data), promise = std::move(promise), conn, client, src,
                             dst](td::Result<QuicStreamID> res) mutable {
                              if (res.is_error()) {
                                LOG(ERROR) << res.move_as_error_prefix(PSTRING() << "dropping query " << src << '>'
                                                                                 << dst << " because stream failed");
                                return;
                              }
                              auto sid = res.move_as_ok();
                              conn->responses.emplace(sid, std::move(promise));
                              td::BufferSlice wire_data =
                                  create_serialize_tl_object<ton_api::quic_query>(std::move(data));
                              td::actor::send_closure(client, &QuicClient::send_stream_data, sid, std::move(wire_data));
                              td::actor::send_closure(client, &QuicClient::send_stream_end, sid);
                            });
  });
}

void QuicSender::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                               td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                               td::uint64 max_answer_size) {
  send_query(src, dst, name, std::move(promise), timeout, std::move(data));
}

void QuicSender::get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                                 td::Promise<td::string> promise) {
  find_out_connection({l_id, p_id}, [P = std::move(promise)](td::Result<OutboundConnection*> R) mutable {
    if (R.is_error())
      P.set_error(R.move_as_error());
    else
      P.set_value(PSTRING() << R.move_as_ok()->client.as_actor_ref().actor_info.get_name());
  });
}

td::actor::Task<> QuicSender::add_local_id(adnl::AdnlNodeIdShort local_id) {
  adnl::AdnlNode node = co_await td::actor::ask(adnl_, &adnl::Adnl::get_self_node, local_id);
  std::set<int> ports;
  for (const auto& addr : node.addr_list().addrs()) {
    auto r_ip = addr->to_ip_address();
    if (r_ip.is_ok() && r_ip.ok().get_port() != 0) {
      ports.insert(r_ip.ok().get_port());
    }
  }
  if (ports.size() == 0) {
    LOG(WARNING) << "local id " << local_id << " has no ports";
    co_return td::Unit{};
  }
  if (ports.size() > 1) {
    LOG(WARNING) << "discarding " << ports.size() - 1 << " redundant ports of local id " << local_id;
  }
  auto port = *ports.begin() + NODE_PORT_OFFSET;

  // This is kinda ugly
  PrivateKey priv_key =
      co_await td::actor::ask(keyring_, &keyring::Keyring::export_private_key, local_id.pubkey_hash());
  auto ed25519_key_res = priv_key.export_as_ed25519();
  if (ed25519_key_res.is_error()) {
    LOG(WARNING) << "failed to convert private key for local id " << local_id << ": " << ed25519_key_res.error();
    co_return td::Unit{};
  }
  // Store the raw key bytes for this local_id
  local_keys_.emplace(local_id, ed25519_key_res.ok().as_octet_string().copy());

  class InConnectionCallback : public QuicServer::Callback {
   public:
    InConnectionCallback(adnl::AdnlNodeIdShort local_id, td::actor::ActorId<QuicSender> sender)
        : local_id_(local_id), sender_(sender) {
    }

    void on_connected(const td::IPAddress& peer, td::SecureString peer_public_key) override {
      // TODO: Use peer_public_key for authentication instead of IP
      LOG(INFO) << "Server: client connected from " << peer << " with public key: "
                << (peer_public_key.empty() ? "<none>" : td::base64_encode(peer_public_key.as_slice()));
      inbound_.emplace(peer.get_ip_str().c_str(), InboundConnection{});
    }

    void on_stream_data(const td::IPAddress& peer, QuicStreamID sid, td::BufferSlice data) override {
      inbound_[peer.get_ip_str().c_str()].msg_builders_[sid].append(std::move(data));
    }

    void on_stream_end(const td::IPAddress& peer, QuicStreamID sid) override {
      auto& conn = inbound_[peer.get_ip_str().c_str()];
      auto data = conn.msg_builders_[sid].extract();
      conn.msg_builders_.erase(sid);
      auto R = fetch_tl_object<ton_api::quic_Request>(data, true);
      if (R.is_error()) {
        LOG(ERROR) << "malformed request from " << peer << " SID:" << sid;
        return;
      }
      auto response = R.move_as_ok();
      ton_api::downcast_call(*response, td::overloaded(
                                            [this, &conn, peer, sid](const ton_api::quic_init& init) {
                                              conn.peer_id_ = adnl::AdnlNodeIdShort{init.local_id_};
                                              auto local_id = adnl::AdnlNodeIdShort{init.peer_id_};
                                              if (local_id != local_id_) {
                                                LOG(WARNING) << "adnl id mismatch ignored";
                                              }
                                              td::actor::send_closure(sender_, &QuicSender::after_in_init,
                                                                      AdnlPath{conn.peer_id_, local_id_}, peer, sid);
                                            },
                                            [this, &conn, peer, sid](ton_api::quic_query& query) {
                                              td::actor::send_closure(sender_, &QuicSender::after_in_query,
                                                                      AdnlPath{conn.peer_id_, local_id_}, peer, sid,
                                                                      std::move(query.data_));
                                            },
                                            [this, &conn](ton_api::quic_message& message) {
                                              td::actor::send_closure(sender_, &QuicSender::after_in_message,
                                                                      AdnlPath{conn.peer_id_, local_id_},
                                                                      std::move(message.data_));
                                            }));
    }

   private:
    struct InboundConnection {
      adnl::AdnlNodeIdShort peer_id_;
      std::unordered_map<QuicStreamID, td::BufferBuilder> msg_builders_;
    };

    adnl::AdnlNodeIdShort local_id_;
    td::actor::ActorId<QuicSender> sender_;

    std::unordered_map<std::string, InboundConnection> inbound_{};
  };

  auto res = QuicServer::listen_rpk(port, td::Ed25519::PrivateKey(local_keys_[local_id].copy()),
                                    std::make_unique<InConnectionCallback>(local_id, actor_id(this)));
  if (res.is_error())
    LOG(WARNING) << res.move_as_error_prefix(PSTRING() << "discarding port " << port << " for local id " << local_id);
  else
    inbound_[local_id] = res.move_as_ok();

  co_return td::Unit{};
}

void QuicSender::find_out_connection(AdnlPath path, td::Promise<OutboundConnection*> P) {
  auto iter = outbound_.find(path);
  if (iter == outbound_.end())
    create_connection(path, std::move(P));
  else {
    if (iter->second.ready_received)
      P.set_value(&iter->second);
    else {
      iter->second.ready = [P = std::move(P),
                            P1 = std::move(iter->second.ready)](td::Result<OutboundConnection*> R) mutable {
        P1.set_result(R.clone());
        P.set_result(std::move(R));
      };
    }
  }
}

void QuicSender::create_connection(AdnlPath path, td::Promise<OutboundConnection*> P) {
  // Add placeholder immediately to prevent duplicate connections from concurrent queries
  auto iter = outbound_.emplace(path, OutboundConnection{}).first;
  iter->second.ready = std::move(P);

  td::actor::send_closure(
      adnl_, &adnl::Adnl::get_peer_node, path.first, path.second,
      [this, self = actor_id(this), path](td::Result<adnl::AdnlNode> res) mutable {
        auto iter = outbound_.find(path);
        if (iter == outbound_.end()) {
          LOG(ERROR) << "connection entry disappeared for " << path.first << " -> " << path.second;
          return;
        }
        if (res.is_error()) {
          iter->second.ready.set_error(res.move_as_error_prefix("failed to obtain peer for connection"));
          outbound_.erase(iter);
          return;
        }
        auto node = res.move_as_ok();
        td::IPAddress peer_addr;
        for (const auto& addr : node.addr_list().addrs()) {
          auto r_ip = addr->to_ip_address();
          if (r_ip.is_ok()) {
            peer_addr = r_ip.move_as_ok();
          }
        }
        if (!peer_addr.is_valid()) {
          iter->second.ready.set_error(td::Status::Error("no ip address for peer"));
          outbound_.erase(iter);
          return;
        }
        auto peer_host = peer_addr.get_ip_host();
        auto peer_port = peer_addr.get_port() + NODE_PORT_OFFSET;

        // Get client's key for this local ID
        auto local_key_iter = local_keys_.find(path.first);
        if (local_key_iter == local_keys_.end()) {
          iter->second.ready.set_error(td::Status::Error("no local key for source ADNL ID"));
          outbound_.erase(iter);
          return;
        }
        auto client_key = td::Ed25519::PrivateKey(local_key_iter->second.copy());

        class OutConnectionCallback : public QuicClient::Callback {
         public:
          OutConnectionCallback(AdnlPath path, td::actor::ActorId<QuicSender> sender)
              : sender_(std::move(sender)), path_(std::move(path)) {
          }

          void on_connected(td::SecureString peer_public_key) override {
            // TODO: Verify peer_public_key matches expected server identity
            LOG(INFO) << "Client: connected to " << path_.second << " with server public key: "
                      << (peer_public_key.empty() ? "<none>" : td::base64_encode(peer_public_key.as_slice()));
            td::actor::send_closure(sender_, &QuicSender::after_out_connection_created, path_);
          }

          void on_stream_data(QuicStreamID sid, td::BufferSlice data) override {
            msg_builders_[sid].append(std::move(data));
          }

          void on_stream_end(QuicStreamID sid) override {
            auto data = msg_builders_[sid].extract();
            msg_builders_.erase(sid);
            auto R = fetch_tl_object<ton_api::quic_Response>(data, true);
            if (R.is_error()) {
              LOG(ERROR) << "malformed response from " << path_.second << " SID:" << sid;
              return;
            }
            auto response = R.move_as_ok();
            ton_api::downcast_call(
                *response, td::overloaded(
                               [this](const ton_api::quic_ready&) {
                                 td::actor::send_closure(sender_, &QuicSender::after_out_connection_ready, path_);
                               },
                               [this, sid](ton_api::quic_answer& answer) {
                                 td::actor::send_closure(sender_, &QuicSender::after_out_query_answer, path_, sid,
                                                         std::move(answer.data_));
                               }));
          }

         private:
          td::actor::ActorId<QuicSender> sender_;
          AdnlPath path_;

          std::unordered_map<QuicStreamID, td::BufferBuilder> msg_builders_;
        };

        // TODO: maybe we should check the peer key during handshake and not after
        auto conn_res = QuicClient::connect_rpk(peer_host, peer_port, std::move(client_key),
                                                std::make_unique<OutConnectionCallback>(path, self));
        if (conn_res.is_error()) {
          iter->second.ready.set_error(conn_res.move_as_error());
          outbound_.erase(iter);
        } else {
          iter->second.client = conn_res.move_as_ok();
        }
      });
}

void QuicSender::after_out_connection_created(AdnlPath path) {
  auto client = outbound_.find(path)->second.client.get();
  td::actor::send_closure(client, &QuicClient::open_stream, [path, client](td::Result<QuicStreamID> res) mutable {
    if (res.is_error()) {
      LOG(ERROR) << res.move_as_error_prefix(PSTRING() << "failed to open stream for connection " << path.first << '>'
                                                       << path.second);
      return;
    }
    auto sid = res.move_as_ok();
    td::BufferSlice data = create_serialize_tl_object<ton_api::quic_init>(path.first.tl(), path.second.tl());
    td::actor::send_closure(client, &QuicClient::send_stream_data, sid, std::move(data));
    td::actor::send_closure(client, &QuicClient::send_stream_end, sid);
  });
}

void QuicSender::after_out_connection_ready(AdnlPath path) {
  auto conn = &outbound_.find(path)->second;
  conn->ready.set_result(conn);
  conn->ready_received = true;
}

void QuicSender::after_out_query_answer(AdnlPath path, QuicStreamID sid, td::BufferSlice data) {
  auto conn = &outbound_.find(path)->second;
  conn->responses[sid].set_result(std::move(data));
}

void QuicSender::after_in_init(AdnlPath path, td::IPAddress peer, QuicStreamID sid) {
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_ready>();
  auto server = inbound_[path.second].get();
  td::actor::send_closure(server, &QuicServer::send_stream_data, peer, sid, std::move(wire_data));
  td::actor::send_closure(server, &QuicServer::send_stream_end, peer, sid);
}

void QuicSender::after_in_query(AdnlPath path, td::IPAddress peer, QuicStreamID sid, td::BufferSlice data) {
  td::actor::send_closure(
      adnl_, &adnl::AdnlPeerTable::deliver_query, path.first, path.second, std::move(data),
      [self = actor_id(this), peer, sid, local_id = path.second](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          LOG(ERROR) << R.move_as_error_prefix("adnl failed to deliver query, not sending any answer");
          return;
        }
        td::actor::send_closure(self, &QuicSender::after_in_query_answer, local_id, peer, sid, R.move_as_ok());
      });
}

void QuicSender::after_in_query_answer(adnl::AdnlNodeIdShort local_id, td::IPAddress peer, QuicStreamID sid,
                                       td::BufferSlice data) {
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_answer>(std::move(data));
  auto server = inbound_[local_id].get();
  td::actor::send_closure(server, &QuicServer::send_stream_data, peer, sid, std::move(wire_data));
  td::actor::send_closure(server, &QuicServer::send_stream_end, peer, sid);
}

void QuicSender::after_in_message(AdnlPath path, td::BufferSlice data) {
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, path.first, path.second, std::move(data));
}

}  // namespace ton::quic
