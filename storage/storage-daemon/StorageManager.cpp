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

namespace ton {

static overlay::OverlayIdFull get_overlay_id(td::Bits256 hash) {
  td::BufferSlice hash_str(hash.as_slice());
  return overlay::OverlayIdFull(std::move(hash_str));
}

StorageManager::StorageManager(adnl::AdnlNodeIdShort local_id, std::string db_root, td::unique_ptr<Callback> callback,
                               bool client_mode, td::actor::ActorId<adnl::Adnl> adnl,
                               td::actor::ActorId<ton_rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays)
    : local_id_(local_id)
    , db_root_(std::move(db_root))
    , callback_(std::move(callback))
    , client_mode_(client_mode)
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
    entry.peer_manager = td::actor::create_actor<PeerManager>("PeerManager", local_id_, get_overlay_id(hash),
                                                              client_mode_, overlays_, adnl_, rldp_);
    NodeActor::load_from_db(
        db_, hash, create_callback(hash, entry.closing_state), PeerManager::create_callback(entry.peer_manager.get()),
        [SelfId = actor_id(this), hash,
         promise = ig.get_promise()](td::Result<td::actor::ActorOwn<NodeActor>> R) mutable {
          td::actor::send_closure(SelfId, &StorageManager::loaded_torrent_from_db, hash, std::move(R));
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

td::unique_ptr<NodeActor::Callback> StorageManager::create_callback(
    td::Bits256 hash, std::shared_ptr<TorrentEntry::ClosingState> closing_state) {
  class Callback : public NodeActor::Callback {
   public:
    Callback(td::actor::ActorId<StorageManager> id, td::Bits256 hash,
             std::shared_ptr<TorrentEntry::ClosingState> closing_state)
        : id_(std::move(id)), hash_(hash), closing_state_(std::move(closing_state)) {
    }
    void on_completed() override {
    }
    void on_closed(Torrent torrent) override {
      CHECK(torrent.get_hash() == hash_);
      td::actor::send_closure(id_, &StorageManager::on_torrent_closed, std::move(torrent), closing_state_);
    }

   private:
    td::actor::ActorId<StorageManager> id_;
    td::Bits256 hash_;
    std::shared_ptr<TorrentEntry::ClosingState> closing_state_;
  };
  return td::make_unique<Callback>(actor_id(this), hash, std::move(closing_state));
}

void StorageManager::add_torrent(Torrent torrent, bool start_download, bool allow_upload, bool copy_inside,
                                 td::Promise<td::Unit> promise) {
  td::Bits256 hash = torrent.get_hash();
  TRY_STATUS_PROMISE(promise, add_torrent_impl(std::move(torrent), start_download, allow_upload));
  db_store_torrent_list();
  if (!copy_inside) {
    promise.set_result(td::Unit());
    return;
  }
  TorrentEntry& entry = torrents_[hash];
  std::string new_dir = db_root_ + "/torrent-files/" + hash.to_hex();
  LOG(INFO) << "Copy torrent to " << new_dir;
  td::actor::send_closure(
      entry.actor, &NodeActor::copy_to_new_root_dir, std::move(new_dir),
      [SelfId = actor_id(this), hash, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          LOG(WARNING) << "Copy torrent: " << R.error();
          td::actor::send_closure(SelfId, &StorageManager::remove_torrent, hash, false, [](td::Result<td::Unit> R) {});
        }
        promise.set_result(std::move(R));
      });
}

td::Status StorageManager::add_torrent_impl(Torrent torrent, bool start_download, bool allow_upload) {
  td::Bits256 hash = torrent.get_hash();
  if (torrents_.count(hash)) {
    return td::Status::Error(PSTRING() << "Cannot add torrent: duplicate hash " << hash.to_hex());
  }
  TorrentEntry& entry = torrents_[hash];
  entry.hash = hash;
  entry.peer_manager = td::actor::create_actor<PeerManager>("PeerManager", local_id_, get_overlay_id(hash),
                                                            client_mode_, overlays_, adnl_, rldp_);
  auto context = PeerManager::create_callback(entry.peer_manager.get());
  LOG(INFO) << "Added torrent " << hash.to_hex() << " , root_dir = " << torrent.get_root_dir();
  entry.actor =
      td::actor::create_actor<NodeActor>("Node", 1, std::move(torrent), create_callback(hash, entry.closing_state),
                                         std::move(context), db_, start_download, allow_upload);
  return td::Status::OK();
}

void StorageManager::add_torrent_by_meta(TorrentMeta meta, std::string root_dir, bool start_download, bool allow_upload,
                                         td::Promise<td::Unit> promise) {
  td::Bits256 hash(meta.info.get_hash());
  Torrent::Options options;
  options.root_dir = root_dir.empty() ? db_root_ + "/torrent-files/" + hash.to_hex() : root_dir;
  TRY_RESULT_PROMISE(promise, torrent, Torrent::open(std::move(options), std::move(meta)));
  add_torrent(std::move(torrent), start_download, allow_upload, false, std::move(promise));
}

void StorageManager::add_torrent_by_hash(td::Bits256 hash, std::string root_dir, bool start_download, bool allow_upload,
                                         td::Promise<td::Unit> promise) {
  Torrent::Options options;
  options.root_dir = root_dir.empty() ? db_root_ + "/torrent-files/" + hash.to_hex() : root_dir;
  TRY_RESULT_PROMISE(promise, torrent, Torrent::open(std::move(options), hash));
  add_torrent(std::move(torrent), start_download, allow_upload, false, std::move(promise));
}

void StorageManager::set_active_download(td::Bits256 hash, bool active, td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::set_should_download, active);
  promise.set_result(td::Unit());
}

void StorageManager::set_active_upload(td::Bits256 hash, bool active, td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::set_should_upload, active);
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

void StorageManager::remove_torrent(td::Bits256 hash, bool remove_files, td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  LOG(INFO) << "Removing torrent " << hash.to_hex();
  entry->closing_state->removing = true;
  entry->closing_state->remove_files = remove_files;
  entry->closing_state->promise = std::move(promise);
  torrents_.erase(hash);
  db_store_torrent_list();
}

void StorageManager::load_from(td::Bits256 hash, td::optional<TorrentMeta> meta, std::string files_path,
                               td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::load_from, std::move(meta), std::move(files_path),
                          std::move(promise));
}

void StorageManager::on_torrent_closed(Torrent torrent, std::shared_ptr<TorrentEntry::ClosingState> closing_state) {
  if (!closing_state->removing) {
    return;
  }
  if (closing_state->remove_files && torrent.inited_header()) {
    size_t files_count = torrent.get_files_count().unwrap();
    for (size_t i = 0; i < files_count; ++i) {
      std::string path = torrent.get_file_path(i);
      td::unlink(path).ignore();
      // TODO: Check errors, remove empty directories
    }
  }
  td::rmrf(db_root_ + "/torrent-files/" + torrent.get_hash().to_hex()).ignore();
  NodeActor::cleanup_db(db_, torrent.get_hash(),
                        [promise = std::move(closing_state->promise)](td::Result<td::Unit> R) mutable {
                          if (R.is_error()) {
                            LOG(ERROR) << "Failed to cleanup database: " << R.move_as_error();
                          }
                          promise.set_result(td::Unit());
                        });
}

void StorageManager::wait_for_completion(td::Bits256 hash, td::Promise<td::Unit> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::wait_for_completion, std::move(promise));
}

void StorageManager::get_peers_info(td::Bits256 hash,
                                    td::Promise<tl_object_ptr<ton_api::storage_daemon_peerList>> promise) {
  TRY_RESULT_PROMISE(promise, entry, get_torrent(hash));
  td::actor::send_closure(entry->actor, &NodeActor::get_peers_info, std::move(promise));
}

}  // namespace ton