#pragma once

#include "adnl/adnl.h"
#include "td/actor/coro_task.h"

#include "quic-client.h"
#include "quic-server.h"

namespace ton::quic {
class QuicSender : public adnl::AdnlSenderInterface {
  using AdnlPath = std::pair<adnl::AdnlNodeIdShort, adnl::AdnlNodeIdShort>;
 public:
  explicit QuicSender(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::Slice cert_file, td::Slice key_file);

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override;
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override;

  td::actor::Task<> add_local_id(adnl::AdnlNodeIdShort local_id);

 private:
  struct OutboundConnection {
    bool ready_received = false;
    td::actor::ActorOwn<QuicClient> client{};
    td::Promise<OutboundConnection*> ready = td::make_promise([](td::Result<OutboundConnection*>) {});
    std::unordered_map<QuicStreamID, td::Promise<td::BufferSlice>> responses{};
  };

  constexpr static int NODE_PORT_OFFSET = 1000;

  void find_out_connection(AdnlPath path, td::Promise<OutboundConnection*> P);
  void create_connection(AdnlPath path, td::Promise<OutboundConnection*> P);

  void after_out_connection_created(AdnlPath path);
  void after_out_connection_ready(AdnlPath path);
  void after_out_query_answer(AdnlPath path, QuicStreamID sid, td::BufferSlice data);

  void after_in_query(AdnlPath path, td::IPAddress peer, QuicStreamID sid, td::BufferSlice data);
  void after_in_query_answer(adnl::AdnlNodeIdShort local_id, td::IPAddress peer, QuicStreamID sid,td::BufferSlice data);
  void after_in_message(AdnlPath path, td::BufferSlice data);

  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::BufferSlice cert_file_, key_file_;

  std::map<AdnlPath, OutboundConnection> outbound_;
  std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<QuicServer>> inbound_;
};
}  // namespace ton::quic
