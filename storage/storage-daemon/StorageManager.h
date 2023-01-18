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

#pragma once
#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "rldp2/rldp.h"
#include "overlay/overlays.h"
#include "storage/PeerManager.h"
#include "storage/db.h"

namespace ton {

class StorageManager : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void on_ready() = 0;
  };

  StorageManager(adnl::AdnlNodeIdShort local_id, std::string db_root, td::unique_ptr<Callback> callback,
                 bool client_mode, td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<ton_rldp::Rldp> rldp,
                 td::actor::ActorId<overlay::Overlays> overlays);

  void start_up() override;

  void add_torrent(Torrent torrent, bool start_download, bool allow_upload, bool copy_inside,
                   td::Promise<td::Unit> promise);
  void add_torrent_by_meta(TorrentMeta meta, std::string root_dir, bool start_download, bool allow_upload,
                           td::Promise<td::Unit> promise);
  void add_torrent_by_hash(td::Bits256 hash, std::string root_dir, bool start_download, bool allow_upload,
                           td::Promise<td::Unit> promise);

  void set_active_download(td::Bits256 hash, bool active, td::Promise<td::Unit> promise);
  void set_active_upload(td::Bits256 hash, bool active, td::Promise<td::Unit> promise);

  void with_torrent(td::Bits256 hash, td::Promise<NodeActor::NodeState> promise);
  void get_all_torrents(td::Promise<std::vector<td::Bits256>> promise);

  void set_all_files_priority(td::Bits256 hash, td::uint8 priority, td::Promise<bool> promise);
  void set_file_priority_by_idx(td::Bits256 hash, size_t idx, td::uint8 priority, td::Promise<bool> promise);
  void set_file_priority_by_name(td::Bits256 hash, std::string name, td::uint8 priority, td::Promise<bool> promise);

  void remove_torrent(td::Bits256 hash, bool remove_files, td::Promise<td::Unit> promise);
  void load_from(td::Bits256 hash, td::optional<TorrentMeta> meta, std::string files_path,
                 td::Promise<td::Unit> promise);

  void wait_for_completion(td::Bits256 hash, td::Promise<td::Unit> promise);
  void get_peers_info(td::Bits256 hash, td::Promise<tl_object_ptr<ton_api::storage_daemon_peerList>> promise);

 private:
  adnl::AdnlNodeIdShort local_id_;
  std::string db_root_;
  td::unique_ptr<Callback> callback_;
  bool client_mode_ = false;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<ton_rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  std::shared_ptr<db::DbType> db_;

  struct TorrentEntry {
    td::Bits256 hash;
    td::actor::ActorOwn<NodeActor> actor;
    td::actor::ActorOwn<PeerManager> peer_manager;

    struct ClosingState {
      bool removing = false;
      td::Promise<td::Unit> promise;
      bool remove_files = false;
    };
    std::shared_ptr<ClosingState> closing_state = std::make_shared<ClosingState>();
  };

  std::map<td::Bits256, TorrentEntry> torrents_;

  td::Status add_torrent_impl(Torrent torrent, bool start_download, bool allow_upload);

  td::Result<TorrentEntry*> get_torrent(td::Bits256 hash) {
    auto it = torrents_.find(hash);
    if (it == torrents_.end()) {
      return td::Status::Error("No such torrent");
    }
    return &it->second;
  }

  td::unique_ptr<NodeActor::Callback> create_callback(td::Bits256 hash,
                                                      std::shared_ptr<TorrentEntry::ClosingState> closing_state);

  void load_torrents_from_db(std::vector<td::Bits256> torrents);
  void loaded_torrent_from_db(td::Bits256 hash, td::Result<td::actor::ActorOwn<NodeActor>> R);
  void after_load_torrents_from_db();
  void db_store_torrent_list();

  void on_torrent_closed(Torrent torrent, std::shared_ptr<TorrentEntry::ClosingState> closing_state);
};

}  // namespace ton
