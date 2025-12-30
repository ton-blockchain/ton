#pragma once
#include <optional>

#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/utils/buffer.h"
#include "td/utils/port/UdpSocketFd.h"

#include "quic-common.h"

namespace ton::quic {
struct QuicConnectionPImpl;

class QuicClient : public td::actor::Actor, public td::ObserverBase {
 public:
  class Callback {
   public:
    virtual void on_connected() = 0;
    virtual void on_stream_data(QuicStreamID sid, td::BufferSlice data) = 0;
    virtual void on_stream_end(QuicStreamID sid) = 0;
    virtual ~Callback() = default;
  };

  void open_stream(td::Promise<QuicStreamID> P);
  void send_stream_data(QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(QuicStreamID sid);

  QuicClient(td::UdpSocketFd fd, std::unique_ptr<QuicConnectionPImpl> p_impl, std::unique_ptr<Callback> callback);
  static td::Result<td::actor::ActorOwn<QuicClient>> connect(td::Slice host, int port,
                                                             std::unique_ptr<Callback> callback, td::Slice alpn = "ton",
                                                             int local_port = 0);

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

  constexpr static size_t DEFAULT_MTU = 1350;

  struct EgressData {
    struct StreamData {
      QuicStreamID sid;
      td::BufferSlice data;
      bool fin;
    };
    std::optional<StreamData> stream_data;
  };

  void on_fd_notify();
  void flush_egress(EgressData data = {.stream_data = std::nullopt});
  void drain_ingress();

  td::UdpSocketFd fd_;
  std::unique_ptr<QuicConnectionPImpl> p_impl_;
  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicClient> self_id_;
};
}  // namespace ton::quic