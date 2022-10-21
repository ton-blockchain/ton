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
#include "td/utils/filesystem.h"
#include "td/actor/actor.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/user.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Random.h"
#include "td/utils/FileLog.h"
#include "checksum.h"
#include "git.h"
#include "auto/tl/ton_api_json.h"

#include "adnl/adnl.h"
#include "rldp2/rldp.h"
#include "dht/dht.h"
#include "overlay/overlays.h"

#include "Torrent.h"
#include "TorrentCreator.h"
#include "StorageManager.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>

using namespace ton;

td::BufferSlice create_query_error(td::CSlice message) {
  return create_serialize_tl_object<ton_api::storage_daemon_queryError>(message.str());
}

td::BufferSlice create_query_error(td::Status error) {
  return create_query_error(error.message());
}

class StorageDaemon : public td::actor::Actor {
 public:
  StorageDaemon(td::IPAddress ip_addr, std::string global_config, std::string db_root, td::uint16 control_port)
      : ip_addr_(ip_addr)
      , global_config_(std::move(global_config))
      , db_root_(std::move(db_root))
      , control_port_(control_port) {
  }

  void start_up() override {
    CHECK(db_root_ != "");
    td::mkdir(db_root_).ensure();
    keyring_ = keyring::Keyring::create(db_root_ + "/keyring");
    {
      auto S = load_global_config();
      if (S.is_error()) {
        LOG(ERROR) << "Failed t load global config: " << S;
        std::exit(2);
      }
    }
    init_adnl();

    class Callback : public StorageManager::Callback {
     public:
      explicit Callback(td::actor::ActorId<StorageDaemon> actor) : actor_(std::move(actor)) {
      }
      void on_ready() override {
        td::actor::send_closure(actor_, &StorageDaemon::init_control_interface);
      }

     private:
      td::actor::ActorId<StorageDaemon> actor_;
    };
    manager_ = td::actor::create_actor<StorageManager>("storage", local_id_, db_root_ + "/torrent",
                                                       td::make_unique<Callback>(actor_id(this)), adnl_.get(),
                                                       rldp_.get(), overlays_.get());
  }

  td::Status load_global_config() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");
    ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");
    if (!conf.dht_) {
      return td::Status::Error(ErrorCode::error, "does not contain [dht] section");
    }
    TRY_RESULT_PREFIX(dht, dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
    dht_config_ = std::move(dht);
    return td::Status::OK();
  }

  void init_adnl() {
    CHECK(ip_addr_.is_valid());

    adnl_network_manager_ = adnl::AdnlNetworkManager::create(static_cast<td::uint16>(ip_addr_.get_port()));
    adnl_ = adnl::Adnl::create(db_root_, keyring_.get());
    td::actor::send_closure(adnl_, &adnl::Adnl::register_network_manager, adnl_network_manager_.get());
    adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(adnl_network_manager_, &adnl::AdnlNetworkManager::add_self_addr, ip_addr_,
                            std::move(cat_mask), 0);

    adnl::AdnlAddressList addr_list;
    adnl::AdnlAddress addr = adnl::AdnlAddressImpl::create(
        create_tl_object<ton_api::adnl_address_udp>(ip_addr_.get_ipv4(), ip_addr_.get_port()));
    addr_list.add_addr(std::move(addr));
    addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    addr_list.set_reinit_date(adnl::Adnl::adnl_start_time());
    auto generate_adnl_id = [&]() -> adnl::AdnlNodeIdShort {
      auto pk = PrivateKey{privkeys::Ed25519::random()};
      auto pub = pk.compute_public_key();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), true, [](td::Unit) {});
      auto adnl_id = adnl::AdnlNodeIdShort{pub.compute_short_id()};
      td::actor::send_closure(adnl_, &adnl::Adnl::add_id, adnl::AdnlNodeIdFull{pub}, addr_list,
                              static_cast<td::uint8>(0));
      return adnl_id;
    };
    local_id_ = generate_adnl_id();
    dht_id_ = generate_adnl_id();
    auto D = dht::Dht::create(dht_id_, db_root_, dht_config_, keyring_.get(), adnl_.get());
    D.ensure();
    dht_ = D.move_as_ok();
    td::actor::send_closure(adnl_, &adnl::Adnl::register_dht_node, dht_.get());

    rldp_ = ton_rldp::Rldp::create(adnl_.get());
    td::actor::send_closure(rldp_, &ton_rldp::Rldp::add_id, local_id_);
    overlays_ = overlay::Overlays::create(db_root_, keyring_.get(), adnl_.get(), dht_.get());
  }

  void init_control_interface() {
    if (control_port_ == 0) {
      return;
    }

    // Hardcoded adnl id
    // TODO: something else
    auto pk = PrivateKey{privkeys::Ed25519(td::sha256_bits256("storage-daemon-control"))};
    auto pub = pk.compute_public_key();
    td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), true, [](td::Unit) {});
    auto adnl_id = adnl::AdnlNodeIdShort{pub.compute_short_id()};
    td::actor::send_closure(adnl_, &adnl::Adnl::add_id, adnl::AdnlNodeIdFull{pub}, adnl::AdnlAddressList(),
                            static_cast<td::uint8>(255));

    class Callback : public adnl::Adnl::Callback {
     public:
      explicit Callback(td::actor::ActorId<StorageDaemon> id) : self_id_(id) {
      }
      void receive_message(adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(self_id_, &StorageDaemon::process_control_query, std::move(data), std::move(promise));
      }

     private:
      td::actor::ActorId<StorageDaemon> self_id_;
    };
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, adnl_id, "", std::make_unique<Callback>(actor_id(this)));
    td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector<adnl::AdnlNodeIdShort>{adnl_id},
                            std::vector<td::uint16>{control_port_},
                            [SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
                              if (R.is_error()) {
                                LOG(ERROR) << "Failed to init control interface: " << R.move_as_error();
                                return;
                              }
                              td::actor::send_closure(SelfId, &StorageDaemon::created_ext_server, R.move_as_ok());
                            });
  }

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> ext_server) {
    ext_server_ = std::move(ext_server);
    LOG(INFO) << "Started control interface on port " << control_port_;
  }

  void process_control_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
    promise = [promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        promise.set_value(create_query_error(R.move_as_error()));
      } else {
        promise.set_value(R.move_as_ok());
      }
    };
    auto F = fetch_tl_object<ton_api::Function>(data, true);
    if (F.is_error()) {
      promise.set_error(F.move_as_error_prefix("failed to parse control query: "));
      return;
    }
    auto f = F.move_as_ok();
    ton_api::downcast_call(*f, [&](auto &obj) { run_control_query(obj, std::move(promise)); });
  }

  void run_control_query(ton_api::storage_daemon_setVerbosity &query, td::Promise<td::BufferSlice> promise) {
    if (query.verbosity_ < 0 || query.verbosity_ > 10) {
      promise.set_value(create_query_error("verbosity should be in range [0..10]"));
      return;
    }
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(ERROR) + query.verbosity_);
    promise.set_result(create_serialize_tl_object<ton_api::storage_daemon_success>());
  }

  void run_control_query(ton_api::storage_daemon_createTorrent &query, td::Promise<td::BufferSlice> promise) {
    Torrent::Creator::Options options;
    options.piece_size = 128 * 1024;
    options.description = std::move(query.description_);
    TRY_RESULT_PROMISE(promise, torrent, Torrent::Creator::create_from_path(std::move(options), query.path_));
    td::Bits256 hash = torrent.get_hash();
    td::actor::send_closure(
        manager_, &StorageManager::add_torrent, std::move(torrent), false,
        [manager = manager_.get(), hash, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            get_torrent_info_full_serialized(manager, hash, std::move(promise));
          }
        });
  }

  void run_control_query(ton_api::storage_daemon_addByHash &query, td::Promise<td::BufferSlice> promise) {
    td::Bits256 hash = query.hash_;
    td::actor::send_closure(
        manager_, &StorageManager::add_torrent_by_hash, hash, std::move(query.root_dir_), query.start_download_,
        [manager = manager_.get(), hash, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            get_torrent_info_full_serialized(manager, hash, std::move(promise));
          }
        });
  }

  void run_control_query(ton_api::storage_daemon_addByMeta &query, td::Promise<td::BufferSlice> promise) {
    TRY_RESULT_PROMISE(promise, meta, TorrentMeta::deserialize(query.meta_));
    td::Bits256 hash(meta.info.get_hash().bits());
    td::actor::send_closure(
        manager_, &StorageManager::add_torrent_by_meta, std::move(meta), std::move(query.root_dir_),
        query.start_download_,
        [manager = manager_.get(), hash, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            get_torrent_info_full_serialized(manager, hash, std::move(promise));
          }
        });
  }

  void run_control_query(ton_api::storage_daemon_setActiveDownload &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(
        manager_, &StorageManager::set_active_download, query.hash_, query.active_,
        promise.wrap([](td::Unit &&) { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  void run_control_query(ton_api::storage_daemon_getTorrents &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(
        manager_, &StorageManager::get_all_torrents,
        [manager = manager_.get(), promise = std::move(promise)](td::Result<std::vector<td::Bits256>> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
            return;
          }
          std::vector<td::Bits256> torrents = R.move_as_ok();
          auto result = std::make_shared<std::vector<tl_object_ptr<ton_api::storage_daemon_torrent>>>(torrents.size());
          td::MultiPromise mp;
          auto ig = mp.init_guard();
          for (size_t i = 0; i < torrents.size(); ++i) {
            get_torrent_info_short(manager, torrents[i],
                                   [i, result, promise = ig.get_promise()](
                                       td::Result<tl_object_ptr<ton_api::storage_daemon_torrent>> R) mutable {
                                     if (R.is_ok()) {
                                       result->at(i) = R.move_as_ok();
                                     }
                                     promise.set_result(td::Unit());
                                   });
          }
          ig.add_promise([promise = std::move(promise), result](td::Result<td::Unit> R) mutable {
            if (R.is_error()) {
              promise.set_error(R.move_as_error());
              return;
            }
            auto v = std::move(*result);
            v.erase(std::remove(v.begin(), v.end(), nullptr), v.end());
            promise.set_result(create_serialize_tl_object<ton_api::storage_daemon_torrentList>(std::move(v)));
          });
        });
  }

  void run_control_query(ton_api::storage_daemon_getTorrentFull &query, td::Promise<td::BufferSlice> promise) {
    get_torrent_info_full_serialized(manager_.get(), query.hash_, std::move(promise));
  }

  void run_control_query(ton_api::storage_daemon_getTorrentMeta &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(
        manager_, &StorageManager::with_torrent, query.hash_,
        promise.wrap([](NodeActor::NodeState state) -> td::Result<td::BufferSlice> {
          Torrent &torrent = state.torrent;
          if (!torrent.inited_info()) {
            return td::Status::Error("Torrent meta is not available");
          }
          std::string meta_str = torrent.get_meta(Torrent::GetMetaOptions().with_proof_depth_limit(10)).serialize();
          return create_serialize_tl_object<ton_api::storage_daemon_torrentMeta>(td::BufferSlice(meta_str));
        }));
  }

  void run_control_query(ton_api::storage_daemon_setFilePriorityAll &query, td::Promise<td::BufferSlice> promise) {
    TRY_RESULT_PROMISE(promise, priority, td::narrow_cast_safe<td::uint8>(query.priority_));
    td::actor::send_closure(manager_, &StorageManager::set_all_files_priority, query.hash_, priority,
                            promise.wrap([](bool done) -> td::Result<td::BufferSlice> {
                              if (done) {
                                return create_serialize_tl_object<ton_api::storage_daemon_prioritySet>();
                              } else {
                                return create_serialize_tl_object<ton_api::storage_daemon_priorityPending>();
                              }
                            }));
  }

  void run_control_query(ton_api::storage_daemon_setFilePriorityByIdx &query, td::Promise<td::BufferSlice> promise) {
    TRY_RESULT_PROMISE(promise, priority, td::narrow_cast_safe<td::uint8>(query.priority_));
    td::actor::send_closure(manager_, &StorageManager::set_file_priority_by_idx, query.hash_, query.idx_, priority,
                            promise.wrap([](bool done) -> td::Result<td::BufferSlice> {
                              if (done) {
                                return create_serialize_tl_object<ton_api::storage_daemon_prioritySet>();
                              } else {
                                return create_serialize_tl_object<ton_api::storage_daemon_priorityPending>();
                              }
                            }));
  }

  void run_control_query(ton_api::storage_daemon_setFilePriorityByName &query, td::Promise<td::BufferSlice> promise) {
    TRY_RESULT_PROMISE(promise, priority, td::narrow_cast_safe<td::uint8>(query.priority_));
    td::actor::send_closure(manager_, &StorageManager::set_file_priority_by_name, query.hash_, std::move(query.name_),
                            priority, promise.wrap([](bool done) -> td::Result<td::BufferSlice> {
                              if (done) {
                                return create_serialize_tl_object<ton_api::storage_daemon_prioritySet>();
                              } else {
                                return create_serialize_tl_object<ton_api::storage_daemon_priorityPending>();
                              }
                            }));
  }

  template <class T>
  void run_control_query(T &query, td::Promise<td::BufferSlice> promise) {
    promise.set_error(td::Status::Error("unknown query"));
  }

 private:
  static void fill_torrent_info_short(Torrent &torrent, ton_api::storage_daemon_torrent &obj) {
    obj.hash_ = torrent.get_hash();
    obj.root_dir_ = torrent.get_root_dir();
    if (torrent.inited_info()) {
      const Torrent::Info &info = torrent.get_info();
      obj.info_ready_ = true;
      obj.header_ready_ = torrent.inited_header();
      obj.total_size_ = info.file_size;
      obj.description_ = info.description;
      if (obj.header_ready_) {
        obj.included_size_ = torrent.get_included_size();
        auto count = torrent.get_files_count();
        obj.files_count_ = count ? count.value() : 0;
      } else {
        obj.files_count_ = 0;
      }
      obj.downloaded_size_ = torrent.get_included_ready_size();
      obj.completed_ = torrent.is_completed();
    } else {
      obj.info_ready_ = false;
      obj.header_ready_ = false;
      obj.total_size_ = 0;
      obj.included_size_ = 0;
      obj.description_ = "";
      obj.files_count_ = 0;
      obj.downloaded_size_ = 0.0;
      obj.completed_ = false;
    }
  }

  static void fill_torrent_info_full(Torrent &torrent, ton_api::storage_daemon_torrentFull &obj) {
    if (!obj.torrent_) {
      obj.torrent_ = create_tl_object<ton_api::storage_daemon_torrent>();
    }
    fill_torrent_info_short(torrent, *obj.torrent_);
    obj.files_.clear();
    auto count = torrent.get_files_count();
    if (!count) {
      return;
    }
    for (size_t i = 0; i < count.value(); ++i) {
      auto file = create_tl_object<ton_api::storage_daemon_fileInfo>();
      file->name_ = torrent.get_file_name(i).str();
      file->size_ = torrent.get_file_size(i);
      file->downloaded_size_ = torrent.get_file_ready_size(i);
      obj.files_.push_back(std::move(file));
    }
  }

  static void get_torrent_info_short(td::actor::ActorId<StorageManager> manager, td::Bits256 hash,
                                     td::Promise<tl_object_ptr<ton_api::storage_daemon_torrent>> promise) {
    td::actor::send_closure(manager, &StorageManager::with_torrent, hash,
                            [promise = std::move(promise)](td::Result<NodeActor::NodeState> R) mutable {
                              if (R.is_error()) {
                                promise.set_result(R.move_as_error());
                                return;
                              }
                              auto state = R.move_as_ok();
                              auto obj = create_tl_object<ton_api::storage_daemon_torrent>();
                              fill_torrent_info_short(state.torrent, *obj);
                              obj->active_download_ = state.active_download;
                              obj->download_speed_ = state.download_speed;
                              obj->upload_speed_ = state.upload_speed;
                              promise.set_result(std::move(obj));
                            });
  }

  static void get_torrent_info_full_serialized(td::actor::ActorId<StorageManager> manager, td::Bits256 hash,
                                               td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(manager, &StorageManager::with_torrent, hash,
                            [promise = std::move(promise)](td::Result<NodeActor::NodeState> R) mutable {
                              if (R.is_error()) {
                                promise.set_error(R.move_as_error());
                              } else {
                                auto state = R.move_as_ok();
                                auto obj = create_tl_object<ton_api::storage_daemon_torrentFull>();
                                fill_torrent_info_full(state.torrent, *obj);
                                obj->torrent_->active_download_ = state.active_download;
                                obj->torrent_->download_speed_ = state.download_speed;
                                obj->torrent_->upload_speed_ = state.upload_speed;
                                for (size_t i = 0; i < obj->files_.size(); ++i) {
                                  obj->files_[i]->priority_ =
                                      (i < state.file_priority.size() ? state.file_priority[i] : 1);
                                }
                                promise.set_result(serialize_tl_object(obj, true));
                              }
                            });
  }

  td::IPAddress ip_addr_;
  std::string global_config_;
  std::string db_root_;
  td::uint16 control_port_;

  std::shared_ptr<dht::DhtGlobalConfig> dht_config_;
  adnl::AdnlNodeIdShort local_id_;
  adnl::AdnlNodeIdShort dht_id_;

  td::actor::ActorOwn<keyring::Keyring> keyring_;
  td::actor::ActorOwn<adnl::AdnlNetworkManager> adnl_network_manager_;
  td::actor::ActorOwn<adnl::Adnl> adnl_;
  td::actor::ActorOwn<dht::Dht> dht_;
  td::actor::ActorOwn<ton_rldp::Rldp> rldp_;
  td::actor::ActorOwn<overlay::Overlays> overlays_;
  td::actor::ActorOwn<adnl::AdnlExtServer> ext_server_;

  td::actor::ActorOwn<StorageManager> manager_;
};

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::IPAddress ip_addr;
  std::string global_config, db_root;
  td::uint16 control_port = 0;

  td::OptionParser p;
  p.set_description("Server for seeding and downloading torrents\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('V', "version", "shows storage-daemon build information", [&]() {
    std::cout << "storage-daemon build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('h', "help", "prints a help message", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option('I', "ip", "set <ip>:<port> for adnl", [&](td::Slice arg) -> td::Status {
    TRY_STATUS(ip_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });
  p.add_checked_option('p', "control-port", "port for control interface", [&](td::Slice arg) -> td::Status {
    TRY_RESULT_ASSIGN(control_port, td::to_integer_safe<td::uint16>(arg));
    return td::Status::OK();
  });
  p.add_option('C', "global-config", "global TON configuration file",
               [&](td::Slice arg) { global_config = arg.str(); });
  p.add_option('D', "db", "db root", [&](td::Slice arg) { db_root = arg.str(); });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
    td::set_signal_handler(td::SignalType::HangUp, [](int sig) {
#if TD_DARWIN || TD_LINUX
      close(0);
      setsid();
#endif
    }).ensure();
  });
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    logger_ = td::FileLog::create(fname.str()).move_as_ok();
    td::log_interface = logger_.get();
  });

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] {
    p.run(argc, argv).ensure();
    td::actor::create_actor<StorageDaemon>("storage-daemon", ip_addr, global_config, db_root, control_port).release();
  });
  while (scheduler.run(1)) {
  }
}
