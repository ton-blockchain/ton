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
  find_out_connection(
      {src, dst}, [self = actor_id(this), data = std::move(data), src, dst](td::Result<OutboundConnection*> R) mutable {
        if (R.is_error()) {
          LOG(ERROR) << R.move_as_error_prefix(PSTRING() << "dropping message " << src << '>' << dst
                                                         << " because connection failed: ");
          return;
        }
        auto conn = R.move_as_ok();
        td::actor::send_closure(self, &QuicSender::send_message_on_connection, conn->local_id, conn->cid,
                                std::move(data), src, dst);
      });
}

void QuicSender::send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                            td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) {
  find_out_connection({src, dst}, [self = actor_id(this), data = std::move(data), promise = std::move(promise), src,
                                   dst](td::Result<OutboundConnection*> R) mutable {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error_prefix(PSTRING()
                                           << "dropping query " << src << '>' << dst << " because connection failed: ");
      return;
    }
    auto conn = R.move_as_ok();
    td::actor::send_closure(self, &QuicSender::send_query_on_connection, conn->local_id, conn->cid, conn,
                            std::move(data), std::move(promise));
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
      // TODO: write some resonable ip here
      P.set_value("<quic connection>");
  });
}

void QuicSender::add_local_id(adnl::AdnlNodeIdShort local_id) {
  add_local_id_coro(local_id).start().detach("add local id");
}
td::actor::Task<> QuicSender::add_local_id_coro(adnl::AdnlNodeIdShort local_id) {
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

    td::Status on_connected(QuicConnectionId cid, td::SecureString peer_public_key) override {
      if (peer_public_key.size() != 32) {
        return td::Status::Error("peer public key must be 32 bytes");
      }
      td::Bits256 key_bits;
      key_bits.as_slice().copy_from(peer_public_key.as_slice());
      auto peer_id = adnl::AdnlNodeIdFull(PublicKey(pubkeys::Ed25519(key_bits))).compute_short_id();

      LOG(INFO) << "Server: client connected with CID: " << cid << ", peer ADNL ID: " << peer_id;
      connections_.emplace(cid, Connection{peer_id});

      // Notify sender that connection is ready (will mark outbound connections as ready)
      td::actor::send_closure(sender_, &QuicSender::after_connection_ready, cid);

      return td::Status::OK();
    }

    void on_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data) override {
      connections_[cid].msg_builders_[sid].append(std::move(data));
    }

    void on_stream_end(QuicConnectionId cid, QuicStreamID sid) override {
      auto& conn = connections_[cid];
      auto data = conn.msg_builders_[sid].extract();
      conn.msg_builders_.erase(sid);

      // Try to parse as Request (for inbound connections)
      auto req_R = fetch_tl_object<ton_api::quic_Request>(data.clone(), true);
      if (req_R.is_ok()) {
        // This is an inbound connection receiving a request
        auto request = req_R.move_as_ok();
        ton_api::downcast_call(*request, td::overloaded(
                                             [this, &conn, cid, sid](ton_api::quic_query& query) {
                                               td::actor::send_closure(sender_, &QuicSender::after_in_query,
                                                                       AdnlPath{conn.peer_id_, local_id_}, cid, sid,
                                                                       std::move(query.data_));
                                             },
                                             [this, &conn](ton_api::quic_message& message) {
                                               td::actor::send_closure(sender_, &QuicSender::after_in_message,
                                                                       AdnlPath{conn.peer_id_, local_id_},
                                                                       std::move(message.data_));
                                             }));
        return;
      }

      // Try to parse as answer (for outbound connections)
      auto answer_R = fetch_tl_object<ton_api::quic_answer>(data.clone(), true);
      if (answer_R.is_ok()) {
        // This is an outbound connection receiving a response
        auto path = AdnlPath{local_id_, conn.peer_id_};
        td::actor::send_closure(sender_, &QuicSender::after_out_query_answer, path, sid,
                                std::move(answer_R.move_as_ok()->data_));
        return;
      }

      LOG(ERROR) << "malformed message from CID, SID:" << sid;
    }

   private:
    struct Connection {
      adnl::AdnlNodeIdShort peer_id_;
      std::unordered_map<QuicStreamID, td::BufferBuilder> msg_builders_;
    };

    adnl::AdnlNodeIdShort local_id_;
    td::actor::ActorId<QuicSender> sender_;

    std::unordered_map<QuicConnectionId, Connection> connections_{};
  };

  auto res = QuicServer::create(port, td::Ed25519::PrivateKey(local_keys_[local_id].copy()),
                                std::make_unique<InConnectionCallback>(local_id, actor_id(this)));
  if (res.is_error())
    LOG(WARNING) << res.move_as_error_prefix(PSTRING()
                                             << "discarding port " << port << " for local id " << local_id << ": ");
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
  iter->second.local_id = path.first;

  td::actor::send_closure(
      adnl_, &adnl::Adnl::get_peer_node, path.first, path.second,
      [this, self = actor_id(this), path](td::Result<adnl::AdnlNode> res) mutable {
        auto iter = outbound_.find(path);
        if (iter == outbound_.end()) {
          LOG(ERROR) << "connection entry disappeared for " << path.first << " -> " << path.second;
          return;
        }
        if (res.is_error()) {
          iter->second.ready.set_error(res.move_as_error_prefix("failed to obtain peer for connection: "));
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

        // Get the QuicServer for the local_id to initiate the outbound connection
        auto server_iter = inbound_.find(path.first);
        if (server_iter == inbound_.end()) {
          iter->second.ready.set_error(td::Status::Error("no QuicServer for local id"));
          outbound_.erase(iter);
          return;
        }

        auto server = server_iter->second.get();
        td::actor::send_closure(
            server, &QuicServer::connect, peer_host, peer_port, std::move(client_key), td::Slice("ton"),
            td::PromiseCreator::lambda([self, path](td::Result<QuicConnectionId> R) {
              if (R.is_error()) {
                td::actor::send_closure(self, &QuicSender::after_out_connection_failed, path, R.move_as_error());
              } else {
                td::actor::send_closure(self, &QuicSender::after_out_connection_established, path, R.move_as_ok());
              }
            }));
      });
}

void QuicSender::after_out_connection_established(AdnlPath path, QuicConnectionId cid) {
  auto iter = outbound_.find(path);
  if (iter == outbound_.end()) {
    LOG(ERROR) << "outbound connection entry disappeared for " << path.first << " -> " << path.second;
    return;
  }
  iter->second.cid = cid;
  cid_to_outbound_path_[cid] = path;
}

void QuicSender::after_connection_ready(QuicConnectionId cid) {
  // Check if this is an outbound connection and mark it ready
  auto path_it = cid_to_outbound_path_.find(cid);
  if (path_it != cid_to_outbound_path_.end()) {
    auto outbound_it = outbound_.find(path_it->second);
    if (outbound_it != outbound_.end()) {
      outbound_it->second.ready.set_result(&outbound_it->second);
      outbound_it->second.ready_received = true;
    }
  }
}

void QuicSender::after_out_connection_failed(AdnlPath path, td::Status error) {
  auto iter = outbound_.find(path);
  if (iter != outbound_.end()) {
    iter->second.ready.set_error(std::move(error));
    outbound_.erase(iter);
  }
}

void QuicSender::send_message_on_connection(adnl::AdnlNodeIdShort local_id, QuicConnectionId cid, td::BufferSlice data,
                                            adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst) {
  auto it = inbound_.find(local_id);
  if (it == inbound_.end()) {
    LOG(ERROR) << "dropping message " << src << '>' << dst << " because no QuicServer for local_id";
    return;
  }
  auto server = it->second.get();
  td::actor::send_closure(
      server, &QuicServer::open_stream, cid,
      [server, cid, data = std::move(data), src, dst](td::Result<QuicStreamID> res) mutable {
        if (res.is_error()) {
          LOG(ERROR) << res.move_as_error_prefix(PSTRING() << "dropping message " << src << '>' << dst
                                                           << " because stream failed: ");
          return;
        }
        auto sid = res.move_as_ok();
        td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_message>(std::move(data));
        td::actor::send_closure(server, &QuicServer::send_stream_data, cid, sid, std::move(wire_data));
        td::actor::send_closure(server, &QuicServer::send_stream_end, cid, sid);
      });
}

void QuicSender::send_query_on_connection(adnl::AdnlNodeIdShort local_id, QuicConnectionId cid,
                                          OutboundConnection* conn, td::BufferSlice data,
                                          td::Promise<td::BufferSlice> promise) {
  auto it = inbound_.find(local_id);
  if (it == inbound_.end()) {
    promise.set_error(td::Status::Error("no QuicServer for local_id"));
    return;
  }
  auto server = it->second.get();
  td::actor::send_closure(server, &QuicServer::open_stream, cid,
                          td::promise_send_closure(actor_id(this), &QuicSender::after_out_query_stream_obtained, conn,
                                                   std::move(data), std::move(promise)));
}

void QuicSender::after_out_query_stream_obtained(OutboundConnection* conn, td::BufferSlice query_data,
                                                 td::Promise<td::BufferSlice> answer_promise,
                                                 td::Result<QuicStreamID> sid_res) {
  if (sid_res.is_error()) {
    LOG(ERROR) << sid_res.move_as_error_prefix(PSTRING() << "dropping query because stream failed: ");
    answer_promise.set_error(sid_res.move_as_error());
    return;
  }
  auto sid = sid_res.move_as_ok();
  conn->responses.emplace(sid, std::move(answer_promise));
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_query>(std::move(query_data));
  auto it = inbound_.find(conn->local_id);
  if (it == inbound_.end()) {
    LOG(ERROR) << "dropping query because no QuicServer for local_id";
    return;
  }
  auto server = it->second.get();
  td::actor::send_closure(server, &QuicServer::send_stream_data, conn->cid, sid, std::move(wire_data));
  td::actor::send_closure(server, &QuicServer::send_stream_end, conn->cid, sid);
}

void QuicSender::after_out_query_answer(AdnlPath path, QuicStreamID sid, td::BufferSlice data) {
  auto conn = &outbound_.find(path)->second;
  conn->responses[sid].set_result(std::move(data));
}

void QuicSender::after_in_query(AdnlPath path, QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data) {
  td::actor::send_closure(
      adnl_, &adnl::AdnlPeerTable::deliver_query, path.first, path.second, std::move(data),
      [self = actor_id(this), cid, sid, local_id = path.second](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          LOG(ERROR) << R.move_as_error_prefix("adnl failed to deliver query, not sending any answer: ");
          return;
        }
        td::actor::send_closure(self, &QuicSender::after_in_query_answer, local_id, cid, sid, R.move_as_ok());
      });
}

void QuicSender::after_in_query_answer(adnl::AdnlNodeIdShort local_id, QuicConnectionId cid, QuicStreamID sid,
                                       td::BufferSlice data) {
  td::BufferSlice wire_data = create_serialize_tl_object<ton_api::quic_answer>(std::move(data));
  auto server = inbound_[local_id].get();
  td::actor::send_closure(server, &QuicServer::send_stream_data, cid, sid, std::move(wire_data));
  td::actor::send_closure(server, &QuicServer::send_stream_end, cid, sid);
}

void QuicSender::after_in_message(AdnlPath path, td::BufferSlice data) {
  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, path.first, path.second, std::move(data));
}

}  // namespace ton::quic
