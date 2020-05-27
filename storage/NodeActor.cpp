#include "NodeActor.h"

#include "vm/boc.h"

#include "td/utils/Enumerator.h"
#include "td/utils/tests.h"

namespace ton {
NodeActor::NodeActor(PeerId self_id, ton::Torrent torrent, td::unique_ptr<Callback> callback, bool should_download)
    : self_id_(self_id)
    , torrent_(std::move(torrent))
    , callback_(std::move(callback))
    , should_download_(should_download)
    , parts_helper_(torrent.get_info().pieces_count()) {
}

void NodeActor::start_peer(PeerId peer_id, td::Promise<td::actor::ActorId<PeerActor>> promise) {
  peers_[peer_id];
  loop();
  auto it = peers_.find(peer_id);
  if (it == peers_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error("Won't start peer now"));
    return;
  }
  promise.set_value(it->second.actor.get());
}

void NodeActor::on_signal_from_peer(PeerId peer_id) {
  loop_peer(peer_id, peers_[peer_id]);
}

void NodeActor::start_up() {
  callback_->register_self(actor_id(this));
  auto pieces_count = torrent_.get_info().pieces_count();
  parts_.parts.resize(pieces_count);

  auto header = torrent_.get_header_parts_range();
  for (td::uint32 i = static_cast<td::uint32>(header.begin); i < header.end; i++) {
    parts_helper_.set_part_priority(i, 255);
  }
  for (td::uint32 i = 0; i < pieces_count; i++) {
    if (torrent_.is_piece_ready(i)) {
      parts_helper_.on_self_part_ready(i);
      parts_.parts[i].ready = true;
    }
  }
  loop();
}

void NodeActor::loop_will_upload() {
  if (peers_.empty()) {
    return;
  }

  if (!will_upload_at_.is_in_past()) {
    alarm_timestamp().relax(will_upload_at_);
    return;
  }

  will_upload_at_ = td::Timestamp::in(5);
  alarm_timestamp().relax(will_upload_at_);
  std::vector<std::tuple<bool, bool, double, PeerId>> peers;
  for (auto &it : peers_) {
    auto state = it.second.state.lock();
    bool needed = false;
    if (state->peer_state_) {
      needed = state->peer_state_.value().want_download;
    }
    peers.emplace_back(!needed, !state->node_state_.want_download, -state->download.speed(), it.first);
  }
  std::sort(peers.begin(), peers.end());

  if (peers.size() > 5) {
    std::swap(peers[4], peers[td::Random::fast(5, (int)peers.size() - 1)]);
    peers.resize(5);
  }

  std::set<PeerId> peers_set;
  for (auto id : peers) {
    peers_set.insert(std::get<PeerId>(id));
  }

  for (auto &it : peers_) {
    auto will_upload = peers_set.count(it.first) > 0;
    auto state = it.second.state.lock();
    if (state->node_state_.will_upload != will_upload) {
      state->node_state_.will_upload = will_upload;
      state->notify_peer();
    }
  }
}

void NodeActor::loop() {
  loop_get_peers();
  loop_start_stop_peers();
  loop_queries();
  loop_will_upload();

  if (!ready_parts_.empty()) {
    for (auto &it : peers_) {
      auto state = it.second.state.lock();
      state->node_ready_parts_.insert(state->node_ready_parts_.end(), ready_parts_.begin(), ready_parts_.end());
      state->notify_peer();
    }
    ready_parts_.clear();
  }

  if (torrent_.is_completed() && !is_completed_) {
    is_completed_ = true;
    callback_->on_completed();
  }
}

std::string NodeActor::get_stats_str() {
  td::StringBuilder sb;
  sb << "Node " << self_id_ << " " << torrent_.get_ready_parts_count() << "\t" << download_;
  sb << "\toutq " << parts_.total_queries;
  sb << "\n";
  for (auto &it : peers_) {
    auto state = it.second.state.lock();
    sb << "\tPeer " << it.first;
    sb << "\t" << parts_helper_.get_ready_parts(it.second.peer_token).ones_count();
    sb << "\t" << state->download;
    if (state->peer_state_) {
      auto &peer_state = state->peer_state_.value();
      sb << "\t  up:" << peer_state.will_upload;
      sb << "\tdown:" << peer_state.want_download;
      sb << "\tcnt:" << parts_helper_.get_want_download_count(it.second.peer_token);
    }
    sb << "\toutq:" << state->node_queries_.size();
    sb << "\tinq:" << state->peer_queries_.size();
    auto &node_state = state->node_state_;
    sb << "\tNup:" << node_state.will_upload;
    sb << "\tNdown:" << node_state.want_download;
    sb << "\n";
  }

  auto o_n = torrent_.get_files_count();
  if (o_n) {
    // by default all parts priority == 1
    auto n = o_n.unwrap();
    file_priority_.resize(n, 1);
    for (size_t i = 0; i < n; i++) {
      auto size = torrent_.get_file_size(i);
      auto ready_size = torrent_.get_file_ready_size(i);
      sb << "#" << i << " " << torrent_.get_file_name(i) << "\t" << 100 * ready_size / size << "%%  "
         << td::format::as_size(ready_size) << "/" << td::format::as_size(size) << "\t priority=" << file_priority_[i]
         << "\n";
    }
  }
  return sb.as_cslice().str();
}

void NodeActor::set_file_priority(size_t i, td::uint8 priority) {
  auto o_files_count = torrent_.get_files_count();
  if (!o_files_count) {
    return;
  }
  auto files_count = o_files_count.unwrap();
  if (file_priority_.size() != files_count) {
    // by default all parts priority == 1
    file_priority_.resize(files_count, 1);
  }

  if (i >= files_count) {
    for (td::uint32 part_i = 0; part_i < torrent_.get_info().pieces_count(); part_i++) {
      parts_helper_.set_part_priority(part_i, priority);
    }
    for (auto &p : file_priority_) {
      p = priority;
    }
    return;
  }
  if (file_priority_[i] == priority) {
    return;
  }
  file_priority_[i] = priority;
  auto range = torrent_.get_file_parts_range(i);
  td::uint32 begin = static_cast<td::uint32>(range.begin);
  td::uint32 end = static_cast<td::uint32>(range.end);
  for (td::uint32 i = begin; i < end; i++) {
    if (i == begin || i + 1 == end) {
      auto chunks = torrent_.chunks_by_piece(i);
      td::uint8 max_priority = 0;
      for (auto chunk_id : chunks) {
        if (chunk_id == 0) {
          max_priority = 255;
        } else {
          max_priority = td::max(max_priority, file_priority_[chunk_id - 1]);
        }
      }
      parts_helper_.set_part_priority(i, max_priority);
    } else {
      parts_helper_.set_part_priority(i, priority);
    }
  }
  yield();
}

void NodeActor::set_should_download(bool should_download) {
  should_download_ = should_download;
  yield();
}

void NodeActor::tear_down() {
  callback_->on_closed(std::move(torrent_));
}

void NodeActor::loop_start_stop_peers() {
  for (auto &it : peers_) {
    auto &peer = it.second;
    auto peer_id = it.first;

    if (peer.notifier.empty()) {
      peer.notifier = td::actor::create_actor<Notifier>("Notifier", actor_id(this), peer_id);
    }

    if (peer.actor.empty()) {
      LOG(ERROR) << "Init Peer " << self_id_ << " -> " << peer_id;
      auto state = peer.state.lock();
      state->node = peer.notifier.get();
      for (td::uint32 i = 0; i < parts_.parts.size(); i++) {
        if (parts_.parts[i].ready) {
          state->node_ready_parts_.push_back(i);
        }
      }
      peer.peer_token = parts_helper_.register_peer(peer_id);
      peer.actor = callback_->create_peer(self_id_, peer_id, peer.state);
    }
  }
}

void NodeActor::loop_queries() {
  if (!should_download_) {
    return;
  }
  for (auto &it : peers_) {
    auto peer_token = it.second.peer_token;
    auto state = it.second.state.lock();
    if (!state->peer_state_) {
      parts_helper_.set_peer_limit(peer_token, 0);
      continue;
    }
    if (!state->peer_state_.value().will_upload) {
      parts_helper_.set_peer_limit(peer_token, 0);
      continue;
    }
    parts_helper_.set_peer_limit(peer_token,
                                 td::narrow_cast<td::uint32>(MAX_PEER_TOTAL_QUERIES - state->node_queries_.size()));
  }

  auto parts = parts_helper_.get_rarest_parts(MAX_TOTAL_QUERIES);
  for (auto &part : parts) {
    auto it = peers_.find(part.peer_id);
    CHECK(it != peers_.end());
    auto state = it->second.state.lock();
    CHECK(state->peer_state_);
    CHECK(state->peer_state_.value().will_upload);
    CHECK(state->node_queries_.size() < MAX_PEER_TOTAL_QUERIES);
    auto part_id = part.part_id;
    state->node_queries_[static_cast<td::uint32>(part_id)];
    parts_helper_.lock_part(part_id);
    parts_.total_queries++;
    parts_.parts[part_id].query_to_peer = part.peer_id;
    state->notify_peer();
  }
}

void NodeActor::loop_get_peers() {
  if (has_get_peers_) {
    return;
  }
  if (next_get_peers_at_.is_in_past()) {
    callback_->get_peers(promise_send_closure(td::actor::actor_id(this), &NodeActor::got_peers));
    has_get_peers_ = true;
    return;
  }
  alarm_timestamp().relax(next_get_peers_at_);
}

void NodeActor::got_peers(td::Result<std::vector<PeerId>> r_peers) {
  if (r_peers.is_error()) {
    next_get_peers_at_ = td::Timestamp::in(GET_PEER_RETRY_TIMEOUT);
  } else {
    auto peers = r_peers.move_as_ok();
    for (auto &peer : peers) {
      if (peer == self_id_) {
        continue;
      }
      peers_[peer];
    }
    next_get_peers_at_ = td::Timestamp::in(GET_PEER_EACH);
  }
  has_get_peers_ = false;
  loop();
}

void NodeActor::loop_peer(const PeerId &peer_id, Peer &peer) {
  auto state = peer.state.lock();
  CHECK(!state->peer.empty());

  for (auto part_id : state->peer_ready_parts_) {
    parts_helper_.on_peer_part_ready(peer.peer_token, part_id);
  }
  state->peer_ready_parts_.clear();

  // Answer queries from peer
  bool should_notify_peer = false;

  auto want_download = parts_helper_.get_want_download_count(peer.peer_token) > 0;
  if (state->node_state_.want_download != want_download) {
    state->node_state_.want_download = want_download;
    should_notify_peer = true;
  }

  for (auto it = state->peer_queries_.begin(); it != state->peer_queries_.end();) {
    if (it->second) {
      it++;
    } else {
      should_notify_peer = true;
      it->second = [&]() -> td::Result<PeerState::Part> {
        if (!state->node_state_.will_upload) {
          return td::Status::Error("Won't upload");
        }
        TRY_RESULT(proof, torrent_.get_piece_proof(it->first));
        TRY_RESULT(data, torrent_.get_piece_data(it->first));
        PeerState::Part res;
        TRY_RESULT(proof_serialized, vm::std_boc_serialize(std::move(proof)));
        res.proof = std::move(proof_serialized);
        res.data = td::BufferSlice(std::move(data));
        return std::move(res);
      }();
    }
  }

  // Handle results from peer
  for (auto it = state->node_queries_.begin(); it != state->node_queries_.end();) {
    if (it->second) {
      auto part_id = it->first;
      auto r_unit = it->second.unwrap().move_fmap([&](PeerState::Part part) -> td::Result<td::Unit> {
        TRY_RESULT(proof, vm::std_boc_deserialize(part.proof));
        TRY_STATUS(torrent_.add_piece(part_id, part.data.as_slice(), std::move(proof)));
        download_.add(part.data.size(), td::Timestamp::now());
        return td::Unit();
      });

      parts_.parts[part_id].query_to_peer = {};
      parts_.total_queries--;
      it = state->node_queries_.erase(it);
      parts_helper_.unlock_part(part_id);

      if (r_unit.is_ok()) {
        on_part_ready(part_id);
      } else {
        //LOG(ERROR) << "Failed " << part_id;
      }
    } else {
      it++;
    }
  }

  if (should_notify_peer) {
    state->notify_peer();
  }

  yield();
}

void NodeActor::on_part_ready(PartId part_id) {
  parts_helper_.on_self_part_ready(part_id);
  CHECK(!parts_.parts[part_id].ready);
  parts_.parts[part_id].ready = true;
  for (auto &peer : peers_) {
    // TODO: notify only peer want_download_count == 0
    peer.second.state.unsafe()->notify_node();
  }
  ready_parts_.push_back(part_id);
}
}  // namespace ton
