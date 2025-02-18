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

#include "PeerActor.h"
#include "auto/tl/ton_api.hpp"

#include "tl-utils/tl-utils.hpp"

#include "td/utils/overloaded.h"
#include "td/utils/Random.h"
#include "vm/boc.h"
#include "common/delay.h"

namespace ton {

PeerState::State from_ton_api(const ton::ton_api::storage_state &state) {
  PeerState::State res;
  res.want_download = state.want_download_;
  res.will_upload = state.will_upload_;
  return res;
}

ton::ton_api::object_ptr<ton::ton_api::storage_state> to_ton_api(const PeerState::State &state) {
  return ton::ton_api::make_object<ton::ton_api::storage_state>(state.will_upload, state.want_download);
}

PeerActor::PeerActor(td::unique_ptr<Callback> callback, std::shared_ptr<PeerState> state)
    : callback_(std::move(callback)), state_(std::move(state)) {
  CHECK(callback_);
}

template <class T, class... ArgsT>
td::uint64 PeerActor::create_and_send_query(ArgsT &&...args) {
  return send_query(ton::create_serialize_tl_object<T>(std::forward<ArgsT>(args)...));
}

td::uint64 PeerActor::send_query(td::BufferSlice query) {
  auto query_id = next_query_id_++;
  callback_->send_query(query_id, std::move(query));
  return query_id;
}

void PeerActor::schedule_loop() {
  yield();
}

void PeerActor::notify_node() {
  need_notify_node_ = true;
}

void PeerActor::execute_query(td::BufferSlice query, td::Promise<td::BufferSlice> promise) {
  on_pong();
  TRY_RESULT_PROMISE(promise, f, ton::fetch_tl_object<ton::ton_api::Function>(std::move(query), true));
  ton::ton_api::downcast_call(
      *f, td::overloaded(
              [&](ton::ton_api::storage_ping &ping) {
                execute_ping(static_cast<td::uint64>(ping.session_id_), std::move(promise));
              },
              [&](ton::ton_api::storage_addUpdate &add_update) { execute_add_update(add_update, std::move(promise)); },
              [&](ton::ton_api::storage_getPiece &get_piece) { execute_get_piece(get_piece, std::move(promise)); },
              [&](ton::ton_api::storage_getTorrentInfo &) { execute_get_torrent_info(std::move(promise)); },
              [&](auto &other) { promise.set_error(td::Status::Error("Unknown function")); }));
  schedule_loop();
}

void PeerActor::on_ping_result(td::Result<td::BufferSlice> r_answer) {
  ping_query_id_ = {};
  if (r_answer.is_ok()) {
    on_pong();
  }
}

void PeerActor::on_pong() {
  wait_pong_till_ = td::Timestamp::in(10);
  if (!state_->peer_online_.exchange(true)) {
    state_->peer_online_set_ = true;
  }
  notify_node();
}

void PeerActor::on_update_result(td::Result<td::BufferSlice> r_answer) {
  update_query_id_ = {};
  if (r_answer.is_ok()) {
    if (!peer_is_inited_) {
      peer_init_offset_ += UPDATE_INIT_BLOCK_SIZE;
      if (peer_init_offset_ >= have_pieces_.as_slice().size()) {
        peer_is_inited_ = true;
      }
    }
  } else {
    have_pieces_list_.insert(have_pieces_list_.end(), sent_have_pieces_list_.begin(), sent_have_pieces_list_.end());
  }
  sent_have_pieces_list_.clear();
}

void PeerActor::on_get_piece_result(PartId piece_id, td::Result<td::BufferSlice> r_answer) {
  //TODO: handle errors ???
  auto res = [&]() -> td::Result<PeerState::Part> {
    TRY_RESULT(slice, std::move(r_answer));
    TRY_RESULT(piece, ton::fetch_result<ton::ton_api::storage_getPiece>(slice.as_slice()));
    PeerState::Part res;
    res.data = std::move(piece->data_);
    res.proof = std::move(piece->proof_);
    return std::move(res);
  }();
  if (res.is_error()) {
    LOG(DEBUG) << "getPiece " << piece_id << " query: " << res.error();
  } else {
    LOG(DEBUG) << "getPiece " << piece_id << " query: OK";
  }
  state_->node_queries_results_.add_element(std::make_pair(piece_id, std::move(res)));
  notify_node();
}

void PeerActor::on_update_state_result(td::Result<td::BufferSlice> r_answer) {
  if (r_answer.is_error()) {
    update_state_query_.query_id = {};
  }
}

void PeerActor::on_get_info_result(td::Result<td::BufferSlice> r_answer) {
  get_info_query_id_ = {};
  next_get_info_at_ = td::Timestamp::in(5.0);
  alarm_timestamp().relax(next_get_info_at_);
  if (r_answer.is_error()) {
    LOG(DEBUG) << "getTorrentInfo query: " << r_answer.move_as_error();
    return;
  }
  auto R = fetch_tl_object<ton::ton_api::storage_torrentInfo>(r_answer.move_as_ok(), true);
  if (R.is_error()) {
    LOG(DEBUG) << "getTorrentInfo query: " << R.move_as_error();
    return;
  }
  td::BufferSlice data = std::move(R.ok_ref()->data_);
  LOG(DEBUG) << "getTorrentInfo query: got result (" << data.size() << " bytes)";
  if (!data.empty() && !state_->torrent_info_ready_) {
    state_->torrent_info_response_callback_(std::move(data));
  }
}

void PeerActor::on_query_result(td::uint64 query_id, td::Result<td::BufferSlice> r_answer) {
  if (r_answer.is_ok()) {
    on_pong();
  }
  if (ping_query_id_ && ping_query_id_.value() == query_id) {
    on_ping_result(std::move(r_answer));
  } else if (update_query_id_ && update_query_id_.value() == query_id) {
    on_update_result(std::move(r_answer));
  } else if (update_state_query_.query_id && update_state_query_.query_id.value() == query_id) {
    on_update_state_result(std::move(r_answer));
  } else if (get_info_query_id_ && get_info_query_id_.value() == query_id) {
    on_get_info_result(std::move(r_answer));
  } else {
    for (auto &query_it : node_get_piece_) {
      if (query_it.second.query_id && query_it.second.query_id.value() == query_id) {
        on_get_piece_result(query_it.first, std::move(r_answer));
        node_get_piece_.erase(query_it.first);
        break;
      }
    }
  }

  schedule_loop();
}

void PeerActor::start_up() {
  callback_->register_self(actor_id(this));

  node_session_id_ = td::Random::secure_uint64();

  state_->peer = actor_id(this);
  state_->peer_ready_ = true;

  notify_node();
  schedule_loop();
}

void PeerActor::loop() {
  loop_ping();
  loop_pong();

  loop_update_init();
  loop_update_state();
  loop_update_pieces();
  loop_get_torrent_info();

  loop_node_get_piece();
  loop_peer_get_piece();

  loop_notify_node();
}

void PeerActor::loop_pong() {
  if (wait_pong_till_ && wait_pong_till_.is_in_past()) {
    wait_pong_till_ = {};
    LOG(DEBUG) << "Disconnected from peer";
    state_->peer_online_ = false;
    notify_node();
  }
  alarm_timestamp().relax(wait_pong_till_);
}

void PeerActor::loop_ping() {
  if (ping_query_id_) {
    return;
  }
  if (!next_ping_at_.is_in_past()) {
    alarm_timestamp().relax(next_ping_at_);
    return;
  }

  next_ping_at_ = td::Timestamp::in(2);
  alarm_timestamp().relax(next_ping_at_);
  ping_query_id_ = create_and_send_query<ton::ton_api::storage_ping>(node_session_id_);
}

td::BufferSlice PeerActor::create_update_query(ton::tl_object_ptr<ton::ton_api::storage_Update> update) {
  auto session_id = static_cast<td::int64>(peer_session_id_.value());
  auto seqno = static_cast<td::int32>(++node_seqno_);
  return ton::create_serialize_tl_object<ton::ton_api::storage_addUpdate>(session_id, seqno, std::move(update));
}

void PeerActor::loop_update_init() {
  if (!peer_session_id_ || update_query_id_ || peer_is_inited_) {
    return;
  }

  update_have_pieces();

  auto node_state = state_->node_state_.load();
  auto s = have_pieces_.as_slice();
  if (s.size() <= peer_init_offset_) {
    peer_is_inited_ = true;
    return;
  }
  s = s.substr(peer_init_offset_, UPDATE_INIT_BLOCK_SIZE);
  auto query = create_update_query(ton::create_tl_object<ton::ton_api::storage_updateInit>(
      td::BufferSlice(s), (int)peer_init_offset_ * 8, to_ton_api(node_state)));

  // take care about update_state_query initial state
  update_state_query_.state = node_state;
  update_state_query_.query_id = 0;

  LOG(DEBUG) << "Sending updateInit query (" << update_state_query_.state.want_download << ", "
             << update_state_query_.state.will_upload << ", offset=" << peer_init_offset_ * 8 << ")";
  update_query_id_ = send_query(std::move(query));
}

void PeerActor::loop_update_state() {
  if (!peer_is_inited_) {
    return;
  }

  auto node_state = state_->node_state_.load();
  if (!(update_state_query_.state == node_state)) {
    update_state_query_.state = node_state;
    update_state_query_.query_id = {};
  }

  if (update_state_query_.query_id) {
    return;
  }

  auto query = create_update_query(
      ton::create_tl_object<ton::ton_api::storage_updateState>(to_ton_api(update_state_query_.state)));
  LOG(DEBUG) << "Sending updateState query (" << update_state_query_.state.want_download << ", "
             << update_state_query_.state.will_upload << ")";
  update_state_query_.query_id = send_query(std::move(query));
}

void PeerActor::update_have_pieces() {
  auto node_ready_parts = state_->node_ready_parts_.read();
  for (auto piece_id : node_ready_parts) {
    if (piece_id < (peer_init_offset_ + UPDATE_INIT_BLOCK_SIZE) * 8 && !have_pieces_.get(piece_id)) {
      have_pieces_list_.push_back(piece_id);
    }
    have_pieces_.set_one(piece_id);
  }
}

void PeerActor::loop_update_pieces() {
  if (update_query_id_ || !peer_is_inited_) {
    return;
  }

  update_have_pieces();

  if (!have_pieces_list_.empty()) {
    size_t count = std::min<size_t>(have_pieces_list_.size(), 1500);
    sent_have_pieces_list_.assign(have_pieces_list_.end() - count, have_pieces_list_.end());
    have_pieces_list_.erase(have_pieces_list_.end() - count, have_pieces_list_.end());
    auto query = create_update_query(ton::create_tl_object<ton::ton_api::storage_updateHavePieces>(
        td::transform(sent_have_pieces_list_, [](auto x) { return static_cast<td::int32>(x); })));
    LOG(DEBUG) << "Sending updateHavePieces query (" << sent_have_pieces_list_.size() << " pieces)";
    update_query_id_ = send_query(std::move(query));
  }
}

void PeerActor::loop_get_torrent_info() {
  if (state_->torrent_info_ready_) {
    if (!torrent_info_) {
      torrent_info_ = state_->torrent_info_;
      for (const auto &u : pending_update_peer_parts_) {
        process_update_peer_parts(u);
      }
      pending_update_peer_parts_.clear();
    }
    return;
  }
  if (get_info_query_id_) {
    return;
  }
  if (next_get_info_at_ && !next_get_info_at_.is_in_past()) {
    return;
  }
  LOG(DEBUG) << "Sending getTorrentInfo query";
  get_info_query_id_ = create_and_send_query<ton::ton_api::storage_getTorrentInfo>();
}

void PeerActor::loop_node_get_piece() {
  for (auto part : state_->node_queries_.read()) {
    if (state_->speed_limiters_.download.empty()) {
      node_get_piece_.emplace(part, NodePieceQuery{});
    } else {
      if (!torrent_info_) {
        CHECK(state_->torrent_info_ready_);
        loop_get_torrent_info();
      }
      auto piece_size =
          std::min<td::uint64>(torrent_info_->piece_size, torrent_info_->file_size - part * torrent_info_->piece_size);
      td::Timestamp timeout = td::Timestamp::in(3.0);
      td::actor::send_closure(
          state_->speed_limiters_.download, &SpeedLimiter::enqueue, (double)piece_size, timeout,
          [=, SelfId = actor_id(this)](td::Result<td::Unit> R) {
            if (R.is_ok()) {
              td::actor::send_closure(SelfId, &PeerActor::node_get_piece_query_ready, part, std::move(R));
            } else {
              delay_action(
                  [=, R = std::move(R)]() mutable {
                    td::actor::send_closure(SelfId, &PeerActor::node_get_piece_query_ready, part, std::move(R));
                  },
                  timeout);
            }
          });
    }
  }

  for (auto &query_it : node_get_piece_) {
    if (query_it.second.query_id) {
      continue;
    }

    LOG(DEBUG) << "Sending getPiece " << query_it.first << " query";
    query_it.second.query_id =
        create_and_send_query<ton::ton_api::storage_getPiece>(static_cast<td::int32>(query_it.first));
  }
}

void PeerActor::node_get_piece_query_ready(PartId part, td::Result<td::Unit> R) {
  if (R.is_error()) {
    on_get_piece_result(part, R.move_as_error());
  } else {
    node_get_piece_.emplace(part, NodePieceQuery{});
  }
  schedule_loop();
}

void PeerActor::loop_peer_get_piece() {
  // process answers
  for (auto &p : state_->peer_queries_results_.read()) {
    state_->peer_queries_active_.erase(p.first);
    auto promise_it = peer_get_piece_.find(p.first);
    if (promise_it == peer_get_piece_.end()) {
      continue;
    }
    td::Promise<PeerState::Part> promise =
        [i = p.first, promise = std::move(promise_it->second.promise)](td::Result<PeerState::Part> R) mutable {
          LOG(DEBUG) << "Responding to getPiece " << i << ": " << (R.is_ok() ? "OK" : R.error().to_string());
          promise.set_result(R.move_map([](PeerState::Part part) {
            return create_serialize_tl_object<ton_api::storage_piece>(std::move(part.proof), std::move(part.data));
          }));
        };
    if (p.second.is_error()) {
      promise.set_error(p.second.move_as_error());
    } else {
      auto part = p.second.move_as_ok();
      auto size = (double)part.data.size();
      td::Promise<td::Unit> P = promise.wrap([part = std::move(part)](td::Unit) mutable { return std::move(part); });
      if (state_->speed_limiters_.upload.empty()) {
        P.set_result(td::Unit());
      } else {
        td::actor::send_closure(state_->speed_limiters_.upload, &SpeedLimiter::enqueue, size, td::Timestamp::in(3.0),
                                std::move(P));
      }
    }
    peer_get_piece_.erase(promise_it);
    notify_node();
  }

  // create queries
  std::vector<td::uint32> new_peer_queries;
  for (auto &query_it : peer_get_piece_) {
    if (state_->peer_queries_active_.insert(query_it.first).second) {
      new_peer_queries.push_back(query_it.first);
      notify_node();
    }
  }
  state_->peer_queries_.add_elements(std::move(new_peer_queries));
}

void PeerActor::loop_notify_node() {
  if (!need_notify_node_) {
    return;
  }
  need_notify_node_ = false;
  state_->notify_node();
}

void PeerActor::execute_ping(td::uint64 session_id, td::Promise<td::BufferSlice> promise) {
  if (!peer_session_id_ || peer_session_id_.value() != session_id) {
    peer_session_id_ = session_id;
    peer_is_inited_ = false;
    peer_init_offset_ = 0;

    update_query_id_ = {};
    update_state_query_.query_id = {};
  }

  promise.set_value(ton::create_serialize_tl_object<ton::ton_api::storage_pong>());
}

void PeerActor::execute_add_update(ton::ton_api::storage_addUpdate &add_update, td::Promise<td::BufferSlice> promise) {
  auto session_id = static_cast<td::uint64>(add_update.session_id_);
  if (session_id != node_session_id_) {
    promise.set_error(td::Status::Error(404, "INVALID_SESSION"));
    return;
  }
  promise.set_value(ton::create_serialize_tl_object<ton::ton_api::storage_ok>());

  auto seqno = static_cast<td::uint32>(add_update.seqno_);
  auto update_peer_state = [&](PeerState::State peer_state) {
    if (peer_seqno_ >= seqno) {
      return;
    }
    if (state_->peer_state_ready_ && state_->peer_state_.load() == peer_state) {
      return;
    }
    LOG(DEBUG) << "Processing update peer state query (" << peer_state.want_download << ", " << peer_state.will_upload
               << ")";
    peer_seqno_ = seqno;
    state_->peer_state_.exchange(peer_state);
    state_->peer_state_ready_ = true;
    notify_node();
  };
  downcast_call(
      *add_update.update_,
      td::overloaded(
          [&](const ton::ton_api::storage_updateHavePieces &have_pieces) {},
          [&](const ton::ton_api::storage_updateState &state) { update_peer_state(from_ton_api(*state.state_)); },
          [&](const ton::ton_api::storage_updateInit &init) { update_peer_state(from_ton_api(*init.state_)); }));
  if (torrent_info_) {
    process_update_peer_parts(add_update.update_);
  } else {
    pending_update_peer_parts_.push_back(std::move(add_update.update_));
  }
}

void PeerActor::process_update_peer_parts(const tl_object_ptr<ton_api::storage_Update> &update) {
  CHECK(torrent_info_);
  td::uint64 pieces_count = torrent_info_->pieces_count();
  std::vector<td::uint32> new_peer_ready_parts;
  auto add_piece = [&](PartId id) {
    if (id < pieces_count && !peer_have_pieces_.get(id)) {
      peer_have_pieces_.set_one(id);
      new_peer_ready_parts.push_back(id);
      notify_node();
    }
  };
  downcast_call(*update,
                td::overloaded(
                    [&](const ton::ton_api::storage_updateHavePieces &have_pieces) {
                      LOG(DEBUG) << "Processing updateHavePieces query (" << have_pieces.piece_id_ << " pieces)";
                      for (auto id : have_pieces.piece_id_) {
                        add_piece(id);
                      }
                    },
                    [&](const ton::ton_api::storage_updateState &state) {},
                    [&](const ton::ton_api::storage_updateInit &init) {
                      LOG(DEBUG) << "Processing updateInit query (offset=" << init.have_pieces_offset_ << ")";
                      td::Bitset new_bitset;
                      new_bitset.set_raw(init.have_pieces_.as_slice().str());
                      size_t offset = init.have_pieces_offset_;
                      for (auto size = new_bitset.size(), i = (size_t)0; i < size; i++) {
                        if (new_bitset.get(i)) {
                          add_piece(static_cast<PartId>(offset + i));
                        }
                      }
                    }));
  state_->peer_ready_parts_.add_elements(std::move(new_peer_ready_parts));
}

void PeerActor::execute_get_piece(ton::ton_api::storage_getPiece &get_piece, td::Promise<td::BufferSlice> promise) {
  PartId piece_id = get_piece.piece_id_;
  peer_get_piece_[piece_id] = {std::move(promise)};
}

void PeerActor::execute_get_torrent_info(td::Promise<td::BufferSlice> promise) {
  td::BufferSlice result = create_serialize_tl_object<ton_api::storage_torrentInfo>(
      state_->torrent_info_ready_ ? vm::std_boc_serialize(state_->torrent_info_->as_cell()).move_as_ok()
                                  : td::BufferSlice());
  if (state_->torrent_info_ready_) {
    LOG(DEBUG) << "Responding to getTorrentInfo: " << result.size() << " bytes";
  } else {
    LOG(DEBUG) << "Responding to getTorrentInfo: no info";
  }
  promise.set_result(std::move(result));
}
}  // namespace ton
