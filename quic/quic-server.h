#pragma once

#include <map>
#include <memory>

#include "td/actor/ActorOwn.h"
#include "td/actor/core/Actor.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/UdpSocketFd.h"

namespace ton::quic {
struct QuicConnectionPImpl;

class QuicServer : public td::actor::Actor, public td::ObserverBase {
 public:
  class Callback {
   public:
    virtual void on_client_connected(const td::IPAddress& peer) = 0;
    virtual void on_client_data(const td::IPAddress& peer, td::Slice data, bool fin) = 0;
    virtual void on_client_disconnected(const td::IPAddress& peer) = 0;
    virtual ~Callback() = default;
  };

  void send_data(const td::IPAddress& peer, td::Slice data);
  void send_disconnect(const td::IPAddress& peer);

  QuicServer(td::UdpSocketFd fd, td::IPAddress local_address, td::Slice cert_file, td::Slice key_file, td::Slice alpn,
             std::unique_ptr<Callback> callback);

  static td::Result<td::actor::ActorOwn<QuicServer>> open(td::Slice bind_host, int port, td::Slice cert_file,
                                                         td::Slice key_file, std::unique_ptr<Callback> callback,
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
  struct PeerState {
    std::unique_ptr<QuicConnectionPImpl> pimpl;
  };

  void on_fd_notify();
  void process_operation_status(td::Status);

  td::Status ensure_conn(const td::IPAddress& peer, td::Slice datagram);
  PeerState* get_peer_state(const td::IPAddress& peer);
  void drop_peer(const td::IPAddress& peer);

  td::UdpSocketFd fd_;
  td::IPAddress local_address_;

  std::string cert_file_;
  std::string key_file_;
  std::string alpn_;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  std::map<td::IPAddress, PeerState> peers_;
};

}  // namespace ton::quic
