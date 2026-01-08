#pragma once

#include <map>
#include <memory>
#include <optional>

#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
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
    // peer_public_key is the client's Ed25519 public key (32 bytes) when using RPK, empty otherwise
    virtual void on_connected(const td::IPAddress &peer, td::SecureString peer_public_key) = 0;
    virtual void on_stream_data(const td::IPAddress &peer, QuicStreamID sid, td::BufferSlice data) = 0;
    virtual void on_stream_end(const td::IPAddress &peer, QuicStreamID sid) = 0;
    virtual ~Callback() = default;
  };

  void send_stream_data(const td::IPAddress &peer, QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(const td::IPAddress &peer, QuicStreamID sid);

  QuicServer(td::UdpSocketFd fd, td::BufferSlice cert_file, td::BufferSlice key_file, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback);
  // RPK constructor
  QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback);

  static td::Result<td::actor::ActorOwn<QuicServer>> listen(int port, td::Slice cert_file, td::Slice key_file,
                                                            std::unique_ptr<Callback> callback, td::Slice alpn = "ton",
                                                            td::Slice bind_host = "0.0.0.0");
  // RPK variant - uses Raw Public Key instead of certificate
  static td::Result<td::actor::ActorOwn<QuicServer>> listen_rpk(int port, td::Ed25519::PrivateKey server_key,
                                                                std::unique_ptr<Callback> callback,
                                                                td::Slice alpn = "ton",
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
  };

  void on_fd_notify();
  void update_alarm();
  void drain_ingress();
  void flush_egress_for(const td::IPAddress &peer, ConnectionState &state,
                        EgressData data = {.stream_data = std::nullopt});
  void flush_egress_all();

  td::Result<ConnectionState *> get_or_create_connection(const UdpMessageBuffer &msg_in);

  td::UdpSocketFd fd_;
  td::BufferSlice cert_file_;
  td::BufferSlice key_file_;
  td::BufferSlice alpn_;
  std::optional<td::Ed25519::PrivateKey> server_key_;  // For RPK mode
  bool use_rpk_ = false;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  std::map<td::IPAddress, ConnectionState> connections_;
};

}  // namespace ton::quic
