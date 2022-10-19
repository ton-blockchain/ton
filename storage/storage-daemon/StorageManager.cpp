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

template <typename T>
static td::Result<tl_object_ptr<T>> db_get(std::shared_ptr<td::KeyValue>& db, td::Bits256 key,
                                           bool allow_not_found = true) {
  std::string data;
  auto R = db->get(key.as_slice(), data);
  if (R.is_error()) {
    return R.move_as_error();
  }
  if (R.ok() == td::KeyValueReader::GetStatus::NotFound) {
    if (allow_not_found) {
      return nullptr;
    } else {
      return td::Status::Error("Not found");
    }
  }
  auto F = fetch_tl_object<T>(data, true);
  if (F.is_error()) {
    return F.move_as_error();
  }
  return F.move_as_ok();
}

StorageManager::StorageManager(adnl::AdnlNodeIdShort local_id, std::string db_root, td::actor::ActorId<adnl::Adnl> adnl,
                               td::actor::ActorId<ton_rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays)
    : local_id_(local_id)
    , db_root_(std::move(db_root))
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

  db_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_root_ + "/torrent-db").move_as_ok());
  for (td::Bits256 hash : db_load_torrent_list()) {
    auto S = [&]() -> td::Status {
      TRY_RESULT(t, db_get<ton_api::storage_daemon_db_torrentShort>(
                        db_, create_hash_tl_object<ton_api::storage_daemon_db_key_torrentShort>(hash), false));
      std::string meta_str;
      TRY_RESULT(
          meta_status,
          db_->get(create_hash_tl_object<ton_api::storage_daemon_db_key_torrentMeta>(hash).as_slice(), meta_str));
      bool has_meta = (meta_status == td::KeyValue::GetStatus::Ok);

      td::Result<Torrent> r_torrent;
      Torrent::Options options;
      options.root_dir = t->root_dir_;
      if (has_meta) {
        TRY_RESULT_PREFIX(meta, TorrentMeta::deserialize(meta_str), "Parse meta: ");
        if (hash != meta.info.get_hash().bits()) {
          return td::Status::Error("Hash mismatch in meta");
        }
        options.validate = (bool)meta.header;
        r_torrent = Torrent::open(std::move(options), std::move(meta));
      } else {
        options.validate = false;
        r_torrent = Torrent::open(std::move(options), hash);
      }
      TRY_RESULT(torrent, std::move(r_torrent));
      return add_torrent_impl(std::move(torrent), t->active_download_);
    }();
    if (S.is_error()) {
      LOG(ERROR) << "Failed to add torrent " << hash.to_hex() << " from db: " << S;
    }
  }

  alarm_timestamp() = td::Timestamp::in(1.0);
}

void StorageManager::alarm() {
  auto it = torrents_.upper_bound(save_meta_ptr_);
  if (it == torrents_.end()) {
    it = torrents_.begin();
  }
  if (it == torrents_.end()) {
    alarm_timestamp() = td::Timestamp::in(1.0);
    return;
  }
  save_meta_ptr_ = it->first;
  td::actor::send_closure(it->second.actor, &NodeActor::with_torrent,
                          [hash = it->first, SelfId = actor_id(this)](td::Result<NodeActor::NodeState> R) {
                            td::optional<TorrentMeta> meta;
                            if (R.is_ok()) {
                              Torrent& torrent = R.ok_ref().torrent;
                              if (torrent.inited_info()) {
                                meta = torrent.get_meta();
                              }
                            }
                            td::actor::send_closure(SelfId, &StorageManager::got_torrent_meta_for_db, hash,
                                                    std::move(meta));
                          });
}

void StorageManager::got_torrent_meta_for_db(td::Bits256 hash, td::optional<TorrentMeta> meta) {
  if (meta) {
    db_store_torrent_meta(hash, meta.unwrap());
  }
  alarm_timestamp() = td::Timestamp::in(1.0);
}

void StorageManager::add_torrent(Torrent torrent, bool start_download, td::Promise<td::Unit> promise) {
  td::Bits256 hash = torrent.get_hash();
  td::optional<TorrentMeta> meta;
  if (torrent.inited_info()) {
    meta = torrent.get_meta();
  }
  TRY_STATUS_PROMISE(promise, add_torrent_impl(std::move(torrent), start_download));
  db_store_torrent_list();
  db_store_torrent_short(torrents_[hash]);
  if (meta) {
    db_store_torrent_meta(hash, meta.unwrap());
  }
  promise.set_result(td::Unit());
}

td::Status StorageManager::add_torrent_impl(Torrent torrent, bool start_download) {
  td::Bits256 hash = torrent.get_hash();
  if (torrents_.count(hash)) {
    return td::Status::Error("Cannot add torrent: duplicate hash");
  }
  TorrentEntry& entry = torrents_[hash];
  entry.hash = hash;
  entry.root_dir = torrent.get_root_dir();
  entry.active_download = start_download;
  td::BufferSlice hash_str(hash.as_slice());
  overlay::OverlayIdFull overlay_id(std::move(hash_str));
  entry.peer_manager =
      td::actor::create_actor<PeerManager>("PeerManager", local_id_, std::move(overlay_id), overlays_, adnl_, rldp_);
  auto context = PeerManager::create_callback(entry.peer_manager.get());
  LOG(INFO) << "Added torrent " << hash.to_hex() << " , root_dir = " << torrent.get_root_dir();
  entry.actor = td::actor::create_actor<NodeActor>("Node", 1, std::move(torrent), std::move(context), start_download);
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
  if (entry->active_download != active) {
    entry->active_download = active;
    td::actor::send_closure(entry->actor, &NodeActor::set_should_download, active);
    db_store_torrent_short(*entry);
  }
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

std::vector<td::Bits256> StorageManager::db_load_torrent_list() {
  auto R = db_get<ton_api::storage_daemon_db_torrentList>(
      db_, create_hash_tl_object<ton_api::storage_daemon_db_key_torrentList>());
  if (R.is_error()) {
    LOG(ERROR) << "Failed to read torrent list from db: " << R.move_as_error();
    return {};
  }
  auto f = R.move_as_ok();
  return f == nullptr ? std::vector<td::Bits256>() : std::move(f->torrents_);
}

void StorageManager::db_store_torrent_list() {
  std::vector<td::Bits256> torrents;
  for (const auto& p : torrents_) {
    torrents.push_back(p.first);
  }
  auto S = db_->set(create_hash_tl_object<ton_api::storage_daemon_db_key_torrentList>().as_slice(),
                    create_serialize_tl_object<ton_api::storage_daemon_db_torrentList>(std::move(torrents)));
  if (S.is_error()) {
    LOG(ERROR) << "Failed to save torrent list to db: " << S;
  }
}

void StorageManager::db_store_torrent_short(const StorageManager::TorrentEntry& entry) {
  auto S = db_->set(
      create_hash_tl_object<ton_api::storage_daemon_db_key_torrentShort>(entry.hash).as_slice(),
      create_serialize_tl_object<ton_api::storage_daemon_db_torrentShort>(entry.root_dir, entry.active_download));
  if (S.is_error()) {
    LOG(ERROR) << "Failed to save torrent " << entry.hash.to_hex() << " to db: " << S;
  }
}

void StorageManager::db_store_torrent_meta(td::Bits256 hash, TorrentMeta meta) {
  auto S =
      db_->set(create_hash_tl_object<ton_api::storage_daemon_db_key_torrentMeta>(hash).as_slice(), meta.serialize());
  if (S.is_error()) {
    LOG(ERROR) << "Failed to save torrent meta of " << hash.to_hex() << " to db: " << S;
  }
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
