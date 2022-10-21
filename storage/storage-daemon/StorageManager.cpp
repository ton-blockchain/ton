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
*/

#include "StorageManager.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"
#include "td/db/RocksDb.h"
#include "td/actor/MultiPromise.h"

static overlay::OverlayIdFull get_overlay_id(td::Bits256 hash) {
  td::BufferSlice hash_str(hash.as_slice());
  return overlay::OverlayIdFull(std::move(hash_str));
}

StorageManager::StorageManager(adnl::AdnlNodeIdShort local_id, std::string db_root, td::unique_ptr<Callback> callback,
                               td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<ton_rldp::Rldp> rldp,
                               td::actor::ActorId<overlay::Overlays> overlays)
    : local_id_(local_id)
    , db_root_(std::move(db_root))
    , callback_(std::move(callback))
    , adnl_(std::move(adnl))
    , rldp_(std::move(rldp))
    , overlays_(std::move(overlays)) {
}

void StorageManager::start_up() {
  CHECK(db_root_ != "");
  td::mkdir(db_root_).ensure();
  db_root_ = td::realpath(db_root_).move_as_ok();
  td::mkdir(db_root_ + "/torrent-db").ensure();
  td::mkdir(db_root_ + "/torrent-files").ensure();
  LOG(INFO) << "Starting Storage manager. DB = " << db_root_;

  db_ = std::make_shared<db::DbType>(
      std::make_shared<td::RocksDb>(td::RocksDb::open(db_root_ + "/torrent-db").move_as_ok()));
  db::db_get<ton_api::storage_db_torrentList>(
      *db_, create_hash_tl_object<ton_api::storage_db_key_torrentList>(), true,
      [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_db_torrentList>> R) {
        std::vector<td::Bits256> torrents;
        if (R.is_error()) {
          LOG(ERROR) << "Failed to load torrent list from db: " << R.move_as_error();
        } else {
          auto r = R.move_as_ok();
          if (r != nullptr) {
            torrents = std::move(r->torrents_);
          }
        }
        td::actor::send_closure(SelfId, &StorageManager::load_torrents_from_db, std::move(torrents));
      });
}

void StorageManager::load_torrents_from_db(std::vector<td::Bits256> torrents) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    td::actor::send_closure(SelfId, &StorageManager::after_load_torrents_from_db);
  });
  for (auto hash : torrents) {
    CHECK(!torrents_.count(hash))
    auto& entry = torrents_[hash];
    entry.peer_manager =
        td::actor::create_actor<PeerManager>("PeerManager", local_id_, get_overlay_id(hash), overlays_, adnl_, rldp_);
    NodeActor::load_from_db(db_, hash, PeerManager::create_callback(entry.peer_manager.get()),
                            [SelfId = actor_id(this), hash,
                             promise = ig.get_promise()](td::Result<td::actor::ActorOwn<NodeActor>> R) mutable {
                              td::actor::send_closure(SelfId, &StorageManager::loaded_torrent_from_db, hash,
                                                      std::move(R));
                              promise.set_result(td::Unit());
                            });
  }
}

void StorageManager::loaded_torrent_from_db(td::Bits256 hash, td::Result<td::actor::ActorOwn<NodeActor>> R) {
  if (R.is_error()) {
    LOG(ERROR) << "Failed to load torrent " << hash.to_hex() << " from db: " << R.move_as_error();
    torrents_.erase(hash);
  } else {
    auto it = torrents_.find(hash);
    CHECK(it != torrents_.end());
    it->second.actor = R.move_as_ok();
    LOG(INFO) << "Loaded torrent " << hash.to_hex() << " from db";
  }
}

void StorageManager::after_load_torrents_from_db() {
  LOG(INFO) << "Finished loading torrents from db (" << torrents_.size() << " torrents)";
  db_store_torrent_list();
  callback_->on_ready();
}

void StorageManager::add_torrent(Torrent torrent, bool start_download, td::Promise<td::Unit> promise) {
  TRY_STATUS_PROMISE(promise, add_torrent_impl(std::move(torrent), start_download));
  db_store_torrent_list();
  promise.set_result(td::Unit());
}

td::Status StorageManager::add_torrent_impl(Torrent torrent, bool start_download) {
  td::Bits256 hash = torrent.get_hash();
  if (torrents_.count(hash)) {
    return td::Status::Error("Cannot add torrent: duplicate hash");
  }
  TorrentEntry& entry = torrents_[hash];
  entry.hash = hash;
  entry.peer_manager =
      td::actor::create_actor<PeerManager>("PeerManager", local_id_, get_overlay_id(hash), overlays_, adnl_, rldp_);
  auto context = PeerManager::create_callback(entry.peer_manager.get());
  LOG(INFO) << "Added torrent " << hash.to_hex() << " , root_dir = " << torrent.get_root_dir();
  entry.actor =
      td::actor::create_actor<NodeActor>("Node", 1, std::move(torrent), std::move(context), db_, start_download);
  return td::Status::OK();
}

void StorageManager::add_torrent_by_meta(TorrentMeta meta, std::string root_dir, bool start_download,
                                         td::Promise<td::Unit> promise) {
  td::Bits256 hash(meta.info.get_hash().bits());
  Torrent::Options options;
  options.root_dir = root_dir.empty() ? db_root_ + "/torrent-files/" + hash.to_hex() : root_dir;
  TRY_RESULT_PROMISE(promise, torrent, Torrent::open(std::move(options), std::move(meta)));
  add_torrent(std::move(torrent), start_download, std::move(promise));
}

void StorageManager::add_torrent_by_hash(td::Bits256 hash, std::string root_dir, bool start_download,
                                         td::Promise<td::Unit> promise) {
  Torrent::Options options;
  options.root_dir = root_dir.empty() ? db_root_ + "/torrent-files/" + hash.to_hex() : root_dir;
  TRY_RESULT_PROMISE(promise, torrent, Torrent::open(std::move(options), hash));
  add_torrent(std::move(torrent), start_download, std::move(promise));
}

void StorageManager::set_active_download(td::Bits256 hash, bool active, td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::set_should_download, active);
  promise.set_result(td::Unit());
}

void StorageManager::with_torrent(td::Bits256 hash, td::Promise<NodeActor::NodeState> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::with_torrent, std::move(promise));
}

void StorageManager::get_all_torrents(td::Promise<std::vector<td::Bits256>> promise) {
  std::vector<td::Bits256> result;
  for (const auto& p : torrents_) {
    result.push_back(p.first);
  }
  promise.set_result(std::move(result));
}

void StorageManager::db_store_torrent_list() {
  std::vector<td::Bits256> torrents;
  for (const auto& p : torrents_) {
    torrents.push_back(p.first);
  }
  db_->set(create_hash_tl_object<ton_api::storage_db_key_torrentList>(),
           create_serialize_tl_object<ton_api::storage_db_torrentList>(std::move(torrents)),
           [](td::Result<td::Unit> R) {
             if (R.is_error()) {
               LOG(ERROR) << "Failed to save torrent list to db: " << R.move_as_error();
             }
           });
}

void StorageManager::set_all_files_priority(td::Bits256 hash, td::uint8 priority, td::Promise<bool> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::set_all_files_priority, priority, std::move(promise));
}

void StorageManager::set_file_priority_by_idx(td::Bits256 hash, size_t idx, td::uint8 priority,
                                              td::Promise<bool> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::set_file_priority_by_idx, idx, priority, std::move(promise));
}

void StorageManager::set_file_priority_by_name(td::Bits256 hash, std::string name, td::uint8 priority,
                                               td::Promise<bool> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::set_file_priority_by_name, std::move(name), priority,
                          std::move(promise));
}
