#pragma once
#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/utils/buffer.h"
#include "td/utils/port/UdpSocketFd.h"

namespace ton::quic {
struct QuicConnectionPImpl;

class QuicConnection : public td::actor::Actor, public td::ObserverBase {
 public:
  class Callback {
   public:
    virtual void on_connected() = 0;
    virtual void on_data(td::Slice data) = 0;
    virtual void on_disconnected() = 0;
    virtual ~Callback() = default;
  };

  void send_data(td::Slice data);
  void send_disconnect();

  QuicConnection(std::unique_ptr<QuicConnectionPImpl> p_impl, std::unique_ptr<Callback> callback);
  static td::Result<td::actor::ActorOwn<QuicConnection>> open(td::Slice host, int port,
                                                              std::unique_ptr<Callback> callback,
                                                              td::Slice alpn = "ton");

 protected:
  void start_up() override;
  void tear_down() override;
  void hangup() override;
  void hangup_shared() override;
  void wake_up() override;
  void alarm() override;
  void loop() override;

  void notify() override;

 private:
  friend QuicConnectionPImpl;

  void on_fd_notify();

  void process_operation_status(td::Status);

  std::unique_ptr<QuicConnectionPImpl> p_impl_;
  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicConnection> self_id_;
};
}  // namespace ton::quic