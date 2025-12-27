#include "td/actor/actor.h"

#include "quic-client.h"
#include "quic-pimpl.h"

namespace ton::quic {
td::Result<td::actor::ActorOwn<QuicClient>> QuicClient::connect(td::Slice host, int port,
                                                                std::unique_ptr<Callback> callback, td::Slice alpn,
                                                                int local_port) {
  auto p_impl = std::make_unique<QuicConnectionPImpl>();
  std::string host_c(host.begin(), host.end());
  TRY_STATUS(p_impl->remote_address.init_host_port(td::CSlice(host_c.c_str()), port));

  td::IPAddress local_addr;
  TRY_STATUS(local_addr.init_host_port("0.0.0.0", local_port));

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));
  TRY_RESULT_ASSIGN(p_impl->local_address, fd.get_local_address());

  TRY_STATUS(p_impl->init_tls_client(host, alpn));
  TRY_STATUS(p_impl->init_quic_client());

  auto name = PSTRING() << "QUIC:" << p_impl->local_address << ">[" << host << ':' << port << ']';
  return td::actor::create_actor<QuicClient>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             std::move(p_impl), std::move(callback));
}

void QuicClient::start_up() {
  LOG(INFO) << "starting up";
  self_id_ = actor_id(this);
  td::actor::SchedulerContext::get()->get_poll().subscribe(fd_.get_poll_info().extract_pollable_fd(this),
                                                           td::PollFlags::ReadWrite());
  flush_egress();
  LOG(INFO) << "startup completed";
}

void QuicClient::tear_down() {
  // TODO(@avevad): close connection cleanly
}

void QuicClient::hangup() {
  // not used
  LOG(ERROR) << "unexpected hangup signal";
}

void QuicClient::hangup_shared() {
  // not used
  LOG(ERROR) << "unexpected hangup_shared signal";
}

void QuicClient::wake_up() {
  // not used
  LOG(ERROR) << "unexpected wake_up signal";
}
void QuicClient::alarm() {
  // TODO(@avevad): maybe watch for ngtcp2 expiry?
  LOG(ERROR) << "unexpected alarm signal";
}

void QuicClient::loop() {
  // not used
  LOG(ERROR) << "unexpected loop signal";
}

void QuicClient::notify() {
  td::actor::send_closure(self_id_, &QuicClient::on_fd_notify);
}

void QuicClient::on_fd_notify() {
  td::sync_with_poll(fd_);
  drain_ingress();
  flush_egress();
}

void QuicClient::flush_egress(EgressData data) {
  if (data.stream_data.has_value()) {
    const auto& stream_data = data.stream_data.value();
    auto cycle = [this, &stream_data]() -> td::Status {
      char buf[DEFAULT_MTU];

      UdpMessageBuffer msg_out;

      msg_out.storage = td::MutableSlice(buf, DEFAULT_MTU);
      TRY_STATUS(p_impl_->write_stream(msg_out, stream_data.sid, stream_data.data, stream_data.fin));

      if (msg_out.storage.empty())
        return td::Status::OK();

      bool sent = false;
      td::UdpSocketFd::OutboundMessage msg{.to = &msg_out.address, .data = td::Slice{msg_out.storage}};
      TRY_STATUS(fd_.send_message(msg, sent));

      if (!sent)
        LOG(WARNING) << "outbound message lost";

      return td::Status::OK();
    };
    if (auto status = cycle(); status.is_error()) {
      LOG(ERROR) << "failed to flush outcoming traffic: " << status;
      return;
    }
  }
  bool run = true;
  auto cycle = [this, &run]() -> td::Status {
    char buf[DEFAULT_MTU];

    UdpMessageBuffer msg_out;

    msg_out.storage = td::MutableSlice(buf, DEFAULT_MTU);
    TRY_STATUS(p_impl_->produce_egress(msg_out));
    run = !msg_out.storage.empty();

    if (!run)
      return td::Status::OK();

    td::UdpSocketFd::OutboundMessage msg{.to = &msg_out.address, .data = td::Slice{msg_out.storage}};
    TRY_STATUS(fd_.send_message(msg, run));

    if (!run)
      LOG(WARNING) << "outbound message lost";

    return td::Status::OK();
  };
  while (run) {
    if (auto status = cycle(); status.is_error()) {
      LOG(ERROR) << "failed to flush outcoming traffic: " << status;
      break;
    }
  }
}

void QuicClient::drain_ingress() {
  bool run = true;
  auto cycle = [this, &run]() -> td::Status {
    char buf[DEFAULT_MTU];

    td::MutableSlice slice(buf, buf + DEFAULT_MTU);
    UdpMessageBuffer msg_in;
    td::UdpSocketFd::InboundMessage msg{.from = &msg_in.address, .data = slice, .error = nullptr};
    TRY_STATUS(fd_.receive_message(msg, run));

    if (!run)
      return td::Status::OK();

    msg_in.storage = msg.data;
    TRY_STATUS(p_impl_->handle_ingress(msg_in));

    return td::Status::OK();
  };
  while (run) {
    if (auto status = cycle(); status.is_error()) {
      LOG(ERROR) << "failed to drain incoming traffic: " << status;
      break;
    }
  }
}

void QuicClient::open_stream(td::Promise<QuicStreamID> P) {
  P.set_result(p_impl_->open_stream());
}

void QuicClient::send_stream_data(QuicStreamID sid, td::Slice data) {
  flush_egress({.stream_data = EgressData::StreamData{.sid = sid, .data = data, .fin = false}});
}

void QuicClient::send_stream_end(QuicStreamID sid) {
  flush_egress({.stream_data = EgressData::StreamData{.sid = sid, .data = {}, .fin = true}});
}

QuicClient::QuicClient(td::UdpSocketFd fd, std::unique_ptr<QuicConnectionPImpl> p_impl,
                       std::unique_ptr<Callback> callback)
    : fd_(std::move(fd)), p_impl_(std::move(p_impl)), callback_(std::move(callback)) {
  class PImplCallback : public QuicConnectionPImpl::Callback {
   public:
    explicit PImplCallback(QuicClient& connection) : connection_(connection) {
    }

    void on_handshake_completed(const HandshakeCompletedEvent& event) override {
      connection_.callback_->on_connected();
    }

    void on_stream_data(const StreamDataEvent& event) override {
      connection_.callback_->on_stream_data(event.data);
      if (event.fin) {
        connection_.callback_->on_stream_end();
      }
    }

   private:
    QuicClient& connection_;
  };
  p_impl_->callback = std::make_unique<PImplCallback>(*this);
}

}  // namespace ton::quic