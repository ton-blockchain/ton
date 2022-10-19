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

#include "td/actor/actor.h"
#include "adnl/adnl.h"
#include "rldp2/rldp.h"
#include "overlay/overlays.h"
#include "storage/PeerManager.h"
#include "td/db/KeyValue.h"

using namespace ton;

class StorageManager : public td::actor::Actor {
 public:
  StorageManager(adnl::AdnlNodeIdShort local_id, std::string db_root, td::actor::ActorId<adnl::Adnl> adnl,
                 td::actor::ActorId<ton_rldp::Rldp> rldp, td::actor::ActorId<overlay::Overlays> overlays);

  void start_up() override;
  void alarm() override;

  void add_torrent(Torrent torrent, bool start_download, td::Promise<td::Unit> promise);
  void add_torrent_by_meta(TorrentMeta meta, std::string root_dir, bool start_download, td::Promise<td::Unit> promise);
  void add_torrent_by_hash(td::Bits256 hash, std::string root_dir, bool start_download, td::Promise<td::Unit> promise);

  void set_active_download(td::Bits256 hash, bool active, td::Promise<td::Unit> promise);

  void with_torrent(td::Bits256 hash, td::Promise<NodeActor::NodeState> promise);
  void get_all_torrents(td::Promise<std::vector<td::Bits256>> promise);

  void set_all_files_priority(td::Bits256 hash, td::uint8 priority, td::Promise<bool> promise);
  void set_file_priority_by_idx(td::Bits256 hash, size_t idx, td::uint8 priority, td::Promise<bool> promise);
  void set_file_priority_by_name(td::Bits256 hash, std::string name, td::uint8 priority, td::Promise<bool> promise);

 private:
  adnl::AdnlNodeIdShort local_id_;
  std::string db_root_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<ton_rldp::Rldp> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;

  std::shared_ptr<td::KeyValue> db_;
  td::Bits256 save_meta_ptr_ = td::Bits256::zero();

  struct TorrentEntry {
    td::Bits256 hash;
    std::string root_dir;
    bool active_download;
    td::actor::ActorOwn<NodeActor> actor;
    td::actor::ActorOwn<PeerManager> peer_manager;
  };

  std::map<td::Bits256, TorrentEntry> torrents_;

  td::Status add_torrent_impl(Torrent torrent, bool start_download);

  td::Result<TorrentEntry*> get_torrent(td::Bits256 hash) {
    auto it = torrents_.find(hash);
    if (it == torrents_.end()) {
      return td::Status::Error("No such torrent");
    }
    return &it->second;
  }

  void got_torrent_meta_for_db(td::Bits256 hash, td::optional<TorrentMeta> meta);

  std::vector<td::Bits256> db_load_torrent_list();
  void db_store_torrent_list();
  void db_store_torrent_short(const TorrentEntry& entry);
  void db_store_torrent_meta(td::Bits256 hash, TorrentMeta meta);
};