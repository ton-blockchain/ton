#include "td/actor/actor.h"

#include "quic-pimpl.h"
#include "quic-server.h"

namespace ton::quic {

td::Result<td::actor::ActorOwn<QuicServer>> QuicServer::listen(td::Slice bind_host, int port, td::Slice cert_file,
                                                             td::Slice key_file, std::unique_ptr<Callback> callback,
                                                             td::Slice alpn) {
  td::IPAddress bind_addr;
  std::string host_c(bind_host.begin(), bind_host.end());
  TRY_STATUS(bind_addr.init_host_port(td::CSlice(host_c.c_str()), port));

  TRY_RESULT_ASSIGN(auto fd, td::UdpSocketFd::open(bind_addr));
  TRY_RESULT_ASSIGN(auto local, fd.get_local_address());

  auto name = PSTRING() << "QUIC:[" << local << "]";
  return td::actor::create_actor<QuicServer>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             local, cert_file, key_file, alpn, std::move(callback));
}

QuicServer::QuicServer(td::UdpSocketFd fd, td::IPAddress local_address, td::Slice cert_file, td::Slice key_file,
                       td::Slice alpn, std::unique_ptr<Callback> callback)
    : fd_(std::move(fd))
    , local_address_(local_address)
    , cert_file_(cert_file.begin(), cert_file.end())
    , key_file_(key_file.begin(), key_file.end())
    , alpn_(alpn.begin(), alpn.end())
    , callback_(std::move(callback)) {
}

void QuicServer::start_up() {
  LOG(INFO) << "starting up";
  self_id_ = actor_id(this);
  td::actor::SchedulerContext::get()->get_poll().subscribe(fd_.get_poll_info().extract_pollable_fd(this),
                                                           td::PollFlags::ReadWrite());
  LOG(INFO) << "startup completed";
}

void QuicServer::tear_down() {
}

void QuicServer::hangup() {
  LOG(ERROR) << "unexpected hangup signal";
}

void QuicServer::hangup_shared() {
  LOG(ERROR) << "unexpected hangup_shared signal";
}

void QuicServer::wake_up() {
  LOG(ERROR) << "unexpected wake_up signal";
}

void QuicServer::alarm() {
}

void QuicServer::loop() {
  LOG(ERROR) << "unexpected loop signal";
}

void QuicServer::notify() {
  td::actor::send_closure(self_id_, &QuicServer::on_fd_notify);
}

QuicServer::PeerState* QuicServer::get_peer_state(const td::IPAddress& peer) {
  auto it = peers_.find(peer);
  if (it == peers_.end()) {
    return nullptr;
  }
  return &it->second;
}

void QuicServer::drop_peer(const td::IPAddress& peer) {
  auto it = peers_.find(peer);
  if (it != peers_.end()) {
    peers_.erase(it);
  }
}

td::Status QuicServer::ensure_conn(const td::IPAddress& peer, td::Slice datagram) {
  auto* st = get_peer_state(peer);
  if (st && st->pimpl) {
    return td::Status::OK();
  }

  ngtcp2_version_cid vc{};
  int rv = ngtcp2_pkt_decode_version_cid(&vc, reinterpret_cast<const uint8_t*>(datagram.data()), datagram.size(), 0);
  if (rv < 0 || vc.version == 0) {
    return td::Status::Error("unexpected first packet");
  }

  PeerState& pst = peers_[peer];
  pst.pimpl = std::make_unique<QuicConnectionPImpl>();
  pst.pimpl->local_address = local_address_;
  pst.pimpl->remote_address = peer;

  class PImplCallback : public QuicConnectionPImpl::Callback {
   public:
    PImplCallback(QuicServer& server, td::IPAddress peer) : server_(server), peer_(peer) {
    }

    void on_handshake_completed(const HandshakeCompletedEvent&) override {
      server_.callback_->on_client_connected(peer_);
    }

    void on_stream_data(const StreamDataEvent& event) override {
      server_.callback_->on_client_data(peer_, event.data, event.fin);
      if (event.fin) {
      }
    }

   private:
    QuicServer& server_;
    td::IPAddress peer_;
  };

  pst.pimpl->callback = std::make_unique<PImplCallback>(*this, peer);

  TRY_STATUS(pst.pimpl->init_tls_server(cert_file_, key_file_, alpn_));
  TRY_STATUS(pst.pimpl->init_quic_server(vc));

  // TRY_STATUS(pst.pimpl->flush_egress(TODO));

  return td::Status::OK();
}

void QuicServer::on_fd_notify() {
  td::sync_with_poll(fd_);

  // while (true) {
  //   uint8_t buf[QuicConnectionPImpl::DEFAULT_MTU];
  //   td::MutableSlice buf_slice(reinterpret_cast<char*>(buf), reinterpret_cast<char*>(buf) + sizeof(buf));
  //
  //   td::IPAddress from;
  //   bool is_received;
  //   td::UdpSocketFd::InboundMessage msg{.from = &from, .data = buf_slice, .error = nullptr};
  //   auto st = fd_.receive_message(msg, is_received);
  //   if (st.is_error()) {
  //     LOG(ERROR) << "receive_message failed: " << st;
  //     break;
  //   }
  //   if (!is_received) {
  //     break;
  //   }
  //
  //   td::Slice datagram(msg.data.data(), msg.data.size());
  //
  //   auto status = ensure_conn(from, datagram);
  //   if (status.is_error()) {
  //     LOG(ERROR) << "dropping datagram from " << from << ": " << status;
  //     continue;
  //   }
  //
  //   auto* peer_state = get_peer_state(from);
  //   if (!peer_state || !peer_state->pimpl) {
  //     continue;
  //   }
  //
  //   status = peer_state->pimpl->handle_ingress_packet(datagram);
  //   if (status.is_error()) {
  //     LOG(ERROR) << "connection aborted for " << from << ": " << status;
  //     callback_->on_client_disconnected(from);
  //     drop_peer(from);
  //     continue;
  //   }
  //
  //   status = peer_state->pimpl->flush_egress(TODO);
  //   if (status.is_error()) {
  //     LOG(ERROR) << "flush_egress failed for " << from << ": " << status;
  //     callback_->on_client_disconnected(from);
  //     drop_peer(from);
  //   }
  // }
}

void QuicServer::process_operation_status(td::Status status) {
  if (status.is_error()) {
    LOG(ERROR) << "operation failed: " << status;
  }
}

void QuicServer::send_data(const td::IPAddress& peer, td::Slice data) {
  auto* st = get_peer_state(peer);
  if (!st || !st->pimpl) {
    LOG(ERROR) << "unknown peer: " << peer;
    return;
  }
  process_operation_status(st->pimpl->write_reply(data, false));
  // process_operation_status(st->pimpl->flush_egress(TODO));
}

void QuicServer::send_disconnect(const td::IPAddress& peer) {
  auto* st = get_peer_state(peer);
  if (!st || !st->pimpl) {
    LOG(ERROR) << "unknown peer: " << peer;
    return;
  }
  process_operation_status(st->pimpl->write_reply(td::Slice(), true));
  // process_operation_status(st->pimpl->flush_egress(TODO));
}

}  // namespace ton::quic
