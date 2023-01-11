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
#include "tl-utils/common-utils.hpp"
#include "tl-utils/tl-utils.hpp"
#include "auto/tl/ton_api.hpp"
#include "td/actor/MultiPromise.h"

namespace ton {
NodeActor::NodeActor(PeerId self_id, Torrent torrent, td::unique_ptr<Callback> callback,
                     td::unique_ptr<NodeCallback> node_callback, std::shared_ptr<db::DbType> db, bool should_download,
                     bool should_upload)
    : self_id_(self_id)
    , torrent_(std::move(torrent))
    , callback_(std::move(callback))
    , node_callback_(std::move(node_callback))
    , db_(std::move(db))
    , should_download_(should_download)
    , should_upload_(should_upload) {
}

NodeActor::NodeActor(PeerId self_id, ton::Torrent torrent, td::unique_ptr<Callback> callback,
                     td::unique_ptr<NodeCallback> node_callback, std::shared_ptr<db::DbType> db, bool should_download,
                     bool should_upload, DbInitialData db_initial_data)
    : self_id_(self_id)
    , torrent_(std::move(torrent))
    , callback_(std::move(callback))
    , node_callback_(std::move(node_callback))
    , db_(std::move(db))
    , should_download_(should_download)
    , should_upload_(should_upload)
    , pending_set_file_priority_(std::move(db_initial_data.priorities))
    , pieces_in_db_(std::move(db_initial_data.pieces_in_db)) {
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
  auto it = peers_.find(peer_id);
  if (it == peers_.end()) {
    return;
  }
  loop_peer(peer_id, it->second);
}

void NodeActor::start_up() {
  node_callback_->register_self(actor_id(this));
  db_store_torrent();
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
      on_part_ready(i);
    }
  }

  torrent_info_str_ =
      std::make_shared<td::BufferSlice>(vm::std_boc_serialize(torrent_.get_info().as_cell()).move_as_ok());
  for (auto &p : peers_) {
    auto &state = p.second.state;
    state->torrent_info_str_ = torrent_info_str_;
    CHECK(!state->torrent_info_ready_.exchange(true));
  }
  LOG(INFO) << "Inited torrent info for " << torrent_.get_hash().to_hex() << ": size=" << torrent_.get_info().file_size
            << ", pieces=" << torrent_.get_info().pieces_count();
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
  db_store_priorities_paused_ = true;
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
  db_store_priorities_paused_ = false;
  db_store_priorities();

  auto pieces = pieces_in_db_;
  for (td::uint64 p : pieces) {
    if (!torrent_.is_piece_in_memory(p)) {
      db_erase_piece(p);
    }
  }
  for (td::uint64 p : torrent_.get_pieces_in_memory()) {
    if (!pieces_in_db_.count(p)) {
      db_store_piece(p, torrent_.get_piece_data(p).move_as_ok());
    }
  }
  db_update_pieces_list();
  recheck_parts(Torrent::PartsRange{0, torrent_.get_info().pieces_count()});
  db_store_torrent_meta();

  LOG(INFO) << "Inited torrent header for " << torrent_.get_hash().to_hex()
            << ": files=" << torrent_.get_files_count().value() << ", included_size=" << torrent_.get_included_size();
}

void NodeActor::recheck_parts(Torrent::PartsRange range) {
  CHECK(torrent_.inited_info());
  for (size_t i = range.begin; i < range.end; ++i) {
    if (parts_.parts[i].ready && !torrent_.is_piece_ready(i)) {
      parts_helper_.on_self_part_not_ready(i);
      parts_.parts[i].ready = false;
    } else if (!parts_.parts[i].ready && torrent_.is_piece_ready(i)) {
      on_part_ready((PartId)i);
    }
  }
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
    peers.emplace_back(!needed, !state->node_state_.load().want_download, -it.second.download_speed.speed(), it.first);
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
    auto will_upload = peers_set.count(it.first) > 0 && should_upload_;
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

  if (next_db_store_meta_at_ && next_db_store_meta_at_.is_in_past()) {
    db_store_torrent_meta();
  }

  if (torrent_.get_fatal_error().is_error()) {
    for (auto &promise : wait_for_completion_) {
      promise.set_error(torrent_.get_fatal_error().clone());
    }
    wait_for_completion_.clear();
  } else if (torrent_.is_completed()) {
    db_store_torrent_meta();
    if (!is_completed_) {
      for (auto &promise : wait_for_completion_) {
        promise.set_result(td::Unit());
      }
      wait_for_completion_.clear();
      is_completed_ = true;
      callback_->on_completed();
    }
  }
}

std::string NodeActor::get_stats_str() {
  td::StringBuilder sb;
  sb << "Node " << self_id_ << " " << torrent_.get_ready_parts_count() << "\t" << download_speed_;
  sb << "\toutq " << parts_.total_queries;
  sb << "\n";
  for (auto &it : peers_) {
    auto &state = it.second.state;
    sb << "\tPeer " << it.first;
    if (torrent_.inited_info()) {
      sb << "\t" << parts_helper_.get_ready_parts(it.second.peer_token).ones_count();
    }
    sb << "\t" << it.second.download_speed;
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
    db_store_priorities();
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
  recheck_parts(Torrent::PartsRange{0, torrent_.get_info().pieces_count()});
  db_store_priorities();
  update_pieces_in_db(0, torrent_.get_info().pieces_count());
  if (!torrent_.is_completed()) {
    is_completed_ = false;
  }
  promise.set_result(true);
  yield();
}

void NodeActor::set_file_priority_by_idx(size_t i, td::uint8 priority, td::Promise<bool> promise) {
  if (!header_ready_) {
    pending_set_file_priority_.push_back(PendingSetFilePriority{i, priority});
    db_store_priorities();
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
  recheck_parts(range);
  update_pieces_in_db(range.begin, range.end);
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
  db_store_priorities();
  if (!torrent_.is_completed()) {
    is_completed_ = false;
  }
  promise.set_result(true);
  yield();
}

void NodeActor::set_file_priority_by_name(std::string name, td::uint8 priority, td::Promise<bool> promise) {
  if (!header_ready_) {
    pending_set_file_priority_.push_back(PendingSetFilePriority{name, priority});
    db_store_priorities();
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

void NodeActor::wait_for_completion(td::Promise<td::Unit> promise) {
  if (torrent_.get_fatal_error().is_error()) {
    promise.set_error(torrent_.get_fatal_error().clone());
  } else if (is_completed_) {
    promise.set_result(td::Unit());
  } else {
    wait_for_completion_.push_back(std::move(promise));
  }
}

void NodeActor::set_should_download(bool should_download) {
  if (should_download == should_download_) {
    return;
  }
  should_download_ = should_download;
  db_store_torrent();
  yield();
}

void NodeActor::set_should_upload(bool should_upload) {
  if (should_upload == should_upload_) {
    return;
  }
  should_upload_ = should_upload;
  db_store_torrent();
  yield();
}

void NodeActor::load_from(td::optional<TorrentMeta> meta, std::string files_path, td::Promise<td::Unit> promise) {
  auto S = [&]() -> td::Status {
    if (meta) {
      TorrentInfo &info = meta.value().info;
      if (info.get_hash() != torrent_.get_hash()) {
        return td::Status::Error("Incorrect hash in meta");
      }
      if (!torrent_.inited_info()) {
        LOG(INFO) << "Loading torrent info for " << torrent_.get_hash().to_hex();
        TRY_STATUS(torrent_.init_info(std::move(info)));
        init_torrent();
      }
      auto &header = meta.value().header;
      if (header && !torrent_.inited_header()) {
        LOG(INFO) << "Loading torrent header for " << torrent_.get_hash().to_hex();
        TRY_STATUS(torrent_.set_header(header.unwrap()));
        init_torrent_header();
      }
      auto proof = std::move(meta.value().root_proof);
      if (!proof.is_null()) {
        LOG(INFO) << "Loading proof for " << torrent_.get_hash().to_hex();
        TRY_STATUS(torrent_.add_proof(std::move(proof)));
      }
    }
    TRY_STATUS_PREFIX(torrent_.get_fatal_error().clone(), "Fatal error: ");
    if (torrent_.inited_header() && !files_path.empty()) {
      torrent_.load_from_files(std::move(files_path));
    }
    TRY_STATUS_PREFIX(torrent_.get_fatal_error().clone(), "Fatal error: ");
    return td::Status::OK();
  }();
  if (S.is_error()) {
    LOG(WARNING) << "Load from failed: " << S;
    promise.set_error(std::move(S));
  } else {
    promise.set_result(td::Unit());
  }
  if (torrent_.inited_header()) {
    recheck_parts(Torrent::PartsRange{0, torrent_.get_info().pieces_count()});
  }
  loop();
}

void NodeActor::copy_to_new_root_dir(std::string new_root_dir, td::Promise<td::Unit> promise) {
  TRY_STATUS_PROMISE(promise, torrent_.copy_to(new_root_dir));
  db_store_torrent();
  promise.set_result(td::Unit());
}

void NodeActor::tear_down() {
  for (auto &promise : wait_for_completion_) {
    promise.set_error(td::Status::Error("Torrent closed"));
  }
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
      peer.actor = node_callback_->create_peer(self_id_, peer_id, peer.state);
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
    node_callback_->get_peers(self_id_, promise_send_closure(td::actor::actor_id(this), &NodeActor::got_peers));
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
      if (!node_state.will_upload || !should_upload_) {
        return td::Status::Error("Won't upload");
      }
      TRY_RESULT(proof, torrent_.get_piece_proof(part_id));
      TRY_RESULT(data, torrent_.get_piece_data(part_id));
      PeerState::Part res;
      TRY_RESULT(proof_serialized, vm::std_boc_serialize(std::move(proof)));
      res.proof = std::move(proof_serialized);
      res.data = td::BufferSlice(std::move(data));
      td::uint64 size = res.data.size() + res.proof.size();
      upload_speed_.add(size);
      peer.upload_speed.add(size);
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
      update_pieces_in_db(part_id, part_id + 1);
      download_speed_.add(part.data.size());
      peer.download_speed.add(part.data.size());
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

void NodeActor::update_pieces_in_db(td::uint64 begin, td::uint64 end) {
  bool changed = false;
  for (auto i = begin; i < end; ++i) {
    bool stored = pieces_in_db_.count(i);
    bool need_store = torrent_.is_piece_in_memory(i);
    if (need_store == stored) {
      continue;
    }
    changed = true;
    if (need_store) {
      db_store_piece(i, torrent_.get_piece_data(i).move_as_ok());
    } else {
      db_erase_piece(i);
    }
  }
  if (changed) {
    db_update_pieces_list();
  }
}

void NodeActor::db_store_torrent() {
  if (!db_) {
    return;
  }
  auto obj = create_tl_object<ton_api::storage_db_torrent>();
  obj->active_download_ = should_download_;
  obj->active_upload_ = should_upload_;
  obj->root_dir_ = torrent_.get_root_dir();
  db_->set(create_hash_tl_object<ton_api::storage_db_key_torrent>(torrent_.get_hash()), serialize_tl_object(obj, true),
           [](td::Result<td::Unit> R) {
             if (R.is_error()) {
               LOG(ERROR) << "Failed to save torrent to db: " << R.move_as_error();
             }
           });
}

void NodeActor::db_store_priorities() {
  if (!db_ || db_store_priorities_paused_) {
    return;
  }
  auto obj = create_tl_object<ton_api::storage_db_priorities>();
  if (file_priority_.empty()) {
    for (auto &s : pending_set_file_priority_) {
      s.file.visit(td::overloaded(
          [&](const PendingSetFilePriority::All &) {
            obj->actions_.push_back(create_tl_object<ton_api::storage_priorityAction_all>(s.priority));
          },
          [&](const size_t &i) {
            obj->actions_.push_back(create_tl_object<ton_api::storage_priorityAction_idx>(i, s.priority));
          },
          [&](const std::string &name) {
            obj->actions_.push_back(create_tl_object<ton_api::storage_priorityAction_name>(name, s.priority));
          }));
    }
  } else {
    size_t prior_cnt[256];
    std::fill(prior_cnt, prior_cnt + 256, 0);
    for (td::uint8 p : file_priority_) {
      ++prior_cnt[p];
    }
    auto base_priority = (td::uint8)(std::max_element(prior_cnt, prior_cnt + 256) - prior_cnt);
    obj->actions_.push_back(create_tl_object<ton_api::storage_priorityAction_all>(base_priority));
    for (size_t i = 0; i < file_priority_.size(); ++i) {
      if (file_priority_[i] != base_priority) {
        obj->actions_.push_back(create_tl_object<ton_api::storage_priorityAction_idx>(i, file_priority_[i]));
      }
    }
  }
  db_->set(create_hash_tl_object<ton_api::storage_db_key_priorities>(torrent_.get_hash()),
           serialize_tl_object(obj, true), [](td::Result<td::Unit> R) {
             if (R.is_error()) {
               LOG(ERROR) << "Failed to save torrent priorities to db: " << R.move_as_error();
             }
           });
}

void NodeActor::db_store_torrent_meta() {
  if (!db_ || !torrent_.inited_info() || (td::int64)torrent_.get_ready_parts_count() == last_stored_meta_count_) {
    after_db_store_torrent_meta(last_stored_meta_count_);
    return;
  }
  next_db_store_meta_at_ = td::Timestamp::never();
  auto meta = torrent_.get_meta_str();
  db_->set(create_hash_tl_object<ton_api::storage_db_key_torrentMeta>(torrent_.get_hash()), td::BufferSlice(meta),
           [new_count = (td::int64)torrent_.get_ready_parts_count(), SelfId = actor_id(this)](td::Result<td::Unit> R) {
             if (R.is_error()) {
               td::actor::send_closure(SelfId, &NodeActor::after_db_store_torrent_meta, R.move_as_error());
             } else {
               td::actor::send_closure(SelfId, &NodeActor::after_db_store_torrent_meta, new_count);
             }
           });
}

void NodeActor::after_db_store_torrent_meta(td::Result<td::int64> R) {
  if (R.is_error()) {
    LOG(ERROR) << "Failed to save torrent meta to db: " << R.move_as_error();
  } else {
    last_stored_meta_count_ = R.move_as_ok();
  }
  next_db_store_meta_at_ = td::Timestamp::in(td::Random::fast(10.0, 20.0));
  alarm_timestamp().relax(next_db_store_meta_at_);
}

void NodeActor::db_store_piece(td::uint64 i, std::string s) {
  pieces_in_db_.insert(i);
  if (!db_) {
    return;
  }
  db_->set(create_hash_tl_object<ton_api::storage_db_key_pieceInDb>(torrent_.get_hash(), i), td::BufferSlice(s),
           [](td::Result<td::Unit> R) {
             if (R.is_error()) {
               LOG(ERROR) << "Failed to store piece to db: " << R.move_as_error();
             }
           });
}

void NodeActor::db_erase_piece(td::uint64 i) {
  pieces_in_db_.erase(i);
  if (!db_) {
    return;
  }
  db_->erase(create_hash_tl_object<ton_api::storage_db_key_pieceInDb>(torrent_.get_hash(), i),
             [](td::Result<td::Unit> R) {
               if (R.is_error()) {
                 LOG(ERROR) << "Failed to store piece to db: " << R.move_as_error();
               }
             });
}

void NodeActor::db_update_pieces_list() {
  if (!db_) {
    return;
  }
  auto obj = create_tl_object<ton_api::storage_db_piecesInDb>();
  for (td::uint64 p : pieces_in_db_) {
    obj->pieces_.push_back(p);
  }
  db_->set(create_hash_tl_object<ton_api::storage_db_key_piecesInDb>(torrent_.get_hash()),
           serialize_tl_object(obj, true), [](td::Result<td::Unit> R) {
             if (R.is_error()) {
               LOG(ERROR) << "Failed to store list of pieces to db: " << R.move_as_error();
             }
           });
}

void NodeActor::load_from_db(std::shared_ptr<db::DbType> db, td::Bits256 hash, td::unique_ptr<Callback> callback,
                             td::unique_ptr<NodeCallback> node_callback,
                             td::Promise<td::actor::ActorOwn<NodeActor>> promise) {
  class Loader : public td::actor::Actor {
   public:
    Loader(std::shared_ptr<db::DbType> db, td::Bits256 hash, td::unique_ptr<Callback> callback,
           td::unique_ptr<NodeCallback> node_callback, td::Promise<td::actor::ActorOwn<NodeActor>> promise)
        : db_(std::move(db))
        , hash_(hash)
        , callback_(std::move(callback))
        , node_callback_(std::move(node_callback))
        , promise_(std::move(promise)) {
    }

    void finish(td::Result<td::actor::ActorOwn<NodeActor>> R) {
      promise_.set_result(std::move(R));
      stop();
    }

    void start_up() override {
      db::db_get<ton_api::storage_db_torrent>(
          *db_, create_hash_tl_object<ton_api::storage_db_key_torrent>(hash_), false,
          [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_db_torrent>> R) {
            if (R.is_error()) {
              td::actor::send_closure(SelfId, &Loader::finish, R.move_as_error_prefix("Torrent: "));
            } else {
              td::actor::send_closure(SelfId, &Loader::got_torrent, R.move_as_ok());
            }
          });
    }

    void got_torrent(tl_object_ptr<ton_api::storage_db_torrent> obj) {
      root_dir_ = std::move(obj->root_dir_);
      active_download_ = obj->active_download_;
      active_upload_ = obj->active_upload_;
      db_->get(create_hash_tl_object<ton_api::storage_db_key_torrentMeta>(hash_),
               [SelfId = actor_id(this)](td::Result<db::DbType::GetResult> R) {
                 if (R.is_error()) {
                   td::actor::send_closure(SelfId, &Loader::finish, R.move_as_error_prefix("Meta: "));
                   return;
                 }
                 auto r = R.move_as_ok();
                 if (r.status == td::KeyValueReader::GetStatus::NotFound) {
                   td::actor::send_closure(SelfId, &Loader::got_meta_str, td::optional<td::BufferSlice>());
                 } else {
                   td::actor::send_closure(SelfId, &Loader::got_meta_str, std::move(r.value));
                 }
               });
    }

    void got_meta_str(td::optional<td::BufferSlice> meta_str) {
      auto r_torrent = [&]() -> td::Result<Torrent> {
        Torrent::Options options;
        options.root_dir = std::move(root_dir_);
        options.in_memory = false;
        options.validate = false;
        if (meta_str) {
          TRY_RESULT(meta, TorrentMeta::deserialize(meta_str.value().as_slice()));
          options.validate = true;
          return Torrent::open(std::move(options), std::move(meta));
        } else {
          return Torrent::open(std::move(options), hash_);
        }
      }();
      if (r_torrent.is_error()) {
        finish(r_torrent.move_as_error());
        return;
      }
      torrent_ = r_torrent.move_as_ok();

      db::db_get<ton_api::storage_db_priorities>(
          *db_, create_hash_tl_object<ton_api::storage_db_key_priorities>(hash_), true,
          [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_db_priorities>> R) {
            if (R.is_error()) {
              td::actor::send_closure(SelfId, &Loader::finish, R.move_as_error_prefix("Priorities: "));
            } else {
              td::actor::send_closure(SelfId, &Loader::got_priorities, R.move_as_ok());
            }
          });
    }

    void got_priorities(tl_object_ptr<ton_api::storage_db_priorities> priorities) {
      if (priorities != nullptr) {
        for (auto &p : priorities->actions_) {
          td::Variant<PendingSetFilePriority::All, size_t, std::string> file;
          int priority = 0;
          ton_api::downcast_call(*p, td::overloaded(
                                         [&](ton_api::storage_priorityAction_all &obj) {
                                           file = PendingSetFilePriority::All();
                                           priority = obj.priority_;
                                         },
                                         [&](ton_api::storage_priorityAction_idx &obj) {
                                           file = (size_t)obj.idx_;
                                           priority = obj.priority_;
                                         },
                                         [&](ton_api::storage_priorityAction_name &obj) {
                                           file = std::move(obj.name_);
                                           priority = obj.priority_;
                                         }));
          auto R = td::narrow_cast_safe<td::uint8>(priority);
          if (R.is_error()) {
            LOG(ERROR) << "Invalid priority in db: " << R.move_as_error();
            continue;
          }
          priorities_.push_back(PendingSetFilePriority{std::move(file), R.move_as_ok()});
        }
      }

      db::db_get<ton_api::storage_db_piecesInDb>(
          *db_, create_hash_tl_object<ton_api::storage_db_key_piecesInDb>(hash_), true,
          [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_db_piecesInDb>> R) {
            if (R.is_error()) {
              td::actor::send_closure(SelfId, &Loader::finish, R.move_as_error_prefix("Pieces in db: "));
            } else {
              td::actor::send_closure(SelfId, &Loader::got_pieces_in_db, R.move_as_ok());
            }
          });
    }

    void got_pieces_in_db(tl_object_ptr<ton_api::storage_db_piecesInDb> list) {
      for (auto idx : list == nullptr ? std::vector<td::int64>() : list->pieces_) {
        ++remaining_pieces_in_db_;
        db_->get(create_hash_tl_object<ton_api::storage_db_key_pieceInDb>(hash_, idx),
                 [SelfId = actor_id(this), idx](td::Result<db::DbType::GetResult> R) {
                   if (R.is_error()) {
                     td::actor::send_closure(SelfId, &Loader::finish, R.move_as_error_prefix("Piece in db: "));
                     return;
                   }
                   auto r = R.move_as_ok();
                   td::optional<td::BufferSlice> piece;
                   if (r.status == td::KeyValueReader::GetStatus::Ok) {
                     piece = std::move(r.value);
                   }
                   td::actor::send_closure(SelfId, &Loader::got_piece_in_db, idx, std::move(piece));
                 });
      }
      if (remaining_pieces_in_db_ == 0) {
        finished_db_read();
      }
    }

    void got_piece_in_db(size_t idx, td::optional<td::BufferSlice> data) {
      if (data) {
        auto r_proof = torrent_.value().get_piece_proof(idx);
        if (r_proof.is_ok()) {
          torrent_.value().add_piece(idx, data.unwrap(), r_proof.move_as_ok());
        }
        pieces_in_db_.insert(idx);
      }
      if (--remaining_pieces_in_db_ == 0) {
        finished_db_read();
      }
    }

    void finished_db_read() {
      DbInitialData data;
      data.priorities = std::move(priorities_);
      data.pieces_in_db = std::move(pieces_in_db_);
      finish(td::actor::create_actor<NodeActor>("Node", 1, torrent_.unwrap(), std::move(callback_),
                                                std::move(node_callback_), std::move(db_), active_download_,
                                                active_upload_, std::move(data)));
    }

   private:
    std::shared_ptr<db::DbType> db_;
    td::Bits256 hash_;
    td::unique_ptr<Callback> callback_;
    td::unique_ptr<NodeCallback> node_callback_;
    td::Promise<td::actor::ActorOwn<NodeActor>> promise_;

    std::string root_dir_;
    bool active_download_{false};
    bool active_upload_{false};
    td::optional<Torrent> torrent_;
    std::vector<PendingSetFilePriority> priorities_;
    std::set<td::uint64> pieces_in_db_;
    size_t remaining_pieces_in_db_ = 0;
  };
  td::actor::create_actor<Loader>("loader", std::move(db), hash, std::move(callback), std::move(node_callback),
                                  std::move(promise))
      .release();
}

void NodeActor::cleanup_db(std::shared_ptr<db::DbType> db, td::Bits256 hash, td::Promise<td::Unit> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));
  db->erase(create_hash_tl_object<ton_api::storage_db_key_torrent>(hash), ig.get_promise());
  db->erase(create_hash_tl_object<ton_api::storage_db_key_torrentMeta>(hash), ig.get_promise());
  db->erase(create_hash_tl_object<ton_api::storage_db_key_priorities>(hash), ig.get_promise());
  db::db_get<ton_api::storage_db_piecesInDb>(
      *db, create_hash_tl_object<ton_api::storage_db_key_piecesInDb>(hash), true,
      [db, promise = ig.get_promise(), hash](td::Result<tl_object_ptr<ton_api::storage_db_piecesInDb>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        auto pieces = R.move_as_ok();
        if (pieces == nullptr) {
          promise.set_result(td::Unit());
          return;
        }
        td::MultiPromise mp;
        auto ig = mp.init_guard();
        ig.add_promise(std::move(promise));
        db->erase(create_hash_tl_object<ton_api::storage_db_key_piecesInDb>(hash), ig.get_promise());
        for (auto idx : pieces->pieces_) {
          db->erase(create_hash_tl_object<ton_api::storage_db_key_pieceInDb>(hash, idx), ig.get_promise());
        }
      });
}

void NodeActor::get_peers_info(td::Promise<tl_object_ptr<ton_api::storage_daemon_peerList>> promise) {
  auto result = std::make_shared<std::vector<tl_object_ptr<ton_api::storage_daemon_peer>>>();
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise([result, promise = std::move(promise), download_speed = download_speed_.speed(),
                  upload_speed = upload_speed_.speed(), parts = parts_.parts.size()](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
      return;
    }
    promise.set_result(
        create_tl_object<ton_api::storage_daemon_peerList>(std::move(*result), download_speed, upload_speed, parts));
  });

  result->reserve(peers_.size());
  size_t i = 0;
  for (auto &peer : peers_) {
    if (!peer.second.state->peer_online_) {
      continue;
    }
    result->push_back(create_tl_object<ton_api::storage_daemon_peer>());
    auto &obj = *result->back();
    obj.download_speed_ = peer.second.download_speed.speed();
    obj.upload_speed_ = peer.second.upload_speed.speed();
    obj.ready_parts_ = parts_helper_.get_ready_parts(peer.second.peer_token).ones_count();
    node_callback_->get_peer_info(
        self_id_, peer.first,
        [result, i, promise = ig.get_promise()](td::Result<std::pair<td::Bits256, std::string>> R) mutable {
          TRY_RESULT_PROMISE(promise, r, std::move(R));
          result->at(i)->adnl_id_ = r.first;
          result->at(i)->ip_str_ = r.second;
          promise.set_result(td::Unit());
        });
    ++i;
  }
}

}  // namespace ton
