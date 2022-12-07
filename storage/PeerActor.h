/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/

#pragma once

#include "Bitset.h"
#include "PeerState.h"

#include "td/utils/optional.h"

#include "auto/tl/ton_api.h"

namespace ton {
class PeerActor : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() {
    }
    virtual void register_self(td::actor::ActorId<PeerActor> self) = 0;
    virtual void send_query(td::uint64 query_id, td::BufferSlice query) = 0;
  };

  PeerActor(td::unique_ptr<Callback> callback, std::shared_ptr<PeerState> state);

  void execute_query(td::BufferSlice query, td::Promise<td::BufferSlice> promise);
  void on_query_result(td::uint64 query_id, td::Result<td::BufferSlice> r_answer);

 private:
  td::unique_ptr<Callback> callback_;
  std::shared_ptr<PeerState> state_;
  bool need_notify_node_{false};

  td::uint64 next_query_id_{0};

  // ping
  td::Timestamp next_ping_at_;
  td::optional<td::uint64> ping_query_id_;
  td::optional<td::uint64> get_info_query_id_;
  td::Timestamp wait_pong_till_;
  td::Timestamp next_get_info_at_;

  // startSession
  td::uint64 node_session_id_;
  td::Bitset peer_have_pieces_;

  // update
  td::optional<td::uint64> peer_session_id_;
  td::optional<td::uint64> update_query_id_;
  bool peer_is_inited_{false};
  size_t peer_init_offset_{0};
  td::uint32 node_seqno_{0};
  td::Bitset have_pieces_;
  std::vector<PartId> have_pieces_list_;
  std::vector<PartId> sent_have_pieces_list_;
  td::uint32 peer_seqno_{0};

  // update state
  struct UpdateState {
    td::optional<td::uint64> query_id;
    PeerState::State state{false, false};
  };
  UpdateState update_state_query_;

  // getPiece
  struct NodePieceQuery {
    td::optional<td::uint64> query_id;
  };
  std::map<PartId, NodePieceQuery> node_get_piece_;

  struct PeerPieceQuery {
    td::Promise<td::BufferSlice> promise;
  };
  std::map<PartId, PeerPieceQuery> peer_get_piece_;

  void start_up() override;

  void loop() override;

  void loop_notify_node();

  void loop_pong();
  void execute_ping(td::uint64 session_id, td::Promise<td::BufferSlice> promise);
  void on_ping_result(td::Result<td::BufferSlice> r_answer);
  void on_pong();

  void loop_ping();

  void loop_update_init();
  void loop_update_pieces();
  void update_have_pieces();
  void loop_get_torrent_info();

  void loop_update_state();

  td::BufferSlice create_update_query(ton::tl_object_ptr<ton::ton_api::storage_Update> update);

  void loop_node_get_piece();

  void loop_peer_get_piece();

  void execute_add_update(ton::ton_api::storage_addUpdate &add_update, td::Promise<td::BufferSlice> promise);
  void execute_get_piece(ton::ton_api::storage_getPiece &get_piece, td::Promise<td::BufferSlice> promise);
  void execute_get_torrent_info(td::Promise<td::BufferSlice> promise);

  void on_update_result(td::Result<td::BufferSlice> r_answer);

  void on_get_piece_result(PartId piece_id, td::Result<td::BufferSlice> r_answer);
  void on_update_state_result(td::Result<td::BufferSlice> r_answer);
  void on_get_info_result(td::Result<td::BufferSlice> r_answer);

  template <class T, class... ArgsT>
  td::uint64 create_and_send_query(ArgsT &&... args);
  td::uint64 send_query(td::BufferSlice query);

  void schedule_loop();
  void notify_node();

  static const size_t UPDATE_INIT_BLOCK_SIZE = 6000;
};
}  // namespace ton
