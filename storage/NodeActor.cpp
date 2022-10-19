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

#include "NodeActor.h"

#include "vm/boc.h"
#include "vm/cellslice.h"

#include "td/utils/Enumerator.h"
#include "td/utils/tests.h"
#include "td/utils/overloaded.h"

namespace ton {
NodeActor::NodeActor(PeerId self_id, ton::Torrent torrent, td::unique_ptr<Callback> callback, bool should_download)
    : self_id_(self_id)
    , torrent_(std::move(torrent))
    , callback_(std::move(callback))
    , should_download_(should_download) {
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
  if (torrent_.inited_info()) {
    init_torrent();
  }
  loop();
}

void NodeActor::init_torrent() {
  auto pieces_count = torrent_.get_info().pieces_count();
  parts_helper_.init_parts_count(pieces_count);
  parts_.parts.resize(pieces_count);

  auto header = torrent_.get_header_parts_range();
  for (auto i = static_cast<td::uint32>(header.begin); i < header.end; i++) {
    parts_helper_.set_part_priority(i, 255);
  }
  for (td::uint32 i = 0; i < pieces_count; i++) {
    if (torrent_.is_piece_ready(i)) {
      parts_helper_.on_self_part_ready(i);
      parts_.parts[i].ready = true;
    }
  }

  torrent_info_str_ =
      std::make_shared<td::BufferSlice>(vm::std_boc_serialize(torrent_.get_info().as_cell()).move_as_ok());
  for (auto &p : peers_) {
    auto &state = p.second.state;
    state->torrent_info_str_ = torrent_info_str_;
    CHECK(!state->torrent_info_ready_.exchange(true));
  }
  if (torrent_.inited_header()) {
    init_torrent_header();
  }
}

void NodeActor::init_torrent_header() {
  if (header_ready_) {
    return;
  }
  header_ready_ = true;
  size_t files_count = torrent_.get_files_count().unwrap();
  for (size_t i = 0; i < files_count; ++i) {
    file_name_to_idx_[torrent_.get_file_name(i).str()] = i;
  }
  file_priority_.resize(files_count, 1);
  for (auto &s : pending_set_file_priority_) {
    td::Promise<bool> P = [](td::Result<bool>) {};
    s.file.visit(
        td::overloaded([&](const PendingSetFilePriority::All &) { set_all_files_priority(s.priority, std::move(P)); },
                       [&](const size_t &i) { set_file_priority_by_idx(i, s.priority, std::move(P)); },
                       [&](const std::string &name) { set_file_priority_by_name(name, s.priority, std::move(P)); }));
  }
  pending_set_file_priority_.clear();
  torrent_.enable_write_to_files();
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
    auto &state = it.second.state;
    bool needed = false;
    if (state->peer_state_ready_) {
      needed = state->peer_state_.load().want_download;
    }
    peers.emplace_back(!needed, !state->node_state_.load().want_download, -state->download.speed(), it.first);
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
    auto &state = it.second.state;
    auto node_state = state->node_state_.load();
    if (node_state.will_upload != will_upload) {
      node_state.will_upload = will_upload;
      state->node_state_.exchange(node_state);
      state->notify_peer();
    }
  }
}

void NodeActor::loop() {
  loop_get_peers();
  loop_start_stop_peers();
  if (torrent_.inited_info()) {
    loop_queries();
    loop_will_upload();
  }

  if (!ready_parts_.empty()) {
    for (auto &it : peers_) {
      auto &state = it.second.state;
      state->node_ready_parts_.add_elements(ready_parts_);
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
    auto &state = it.second.state;
    sb << "\tPeer " << it.first;
    if (torrent_.inited_info()) {
      sb << "\t" << parts_helper_.get_ready_parts(it.second.peer_token).ones_count();
    }
    sb << "\t" << state->download;
    if (state->peer_state_ready_) {
      auto peer_state = state->peer_state_.load();
      sb << "\t  up:" << peer_state.will_upload;
      sb << "\tdown:" << peer_state.want_download;
      if (torrent_.inited_info()) {
        sb << "\tcnt:" << parts_helper_.get_want_download_count(it.second.peer_token);
      }
    }
    sb << "\toutq:" << state->node_queries_active_.size();
    auto node_state = state->node_state_.load();
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

void NodeActor::set_all_files_priority(td::uint8 priority, td::Promise<bool> promise) {
  if (!header_ready_) {
    pending_set_file_priority_.clear();
    pending_set_file_priority_.push_back(PendingSetFilePriority{PendingSetFilePriority::All(), priority});
    promise.set_result(false);
    return;
  }
  auto header_range = torrent_.get_header_parts_range();
  for (td::uint32 i = 0; i < torrent_.get_info().pieces_count(); i++) {
    if (!header_range.contains(i)) {
      parts_helper_.set_part_priority(i, priority);
    }
  }
  for (size_t i = 0; i < file_priority_.size(); ++i) {
    file_priority_[i] = priority;
    torrent_.set_file_excluded(i, priority == 0);
  }
  promise.set_result(true);
  yield();
}

void NodeActor::set_file_priority_by_idx(size_t i, td::uint8 priority, td::Promise<bool> promise) {
  if (!header_ready_) {
    pending_set_file_priority_.push_back(PendingSetFilePriority{i, priority});
    promise.set_result(false);
    return;
  }
  auto files_count = torrent_.get_files_count().unwrap();
  if (i >= files_count) {
    promise.set_error(td::Status::Error("File index is too big"));
    return;
  }
  if (file_priority_[i] == priority) {
    promise.set_result(true);
    return;
  }
  file_priority_[i] = priority;
  torrent_.set_file_excluded(i, priority == 0);
  auto range = torrent_.get_file_parts_range(i);
  for (auto i = range.begin; i < range.end; i++) {
    if (i == range.begin || i + 1 == range.end) {
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
  promise.set_result(true);
  yield();
}

void NodeActor::set_file_priority_by_name(std::string name, td::uint8 priority, td::Promise<bool> promise) {
  if (!header_ready_) {
    pending_set_file_priority_.push_back(PendingSetFilePriority{name, priority});
    promise.set_result(false);
    return;
  }
  auto it = file_name_to_idx_.find(name);
  if (it == file_name_to_idx_.end()) {
    promise.set_error(td::Status::Error("No such file"));
    return;
  }
  set_file_priority_by_idx(it->second, priority, std::move(promise));
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
      auto &state = peer.state = std::make_shared<PeerState>(peer.notifier.get());
      if (torrent_.inited_info()) {
        std::vector<td::uint32> node_ready_parts;
        for (td::uint32 i = 0; i < parts_.parts.size(); i++) {
          if (parts_.parts[i].ready) {
            node_ready_parts.push_back(i);
          }
        }
        state->node_ready_parts_.add_elements(std::move(node_ready_parts));
        state->torrent_info_str_ = torrent_info_str_;
        state->torrent_info_ready_ = true;
      } else {
        state->torrent_info_response_callback_ = [SelfId = actor_id(this)](td::BufferSlice data) {
          td::actor::send_closure(SelfId, &NodeActor::got_torrent_info_str, std::move(data));
        };
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
    auto &state = it.second.state;
    if (!state->peer_state_ready_) {
      parts_helper_.set_peer_limit(peer_token, 0);
      continue;
    }
    if (!state->peer_state_.load().will_upload) {
      parts_helper_.set_peer_limit(peer_token, 0);
      continue;
    }
    parts_helper_.set_peer_limit(
        peer_token, td::narrow_cast<td::uint32>(MAX_PEER_TOTAL_QUERIES - state->node_queries_active_.size()));
  }

  auto parts = parts_helper_.get_rarest_parts(MAX_TOTAL_QUERIES);
  for (auto &part : parts) {
    auto it = peers_.find(part.peer_id);
    CHECK(it != peers_.end());
    auto &state = it->second.state;
    CHECK(state->peer_state_ready_);
    CHECK(state->peer_state_.load().will_upload);
    CHECK(state->node_queries_active_.size() < MAX_PEER_TOTAL_QUERIES);
    auto part_id = part.part_id;
    if (state->node_queries_active_.insert(static_cast<td::uint32>(part_id)).second) {
      state->node_queries_.add_element(static_cast<td::uint32>(part_id));
    }
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
    callback_->get_peers(self_id_, promise_send_closure(td::actor::actor_id(this), &NodeActor::got_peers));
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
  auto &state = peer.state;
  if (!state->peer_ready_ || !torrent_.inited_info()) {
    return;
  }

  for (auto part_id : state->peer_ready_parts_.read()) {
    parts_helper_.on_peer_part_ready(peer.peer_token, part_id);
  }

  // Answer queries from peer
  bool should_notify_peer = false;

  auto want_download = parts_helper_.get_want_download_count(peer.peer_token) > 0;
  auto node_state = state->node_state_.load();
  if (node_state.want_download != want_download) {
    node_state.want_download = want_download;
    state->node_state_.exchange(node_state);
    should_notify_peer = true;
  }

  std::vector<std::pair<td::uint32, td::Result<PeerState::Part>>> results;
  for (td::uint32 part_id : state->peer_queries_.read()) {
    should_notify_peer = true;
    auto res = [&]() -> td::Result<PeerState::Part> {
      if (!node_state.will_upload) {
        return td::Status::Error("Won't upload");
      }
      TRY_RESULT(proof, torrent_.get_piece_proof(part_id));
      TRY_RESULT(data, torrent_.get_piece_data(part_id));
      PeerState::Part res;
      TRY_RESULT(proof_serialized, vm::std_boc_serialize(std::move(proof)));
      res.proof = std::move(proof_serialized);
      res.data = td::BufferSlice(std::move(data));
      return std::move(res);
    }();
    results.emplace_back(part_id, std::move(res));
  }
  state->peer_queries_results_.add_elements(std::move(results));

  // Handle results from peer
  for (auto &p : state->node_queries_results_.read()) {
    auto part_id = p.first;
    if (!state->node_queries_active_.count(part_id)) {
      continue;
    }
    auto r_unit = p.second.move_fmap([&](PeerState::Part part) -> td::Result<td::Unit> {
      TRY_RESULT(proof, vm::std_boc_deserialize(part.proof));
      TRY_STATUS(torrent_.add_piece(part_id, part.data.as_slice(), std::move(proof)));
      download_.add(part.data.size(), td::Timestamp::now());
      return td::Unit();
    });

    parts_.parts[part_id].query_to_peer = {};
    parts_.total_queries--;
    state->node_queries_active_.erase(part_id);
    parts_helper_.unlock_part(part_id);

    if (r_unit.is_ok()) {
      on_part_ready(part_id);
    }
  }

  if (!header_ready_ && torrent_.inited_info() && torrent_.inited_header()) {
    init_torrent_header();
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
    peer.second.state->notify_peer();
  }
  ready_parts_.push_back(part_id);
}

void NodeActor::got_torrent_info_str(td::BufferSlice data) {
  if (torrent_.inited_info()) {
    return;
  }
  auto r_info_cell = vm::std_boc_deserialize(data.as_slice());
  if (r_info_cell.is_error()) {
    return;
  }
  TorrentInfo info;
  vm::CellSlice cs = vm::load_cell_slice(r_info_cell.move_as_ok());
  if (!info.unpack(cs)) {
    return;
  }
  info.init_cell();
  if (torrent_.init_info(std::move(info)).is_error()) {
    return;
  }
  init_torrent();
  loop();
}

}  // namespace ton
