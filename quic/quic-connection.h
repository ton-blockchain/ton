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
    virtual void on_connected();
    virtual void on_data(td::CSlice data) = 0;
    virtual void on_disconnected();
    virtual ~Callback();
  };

  void send_data(td::CSlice data);
  void send_disconnect();

  QuicConnection(std::unique_ptr<QuicConnectionPImpl> p_impl, std::unique_ptr<Callback> callback);
  static td::Result<td::actor::ActorOwn<QuicConnection>> open(td::CSlice host, int port,
                                                              std::unique_ptr<Callback> callback,
                                                              td::CSlice alpn = "ton");

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
};
}  // namespace ton::quic