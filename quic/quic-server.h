#pragma once

#include <map>
#include <memory>
#include <optional>

#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/actor/coro_task.h"
#include "td/utils/buffer.h"
#include "td/utils/crypto.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/UdpSocketFd.h"

#include "Ed25519.h"
#include "openssl-utils.h"
#include "quic-common.h"

namespace ton::quic {
struct QuicConnectionPImpl;

class QuicServer : public td::actor::Actor, public td::ObserverBase {
 public:
  class Callback {
   public:
    virtual td::Status on_connected(QuicConnectionId cid, td::SecureString peer_public_key) = 0;
    virtual void on_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data) = 0;
    virtual void on_stream_end(QuicConnectionId cid, QuicStreamID sid) = 0;
    virtual ~Callback() = default;
  };

  void send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(QuicConnectionId cid, QuicStreamID sid);
  td::Result<QuicStreamID> open_stream(QuicConnectionId cid);
  td::Result<QuicConnectionId> connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn);

  QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback);

  static td::Result<td::actor::ActorOwn<QuicServer>> listen(int port, td::Ed25519::PrivateKey server_key,
                                                            std::unique_ptr<Callback> callback, td::Slice alpn = "ton",
                                                            td::Slice bind_host = "0.0.0.0");

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
  class PImplCallback;

  constexpr static size_t DEFAULT_MTU = 1350;

  struct EgressData {
    struct StreamData {
      QuicStreamID sid;
      td::BufferSlice data;
      bool fin;
    };
    std::optional<StreamData> stream_data;
  };

  struct ConnectionState {
    std::unique_ptr<QuicConnectionPImpl> p_impl;
    td::IPAddress remote_address;
    bool is_outbound;
  };

  void on_fd_notify();
  void update_alarm();
  void drain_ingress();
  void flush_egress_for(QuicConnectionId cid, ConnectionState &state, EgressData data = {.stream_data = std::nullopt});
  void flush_egress_all();

  ConnectionState *find_connection(const QuicConnectionId &cid);
  td::Result<std::map<QuicConnectionId, ConnectionState>::iterator> get_or_create_connection(
      const UdpMessageBuffer &msg_in);
  void remove_connection(const QuicConnectionId &cid);

  td::UdpSocketFd fd_;
  td::BufferSlice alpn_;
  td::Ed25519::PrivateKey server_key_;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  std::map<QuicConnectionId, ConnectionState> connections_;
};

}  // namespace ton::quic
