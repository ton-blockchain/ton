#pragma once

#include <map>
#include <memory>
#include <optional>
#include <variant>

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

struct StreamOptions {
  std::optional<td::uint64> max_size;
  td::Timestamp timeout = td::Timestamp::never();
  double timeout_seconds = 0.0;
  td::uint64 query_size = 0;
  td::uint32 query_magic = 0;
};

class QuicServer : public td::actor::Actor, public td::ObserverBase {
 public:
  class Callback {
   public:
    virtual void on_connected(QuicConnectionId cid, td::SecureString peer_public_key, bool is_outbound) = 0;
    virtual td::Status on_stream(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data, bool is_end) = 0;
    virtual void on_closed(QuicConnectionId cid) = 0;
    virtual void on_stream_closed(QuicConnectionId cid, QuicStreamID sid) = 0;
    virtual void set_stream_options(QuicConnectionId cid, QuicStreamID sid, StreamOptions options) {
    }
    virtual ~Callback() = default;
  };

  void send_stream_data(QuicConnectionId cid, QuicStreamID sid, td::BufferSlice data);
  void send_stream_end(QuicConnectionId cid, QuicStreamID sid);
  td::Result<QuicStreamID> open_stream(QuicConnectionId cid, StreamOptions options = {});

  td::Result<QuicStreamID> send_stream(QuicConnectionId cid, std::variant<QuicStreamID, StreamOptions> stream,
                                       td::BufferSlice data, bool is_end);

  td::Result<QuicConnectionId> connect(td::Slice host, int port, td::Ed25519::PrivateKey client_key, td::Slice alpn);

  void close(QuicConnectionId cid);

  QuicServer(td::UdpSocketFd fd, td::Ed25519::PrivateKey server_key, td::BufferSlice alpn,
             std::unique_ptr<Callback> callback);

  static td::Result<td::actor::ActorOwn<QuicServer>> create(int port, td::Ed25519::PrivateKey server_key,
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
    QuicConnectionPImpl &impl() {
      CHECK(impl_);
      return *impl_;
    }
    std::unique_ptr<QuicConnectionPImpl> impl_;
    td::IPAddress remote_address;
    QuicConnectionId cid;
    std::optional<QuicConnectionId> temp_cid;
    std::optional<std::pair<td::IPAddress, td::BufferSlice>> blocked_packet;
    bool is_outbound;
    friend td::StringBuilder &operator<<(td::StringBuilder &sb, const ConnectionState &state) {
      sb << "Connection{" << (state.is_outbound ? "to" : "from") << " " << state.remote_address;
      sb << " cid=" << state.cid;
      if (state.temp_cid) {
        sb << " (temp=" << state.temp_cid.value() << ")";
      }
      sb << "}";
      return sb;
    }
  };

  void update_alarm();
  void update_alarm_for(ConnectionState &state);
  void drain_ingress();
  void flush_egress_for(ConnectionState &state, EgressData data = EgressData{});
  void flush_egress_all();

  std::shared_ptr<ConnectionState> find_connection(const QuicConnectionId &cid);
  td::Result<std::shared_ptr<ConnectionState>> get_or_create_connection(const UdpMessageBuffer &msg_in);
  bool try_close(ConnectionState &state);

  td::UdpSocketFd fd_;
  td::BufferSlice alpn_;
  td::Ed25519::PrivateKey server_key_;

  std::unique_ptr<Callback> callback_;
  td::actor::ActorId<QuicServer> self_id_;

  std::map<QuicConnectionId, QuicConnectionId> to_primary_cid_;
  std::map<QuicConnectionId, std::shared_ptr<ConnectionState>> connections_;
};

}  // namespace ton::quic
