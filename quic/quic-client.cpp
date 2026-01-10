#include "td/actor/actor.h"

#include "quic-client.h"
#include "quic-pimpl.h"

namespace ton::quic {
td::Result<td::actor::ActorOwn<QuicClient>> QuicClient::connect(td::Slice host, int port,
                                                                td::Ed25519::PrivateKey client_key,
                                                                std::unique_ptr<Callback> callback, td::Slice alpn,
                                                                int local_port) {
  td::IPAddress remote_address;
  std::string host_c(host.begin(), host.end());
  TRY_STATUS(remote_address.init_host_port(td::CSlice(host_c.c_str()), port));

  td::IPAddress local_addr;
  TRY_STATUS(local_addr.init_host_port("0.0.0.0", local_port));

  TRY_RESULT(fd, td::UdpSocketFd::open(local_addr));
  TRY_RESULT(actual_local_address, fd.get_local_address());

  TRY_RESULT(p_impl, QuicConnectionPImpl::create_client(actual_local_address, remote_address, client_key, alpn));

  auto name = PSTRING() << "QUIC:" << actual_local_address << ">[" << host << ':' << port << ']';
  return td::actor::create_actor<QuicClient>(td::actor::ActorOptions().with_name(name).with_poll(true), std::move(fd),
                                             std::move(p_impl), std::move(callback));
}

void QuicClient::start_up() {
  LOG(INFO) << "starting up";
  self_id_ = actor_id(this);
  td::actor::SchedulerContext::get().get_poll().subscribe(fd_.get_poll_info().extract_pollable_fd(this),
                                                          td::PollFlags::ReadWrite());
  flush_egress();
  update_alarm();
  LOG(INFO) << "startup completed";
}

void QuicClient::tear_down() {
  td::actor::SchedulerContext::get().get_poll().unsubscribe(fd_.get_poll_info().get_pollable_fd_ref());
  LOG(INFO) << "tear down";
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
  if (!p_impl_) {
    alarm_timestamp() = td::Timestamp::never();
    return;
  }
  if (p_impl_->is_expired()) {
    auto R = p_impl_->handle_expiry();
    if (R.is_error()) {
      LOG(WARNING) << "failed to handle QUIC expiry: " << R.error();
      stop();
      return;
    }

    switch (R.ok()) {
      case QuicConnectionPImpl::ExpiryAction::None:
        break;
      case QuicConnectionPImpl::ExpiryAction::ScheduleWrite:
        flush_egress();
        break;
      case QuicConnectionPImpl::ExpiryAction::IdleClose:
        stop();
        return;
      case QuicConnectionPImpl::ExpiryAction::Close:
        flush_egress();
        stop();
        return;
    }
  }
  update_alarm();
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
  update_alarm();
}

void QuicClient::flush_egress(EgressData data) {
  if (data.stream_data.has_value()) {
    auto& stream_data = data.stream_data.value();
    auto cycle = [this, &stream_data]() -> td::Status {
      char buf[DEFAULT_MTU];

      UdpMessageBuffer msg_out;

      msg_out.storage = td::MutableSlice(buf, DEFAULT_MTU);
      TRY_STATUS(p_impl_->write_stream(msg_out, stream_data.sid, std::move(stream_data.data), stream_data.fin));

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

void QuicClient::send_stream_data(QuicStreamID sid, td::BufferSlice data) {
  flush_egress({.stream_data = EgressData::StreamData{.sid = sid, .data = std::move(data), .fin = false}});
  update_alarm();
}

void QuicClient::send_stream_end(QuicStreamID sid) {
  flush_egress({.stream_data = EgressData::StreamData{.sid = sid, .data = {}, .fin = true}});
  update_alarm();
}

void QuicClient::update_alarm() {
  if (!p_impl_) {
    alarm_timestamp() = td::Timestamp::never();
    return;
  }
  alarm_timestamp() = p_impl_->get_expiry_timestamp();
}

QuicClient::QuicClient(td::UdpSocketFd fd, std::unique_ptr<QuicConnectionPImpl> p_impl,
                       std::unique_ptr<Callback> callback)
    : fd_(std::move(fd)), p_impl_(std::move(p_impl)), callback_(std::move(callback)) {
  class PImplCallback : public QuicConnectionPImpl::Callback {
   public:
    explicit PImplCallback(QuicClient& connection) : connection_(connection) {
    }

    td::Status on_handshake_completed(HandshakeCompletedEvent event) override {
      return connection_.callback_->on_connected(std::move(event.peer_public_key));
    }

    void on_stream_data(StreamDataEvent event) override {
      connection_.callback_->on_stream_data(event.sid, std::move(event.data));
      if (event.fin) {
        connection_.callback_->on_stream_end(event.sid);
      }
    }

   private:
    QuicClient& connection_;
  };
  p_impl_->callback = std::make_unique<PImplCallback>(*this);
}

}  // namespace ton::quic
