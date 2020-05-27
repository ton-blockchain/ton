#include "PeerActor.h"
#include "auto/tl/ton_api.hpp"

#include "tl-utils/tl-utils.hpp"

#include "td/utils/overloaded.h"
#include "td/utils/Random.h"

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

PeerActor::PeerActor(td::unique_ptr<Callback> callback, td::SharedState<PeerState> state)
    : callback_(std::move(callback)), state_(std::move(state)) {
  CHECK(callback_);
}

template <class T, class... ArgsT>
td::uint64 PeerActor::create_and_send_query(ArgsT &&... args) {
  return send_query(ton::create_serialize_tl_object<T>(std::forward<ArgsT>(args)...));
}

td::uint64 PeerActor::send_query(td::BufferSlice query) {
  auto query_id = next_query_id_++;
  //LOG(ERROR) << "send_query " << to_string(ton::fetch_tl_object<ton::ton_api::Function>(std::move(query), true).ok());
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
  TRY_RESULT_PROMISE(promise, f, ton::fetch_tl_object<ton::ton_api::Function>(std::move(query), true));
  //LOG(ERROR) << "execute_query " << to_string(f);
  ton::ton_api::downcast_call(
      *f, td::overloaded(
              [&](ton::ton_api::storage_ping &ping) {
                execute_ping(static_cast<td::uint64>(ping.session_id_), std::move(promise));
              },
              [&](ton::ton_api::storage_addUpdate &add_update) { execute_add_update(add_update, std::move(promise)); },
              [&](ton::ton_api::storage_getPiece &get_piece) { execute_get_piece(get_piece, std::move(promise)); },
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
  wait_pong_till_ = td::Timestamp::in(4);
  state_.lock()->peer_online_ = true;
  notify_node();
}

void PeerActor::on_update_result(td::Result<td::BufferSlice> r_answer) {
  update_query_id_ = {};
  if (r_answer.is_ok()) {
    peer_is_inited_ = true;
    have_pieces_list_.clear();
  }
}

void PeerActor::on_get_piece_result(PartId piece_id, td::Result<td::BufferSlice> r_answer) {
  auto state = state_.lock();
  auto it = state->node_queries_.find(piece_id);
  if (it == state->node_queries_.end()) {
    LOG(ERROR) << "???";
    return;
  }
  //TODO: handle errors ???
  it->second = [&]() -> td::Result<PeerState::Part> {
    TRY_RESULT(slice, std::move(r_answer));
    TRY_RESULT(piece, ton::fetch_result<ton::ton_api::storage_getPiece>(slice.as_slice()));
    PeerState::Part res;
    res.data = std::move(piece->data_);
    res.proof = std::move(piece->proof_);
    return std::move(res);
  }();
  notify_node();
}

void PeerActor::on_update_state_result(td::Result<td::BufferSlice> r_answer) {
  if (r_answer.is_error()) {
    update_state_query_.query_id = {};
  }
}

void PeerActor::on_query_result(td::uint64 query_id, td::Result<td::BufferSlice> r_answer) {
  if (r_answer.is_ok()) {
    on_pong();
    state_.lock()->download.add(r_answer.ok().size(), td::Timestamp::now());
  }
  if (ping_query_id_ && ping_query_id_.value() == query_id) {
    on_ping_result(std::move(r_answer));
  } else if (update_query_id_ && update_query_id_.value() == query_id) {
    on_update_result(std::move(r_answer));
  } else if (update_state_query_.query_id && update_state_query_.query_id.value() == query_id) {
    on_update_state_result(std::move(r_answer));
  } else {
    for (auto &query_it : node_get_piece_) {
      if (query_it.second.query_id && query_it.second.query_id.value() == query_id) {
        on_get_piece_result(query_it.first, std::move(r_answer));
        query_it.second.query_id = {};
      }
    }
  }

  schedule_loop();
}

void PeerActor::start_up() {
  callback_->register_self(actor_id(this));

  node_session_id_ = td::Random::secure_uint64();

  auto state = state_.lock();
  state->peer = actor_id(this);

  notify_node();
  schedule_loop();
}

void PeerActor::loop() {
  loop_ping();
  loop_pong();

  loop_update_init();
  loop_update_state();
  loop_update_pieces();

  loop_node_get_piece();
  loop_peer_get_piece();

  loop_notify_node();
}

void PeerActor::loop_pong() {
  if (wait_pong_till_ && wait_pong_till_.is_in_past()) {
    wait_pong_till_ = {};
    LOG(INFO) << "Disconnected";
    state_.lock()->peer_online_ = false;
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
  if (!peer_session_id_) {
    return;
  }
  if (update_query_id_) {
    return;
  }
  if (peer_is_inited_) {
    return;
  }

  update_have_pieces();
  have_pieces_list_.clear();

  auto state = state_.lock();
  auto query = create_update_query(ton::create_tl_object<ton::ton_api::storage_updateInit>(
      td::BufferSlice(have_pieces_.as_slice()), to_ton_api(state->node_state_)));

  // take care about update_state_query initial state
  update_state_query_.state = state->node_state_;
  update_state_query_.query_id = 0;

  update_query_id_ = send_query(std::move(query));
}

void PeerActor::loop_update_state() {
  if (!peer_is_inited_) {
    return;
  }

  auto state = state_.lock();
  if (!(update_state_query_.state == state->node_state_)) {
    update_state_query_.state = state->node_state_;
    update_state_query_.query_id = {};
  }

  if (update_state_query_.query_id) {
    return;
  }

  auto query = create_update_query(
      ton::create_tl_object<ton::ton_api::storage_updateState>(to_ton_api(update_state_query_.state)));
  update_state_query_.query_id = send_query(std::move(query));
}

void PeerActor::update_have_pieces() {
  auto state = state_.lock();
  have_pieces_list_.insert(have_pieces_list_.end(), state->node_ready_parts_.begin(), state->node_ready_parts_.end());
  for (auto piece_id : state->node_ready_parts_) {
    have_pieces_.set_one(piece_id);
  }
  state->node_ready_parts_.clear();
}

void PeerActor::loop_update_pieces() {
  if (update_query_id_) {
    return;
  }

  if (!peer_is_inited_) {
    return;
  }

  update_have_pieces();

  if (!have_pieces_list_.empty()) {
    auto query = create_update_query(ton::create_tl_object<ton::ton_api::storage_updateHavePieces>(
        td::transform(have_pieces_list_, [](auto x) { return static_cast<td::int32>(x); })));
    update_query_id_ = send_query(std::move(query));
  }
}

void PeerActor::loop_node_get_piece() {
  auto state = state_.lock();

  for (auto it = node_get_piece_.begin(); it != node_get_piece_.end();) {
    auto other_it = state->node_queries_.find(it->first);
    if (other_it == state->node_queries_.end() || other_it->second) {
      it = node_get_piece_.erase(it);
    } else {
      it++;
    }
  }

  for (auto &query_it : state->node_queries_) {
    if (query_it.second) {
      continue;
    }
    node_get_piece_.emplace(query_it.first, NodePieceQuery{});
  }

  for (auto &query_it : node_get_piece_) {
    if (query_it.second.query_id) {
      continue;
    }

    query_it.second.query_id =
        create_and_send_query<ton::ton_api::storage_getPiece>(static_cast<td::int32>(query_it.first));
  }
}

void PeerActor::loop_peer_get_piece() {
  auto state = state_.lock();

  // process answers
  for (auto &it : state->peer_queries_) {
    if (!it.second) {
      continue;
    }
    auto promise_it = peer_get_piece_.find(it.first);
    if (promise_it == peer_get_piece_.end()) {
      continue;
    }
    promise_it->second.promise.set_result(it.second.unwrap().move_map([](PeerState::Part part) {
      return ton::create_serialize_tl_object<ton::ton_api::storage_piece>(std::move(part.proof), std::move(part.data));
    }));
    peer_get_piece_.erase(promise_it);
  }

  // erase unneeded queries
  for (auto it = state->peer_queries_.begin(); it != state->peer_queries_.end();) {
    if (peer_get_piece_.count(it->first) == 0) {
      it = state->peer_queries_.erase(it);
      notify_node();
    } else {
      it++;
    }
  }

  // create queries
  for (auto &query_it : peer_get_piece_) {
    auto res = state->peer_queries_.emplace(query_it.first, td::optional<td::Result<PeerState::Part>>());
    if (res.second) {
      notify_node();
    }
  }
}

void PeerActor::loop_notify_node() {
  if (!need_notify_node_) {
    return;
  }
  need_notify_node_ = false;
  state_.lock()->notify_node();
}

void PeerActor::execute_ping(td::uint64 session_id, td::Promise<td::BufferSlice> promise) {
  if (!peer_session_id_ || peer_session_id_.value() != session_id) {
    peer_session_id_ = session_id;
    peer_is_inited_ = false;

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

  auto state = state_.lock();
  auto add_piece = [&](PartId id) {
    if (!peer_have_pieces_.get(id)) {
      peer_have_pieces_.set_one(id);
      state->peer_ready_parts_.push_back(id);
      notify_node();
    }
  };

  auto seqno = static_cast<td::uint32>(add_update.seqno_);
  auto update_peer_state = [&](PeerState::State peer_state) {
    if (peer_seqno_ >= seqno) {
      return;
    }
    if (state->peer_state_ && state->peer_state_.value() == peer_state) {
      return;
    }
    peer_seqno_ = seqno;
    state->peer_state_ = peer_state;
    notify_node();
  };

  //LOG(ERROR) << "Got " << to_string(add_update);
  downcast_call(*add_update.update_,
                td::overloaded(
                    [&](ton::ton_api::storage_updateHavePieces &have_pieces) {
                      for (auto id : have_pieces.piece_id_) {
                        add_piece(id);
                      }
                    },
                    [&](ton::ton_api::storage_updateState &state) { update_peer_state(from_ton_api(*state.state_)); },
                    [&](ton::ton_api::storage_updateInit &init) {
                      update_peer_state(from_ton_api(*init.state_));
                      td::Bitset new_bitset;
                      new_bitset.set_raw(init.have_pieces_.as_slice().str());
                      for (auto size = new_bitset.size(), i = size_t(0); i < size; i++) {
                        if (new_bitset.get(i)) {
                          add_piece(static_cast<PartId>(i));
                        }
                      }
                    }));
}

void PeerActor::execute_get_piece(ton::ton_api::storage_getPiece &get_piece, td::Promise<td::BufferSlice> promise) {
  PartId piece_id = get_piece.piece_id_;
  peer_get_piece_[piece_id] = {std::move(promise)};
}
}  // namespace ton
