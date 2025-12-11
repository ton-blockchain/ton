#include "td/actor/actor.h"

#include "quic-connection.h"
#include "quic-pimpl.h"

namespace ton::quic {
td::Result<td::actor::ActorOwn<QuicConnection>> QuicConnection::open(td::CSlice host, int port,
                                                                     std::unique_ptr<Callback> callback,
                                                                     td::CSlice alpn) {
  auto p_impl = std::make_unique<QuicConnectionPImpl>();
  TRY_STATUS(p_impl->remote_address.init_host_port(host, port));
  TRY_RESULT_ASSIGN(p_impl->fd, td::UdpSocketFd::open(p_impl->remote_address));
  TRY_RESULT_ASSIGN(p_impl->local_address, p_impl->fd.get_local_address());

  TRY_STATUS(p_impl->init_tls(host, alpn));
  TRY_STATUS(p_impl->init_quic());

  auto name = PSTRING() << "QUIC[" << p_impl->local_address << ">" << host << ':' << port << ']';
  return td::actor::create_actor<QuicConnection>(name, std::move(p_impl), std::move(callback));
}

void QuicConnection::start_up() {
  td::actor::SchedulerContext::get()->get_poll().subscribe(p_impl_->fd.get_poll_info().extract_pollable_fd(this),
                                                           td::PollFlags::ReadWrite());
  process_operation_status(p_impl_->flush_egress());
}

void QuicConnection::tear_down() {
  // TODO(@avevad): close connection cleanly
}

void QuicConnection::hangup() {
  // not used
  LOG(ERROR) << "unexpected hangup signal";
}

void QuicConnection::hangup_shared() {
  // not used
  LOG(ERROR) << "unexpected hangup_shared signal";
}

void QuicConnection::wake_up() {
  // not used
  LOG(ERROR) << "unexpected wake_up signal";
}
void QuicConnection::alarm() {
  // TODO(@avevad): maybe watch for ngtcp2 expiry?
  LOG(ERROR) << "unexpected alarm signal";
}

void QuicConnection::loop() {
  // not used
  LOG(ERROR) << "unexpected loop signal";
}

void QuicConnection::notify() {
  td::actor::send_closure(actor_id(this), &QuicConnection::on_fd_notify);
}

void QuicConnection::on_fd_notify() {
  process_operation_status(p_impl_->handle_ingress());
  process_operation_status(p_impl_->flush_egress());
}

void QuicConnection::send_data(td::CSlice data) {
  process_operation_status(p_impl_->write_stream(data, false));
  process_operation_status(p_impl_->flush_egress());
}

void QuicConnection::send_disconnect() {
  process_operation_status(p_impl_->write_stream("", true));
  process_operation_status(p_impl_->flush_egress());
}

void QuicConnection::process_operation_status(td::Status status) {
  if (status.is_error()) {
    LOG(ERROR) << status;
    stop();
  }
}

QuicConnection::QuicConnection(std::unique_ptr<QuicConnectionPImpl> p_impl, std::unique_ptr<Callback> callback)
    : p_impl_(std::move(p_impl)), callback_(std::move(callback)) {
  class PImplCallback : public QuicConnectionPImpl::Callback {
   public:
    explicit PImplCallback(QuicConnection& connection) : connection_(connection) {
    }

    void on_handshake_completed(const HandshakeCompletedEvent& event) override {
      connection_.callback_->on_connected();
    }

    void on_stream_data(const StreamDataEvent& event) override {
      connection_.callback_->on_data(event.data);
      if (event.fin) {
        connection_.callback_->on_disconnected();
      }
    }

   private:
    QuicConnection& connection_;
  };
  p_impl_->callback = std::make_unique<PImplCallback>(*this);
}

}  // namespace ton::quic