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
#include "common/delay.h"

#include "adnl/adnl.h"
#include "rldp2/rldp.h"
#include "dht/dht.h"
#include "overlay/overlays.h"

#include "Torrent.h"
#include "TorrentCreator.h"
#include "StorageManager.h"
#include "StorageProvider.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>

namespace ton {

td::BufferSlice create_query_error(td::CSlice message) {
  return create_serialize_tl_object<ton_api::storage_daemon_queryError>(message.str());
}

td::BufferSlice create_query_error(td::Status error) {
  return create_query_error(error.message());
}

class StorageDaemon : public td::actor::Actor {
 public:
  StorageDaemon(td::IPAddress ip_addr, bool client_mode, std::string global_config, std::string db_root,
                td::uint16 control_port, bool enable_storage_provider)
      : ip_addr_(ip_addr)
      , client_mode_(client_mode)
      , global_config_(std::move(global_config))
      , db_root_(std::move(db_root))
      , control_port_(control_port)
      , enable_storage_provider_(enable_storage_provider) {
  }

  void start_up() override {
    CHECK(db_root_ != "");
    td::mkdir(db_root_).ensure();
    db_root_ = td::realpath(db_root_).move_as_ok();
    keyring_ = keyring::Keyring::create(db_root_ + "/keyring");
    {
      auto S = load_global_config();
      if (S.is_error()) {
        LOG(FATAL) << "Failed to load global config: " << S;
      }
    }
    {
      auto S = load_daemon_config();
      if (S.is_error()) {
        LOG(FATAL) << "Failed to load daemon config: " << S;
      }
    }

    init_adnl();

    class Callback : public StorageManager::Callback {
     public:
      explicit Callback(td::actor::ActorId<StorageDaemon> actor) : actor_(std::move(actor)) {
      }
      void on_ready() override {
        td::actor::send_closure(actor_, &StorageDaemon::inited_storage_manager);
      }

     private:
      td::actor::ActorId<StorageDaemon> actor_;
    };
    manager_ = td::actor::create_actor<StorageManager>("storage", local_id_, db_root_ + "/torrent",
                                                       td::make_unique<Callback>(actor_id(this)), client_mode_,
                                                       adnl_.get(), rldp_.get(), overlays_.get());
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

  td::Status load_daemon_config() {
    daemon_config_ = create_tl_object<ton_api::storage_daemon_config>();
    auto r_conf_data = td::read_file(daemon_config_file());
    if (r_conf_data.is_ok()) {
      auto conf_data = r_conf_data.move_as_ok();
      TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");
      TRY_STATUS_PREFIX(ton_api::from_json(*daemon_config_, conf_json.get_object()), "json does not fit TL scheme: ");
      return td::Status::OK();
    }
    std::string keys_dir = db_root_ + "/cli-keys/";
    LOG(INFO) << "First launch, storing keys for storage-daemon-cli to " << keys_dir;
    td::mkdir(keys_dir).ensure();
    auto generate_public_key = [&]() -> PublicKey {
      auto pk = PrivateKey{privkeys::Ed25519::random()};
      auto pub = pk.compute_public_key();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), false, [](td::Unit) {});
      return pub;
    };
    {
      // Server key
      daemon_config_->server_key_ = generate_public_key().tl();
      TRY_STATUS(td::write_file(keys_dir + "server.pub", serialize_tl_object(daemon_config_->server_key_, true)));
    }
    {
      // Client key
      auto pk = PrivateKey{privkeys::Ed25519::random()};
      daemon_config_->cli_key_hash_ = pk.compute_short_id().bits256_value();
      TRY_STATUS(td::write_file(keys_dir + "client", serialize_tl_object(pk.tl(), true)));
    }
    daemon_config_->adnl_id_ = generate_public_key().tl();
    daemon_config_->dht_id_ = generate_public_key().tl();
    return save_daemon_config();
  }

  td::Status save_daemon_config() {
    auto s = td::json_encode<std::string>(td::ToJson(*daemon_config_), true);
    TRY_STATUS_PREFIX(td::write_file(daemon_config_file(), s), "Failed to write daemon config: ");
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
    if (!client_mode_) {
      addr_list.add_udp_address(ip_addr_).ensure();
    }
    addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    addr_list.set_reinit_date(adnl::Adnl::adnl_start_time());

    adnl::AdnlNodeIdFull local_id_full = adnl::AdnlNodeIdFull::create(daemon_config_->adnl_id_).move_as_ok();
    local_id_ = local_id_full.compute_short_id();
    td::actor::send_closure(adnl_, &adnl::Adnl::add_id, local_id_full, addr_list, static_cast<td::uint8>(0));
    adnl::AdnlNodeIdFull dht_id_full = adnl::AdnlNodeIdFull::create(daemon_config_->dht_id_).move_as_ok();
    dht_id_ = dht_id_full.compute_short_id();
    td::actor::send_closure(adnl_, &adnl::Adnl::add_id, dht_id_full, addr_list, static_cast<td::uint8>(0));

    if (client_mode_) {
      auto D = dht::Dht::create_client(dht_id_, db_root_, dht_config_, keyring_.get(), adnl_.get());
      D.ensure();
      dht_ = D.move_as_ok();
    } else {
      auto D = dht::Dht::create(dht_id_, db_root_, dht_config_, keyring_.get(), adnl_.get());
      D.ensure();
      dht_ = D.move_as_ok();
    }
    td::actor::send_closure(adnl_, &adnl::Adnl::register_dht_node, dht_.get());

    rldp_ = ton_rldp::Rldp::create(adnl_.get());
    td::actor::send_closure(rldp_, &ton_rldp::Rldp::add_id, local_id_);
    overlays_ = overlay::Overlays::create(db_root_, keyring_.get(), adnl_.get(), dht_.get());
  }

  void inited_storage_manager() {
    if (enable_storage_provider_) {
      if (!daemon_config_->provider_address_.empty()) {
        auto provider_account = ContractAddress::parse(daemon_config_->provider_address_).move_as_ok();
        init_tonlib_client();
        provider_ = td::actor::create_actor<StorageProvider>("provider", provider_account, db_root_ + "/provider",
                                                             tonlib_client_.get(), manager_.get(), keyring_.get());
      } else {
        LOG(WARNING) << "Storage provider account is not set, it can be set in storage-daemon-cli";
      }
    }
    init_control_interface();
  }

  void init_control_interface() {
    if (control_port_ == 0) {
      return;
    }

    auto adnl_id_full = adnl::AdnlNodeIdFull::create(daemon_config_->server_key_).move_as_ok();
    auto adnl_id = adnl_id_full.compute_short_id();
    td::actor::send_closure(adnl_, &adnl::Adnl::add_id, adnl_id_full, adnl::AdnlAddressList(),
                            static_cast<td::uint8>(255));

    class Callback : public adnl::Adnl::Callback {
     public:
      explicit Callback(td::actor::ActorId<StorageDaemon> id) : self_id_(id) {
      }
      void receive_message(adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(self_id_, &StorageDaemon::process_control_query, src, std::move(data),
                                std::move(promise));
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

  void process_control_query(adnl::AdnlNodeIdShort src, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
    promise = [promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        promise.set_value(create_query_error(R.move_as_error()));
      } else {
        promise.set_value(R.move_as_ok());
      }
    };
    if (src.bits256_value() != daemon_config_->cli_key_hash_) {
      promise.set_error(td::Status::Error("Not authorized"));
      return;
    }
    auto F = fetch_tl_object<ton_api::Function>(data, true);
    if (F.is_error()) {
      promise.set_error(F.move_as_error_prefix("failed to parse control query: "));
      return;
    }
    auto f = F.move_as_ok();
    LOG(DEBUG) << "Running control query " << f->get_id();
    ton_api::downcast_call(*f, [&](auto &obj) { run_control_query(obj, std::move(promise)); });
  }

  void run_control_query(ton_api::storage_daemon_setVerbosity &query, td::Promise<td::BufferSlice> promise) {
    if (query.verbosity_ < 0 || query.verbosity_ > 10) {
      promise.set_value(create_query_error("verbosity should be in range [0..10]"));
      return;
    }
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + query.verbosity_);
    promise.set_result(create_serialize_tl_object<ton_api::storage_daemon_success>());
  }

  void run_control_query(ton_api::storage_daemon_createTorrent &query, td::Promise<td::BufferSlice> promise) {
    // Run in a separate thread
    delay_action(
        [promise = std::move(promise), manager = manager_.get(), db_root = db_root_,
         query = std::move(query)]() mutable {
          Torrent::Creator::Options options;
          options.piece_size = 128 * 1024;
          options.description = std::move(query.description_);
          TRY_RESULT_PROMISE(promise, torrent, Torrent::Creator::create_from_path(std::move(options), query.path_));
          td::Bits256 hash = torrent.get_hash();
          td::actor::send_closure(manager, &StorageManager::add_torrent, std::move(torrent), false, query.allow_upload_,
                                  query.copy_inside_,
                                  [manager, hash, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
                                    if (R.is_error()) {
                                      promise.set_error(R.move_as_error());
                                      return;
                                    }
                                    get_torrent_info_full_serialized(manager, hash, std::move(promise));
                                  });
        },
        td::Timestamp::now());
  }

  void run_control_query(ton_api::storage_daemon_addByHash &query, td::Promise<td::BufferSlice> promise) {
    td::Bits256 hash = query.hash_;
    bool start_download_now = query.start_download_ && query.priorities_.empty();
    td::actor::send_closure(
        manager_, &StorageManager::add_torrent_by_hash, hash, std::move(query.root_dir_), start_download_now,
        query.allow_upload_,
        query_add_torrent_cont(hash, query.start_download_, std::move(query.priorities_), std::move(promise)));
  }

  void run_control_query(ton_api::storage_daemon_addByMeta &query, td::Promise<td::BufferSlice> promise) {
    TRY_RESULT_PROMISE(promise, meta, TorrentMeta::deserialize(query.meta_));
    td::Bits256 hash(meta.info.get_hash());
    bool start_download_now = query.start_download_ && query.priorities_.empty();
    td::actor::send_closure(
        manager_, &StorageManager::add_torrent_by_meta, std::move(meta), std::move(query.root_dir_), start_download_now,
        query.allow_upload_,
        query_add_torrent_cont(hash, query.start_download_, std::move(query.priorities_), std::move(promise)));
  }

  td::Promise<td::Unit> query_add_torrent_cont(td::Bits256 hash, bool start_download,
                                               std::vector<tl_object_ptr<ton_api::storage_PriorityAction>> priorities,
                                               td::Promise<td::BufferSlice> promise) {
    return [manager = manager_.get(), hash, start_download = start_download, priorities = std::move(priorities),
            promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
        return;
      }
      if (!priorities.empty()) {
        for (auto &p : priorities) {
          ton_api::downcast_call(
              *p, td::overloaded(
                      [&](ton_api::storage_priorityAction_all &obj) {
                        td::actor::send_closure(manager, &StorageManager::set_all_files_priority, hash,
                                                (td::uint8)obj.priority_, [](td::Result<bool>) {});
                      },
                      [&](ton_api::storage_priorityAction_idx &obj) {
                        td::actor::send_closure(manager, &StorageManager::set_file_priority_by_idx, hash, obj.idx_,
                                                (td::uint8)obj.priority_, [](td::Result<bool>) {});
                      },
                      [&](ton_api::storage_priorityAction_name &obj) {
                        td::actor::send_closure(manager, &StorageManager::set_file_priority_by_name, hash,
                                                std::move(obj.name_), (td::uint8)obj.priority_,
                                                [](td::Result<bool>) {});
                      }));
        }
        if (start_download) {
          td::actor::send_closure(manager, &StorageManager::set_active_download, hash, true,
                                  [](td::Result<td::Unit>) {});
        }
      }
      get_torrent_info_full_serialized(manager, hash, std::move(promise));
    };
  }

  void run_control_query(ton_api::storage_daemon_setActiveDownload &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(
        manager_, &StorageManager::set_active_download, query.hash_, query.active_,
        promise.wrap([](td::Unit &&) { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  void run_control_query(ton_api::storage_daemon_setActiveUpload &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(
        manager_, &StorageManager::set_active_upload, query.hash_, query.active_,
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

  void run_control_query(ton_api::storage_daemon_getTorrentPeers &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(manager_, &StorageManager::get_peers_info, query.hash_,
                            promise.wrap([](tl_object_ptr<ton_api::storage_daemon_peerList> obj) -> td::BufferSlice {
                              return serialize_tl_object(obj, true);
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

  void run_control_query(ton_api::storage_daemon_removeTorrent &query, td::Promise<td::BufferSlice> promise) {
    td::actor::send_closure(
        manager_, &StorageManager::remove_torrent, query.hash_, query.remove_files_,
        promise.wrap([](td::Unit &&) { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  void run_control_query(ton_api::storage_daemon_loadFrom &query, td::Promise<td::BufferSlice> promise) {
    td::optional<TorrentMeta> meta;
    if (!query.meta_.empty()) {
      TRY_RESULT_PROMISE_ASSIGN(promise, meta, TorrentMeta::deserialize(query.meta_));
    }
    td::actor::send_closure(
        manager_, &StorageManager::load_from, query.hash_, std::move(meta), std::move(query.path_),
        [manager = manager_.get(), hash = query.hash_, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            get_torrent_info_short(manager, hash, promise.wrap([](tl_object_ptr<ton_api::storage_daemon_torrent> obj) {
              return serialize_tl_object(obj, true);
            }));
          }
        });
  }

  void run_control_query(ton_api::storage_daemon_getNewContractMessage &query, td::Promise<td::BufferSlice> promise) {
    td::Promise<std::pair<td::RefInt256, td::uint32>> P =
        [promise = std::move(promise), hash = query.hash_, query_id = query.query_id_,
         manager = manager_.get()](td::Result<std::pair<td::RefInt256, td::uint32>> R) mutable {
          TRY_RESULT_PROMISE(promise, r, std::move(R));
          td::actor::send_closure(
              manager, &StorageManager::with_torrent, hash,
              promise.wrap([r = std::move(r), query_id](NodeActor::NodeState state) -> td::Result<td::BufferSlice> {
                Torrent &torrent = state.torrent;
                if (!torrent.is_completed()) {
                  return td::Status::Error("Torrent is not complete");
                }
                TRY_RESULT(microchunk_tree, MicrochunkTree::Builder::build_for_torrent(torrent, 1LL << 60));
                td::Ref<vm::Cell> msg = create_new_contract_message_body(
                    torrent.get_info().as_cell(), microchunk_tree.get_root_hash(), query_id, r.first, r.second);
                return create_serialize_tl_object<ton_api::storage_daemon_newContractMessage>(
                    vm::std_boc_serialize(msg).move_as_ok(), r.first->to_dec_string(), r.second);
              }));
        };

    ton_api::downcast_call(*query.params_,
                           td::overloaded(
                               [&](ton_api::storage_daemon_newContractParams &obj) {
                                 td::RefInt256 rate = td::string_to_int256(obj.rate_);
                                 if (rate.is_null() || rate->sgn() < 0) {
                                   P.set_error(td::Status::Error("Invalid rate"));
                                   return;
                                 }
                                 P.set_result(std::make_pair(std::move(rate), (td::uint32)obj.max_span_));
                               },
                               [&](ton_api::storage_daemon_newContractParamsAuto &obj) {
                                 TRY_RESULT_PROMISE(P, address, ContractAddress::parse(obj.provider_address_));
                                 init_tonlib_client();
                                 StorageProvider::get_provider_params(
                                     tonlib_client_.get(), address, P.wrap([](ProviderParams params) {
                                       return std::make_pair(std::move(params.rate_per_mb_day), params.max_span);
                                     }));
                               }));
  }

  void run_control_query(ton_api::storage_daemon_importPrivateKey &query, td::Promise<td::BufferSlice> promise) {
    auto pk = ton::PrivateKey{query.key_};
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false,
                            promise.wrap([hash = pk.compute_short_id()](td::Unit) mutable {
                              return create_serialize_tl_object<ton_api::storage_daemon_keyHash>(hash.bits256_value());
                            }));
  }

  void run_control_query(ton_api::storage_daemon_deployProvider &query, td::Promise<td::BufferSlice> promise) {
    if (!enable_storage_provider_) {
      promise.set_error(
          td::Status::Error("Storage provider is not enabled, run daemon with --storage-provider to enable it"));
      return;
    }
    if (!provider_.empty() || deploying_provider_) {
      promise.set_error(td::Status::Error("Storage provider already exists"));
      return;
    }
    TRY_RESULT_PROMISE_ASSIGN(promise, deploying_provider_, generate_fabric_contract(keyring_.get()));
    promise.set_result(create_serialize_tl_object<ton_api::storage_daemon_providerAddress>(
        deploying_provider_.value().address.to_string()));
    do_deploy_provider();
  }

  void run_control_query(ton_api::storage_daemon_initProvider &query, td::Promise<td::BufferSlice> promise) {
    if (!enable_storage_provider_) {
      promise.set_error(
          td::Status::Error("Storage provider is not enabled, run daemon with --storage-provider to enable it"));
      return;
    }
    if (!provider_.empty() || deploying_provider_) {
      promise.set_error(td::Status::Error("Storage provider already exists"));
      return;
    }
    TRY_RESULT_PROMISE_PREFIX(promise, address, ContractAddress::parse(query.account_address_), "Invalid address: ");
    do_init_provider(
        address, promise.wrap([](td::Unit) { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  void do_init_provider(ContractAddress address, td::Promise<td::Unit> promise, bool deploying = false) {
    if (deploying && (!deploying_provider_ || deploying_provider_.value().address != address)) {
      promise.set_error(td::Status::Error("Deploying was cancelled"));
      return;
    }
    daemon_config_->provider_address_ = address.to_string();
    TRY_STATUS_PROMISE(promise, save_daemon_config());
    init_tonlib_client();
    provider_ = td::actor::create_actor<StorageProvider>("provider", address, db_root_ + "/provider",
                                                         tonlib_client_.get(), manager_.get(), keyring_.get());
    deploying_provider_ = {};
    promise.set_result(td::Unit());
  }

  void do_deploy_provider() {
    if (!deploying_provider_) {
      return;
    }
    init_tonlib_client();
    check_contract_exists(
        deploying_provider_.value().address, tonlib_client_.get(),
        [SelfId = actor_id(this), client = tonlib_client_.get(),
         init = deploying_provider_.value()](td::Result<bool> R) mutable {
          if (R.is_error()) {
            LOG(INFO) << "Deploying storage contract: " << R.move_as_error();
            delay_action([=]() { td::actor::send_closure(SelfId, &StorageDaemon::do_deploy_provider); },
                         td::Timestamp::in(5.0));
            return;
          }
          if (R.ok()) {
            LOG(INFO) << "Deploying storage contract: DONE";
            td::actor::send_closure(
                SelfId, &StorageDaemon::do_init_provider, init.address, [](td::Result<td::Unit>) {}, true);
            return;
          }
          ContractAddress address = init.address;
          td::BufferSlice state_init_boc = vm::std_boc_serialize(init.state_init).move_as_ok();
          td::BufferSlice body_boc = vm::std_boc_serialize(init.msg_body).move_as_ok();
          auto query = create_tl_object<tonlib_api::raw_createAndSendMessage>(
              create_tl_object<tonlib_api::accountAddress>(address.to_string()), state_init_boc.as_slice().str(),
              body_boc.as_slice().str());
          td::actor::send_closure(
              client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::raw_createAndSendMessage>,
              std::move(query), [=](td::Result<tl_object_ptr<tonlib_api::ok>> R) {
                if (R.is_error()) {
                  LOG(INFO) << "Deploying storage contract: " << R.move_as_error();
                }
                delay_action([=]() { td::actor::send_closure(SelfId, &StorageDaemon::do_deploy_provider); },
                             td::Timestamp::in(5.0));
              });
        });
  }

  void run_control_query(ton_api::storage_daemon_removeStorageProvider &query, td::Promise<td::BufferSlice> promise) {
    if (!enable_storage_provider_) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    if (provider_.empty() && !deploying_provider_) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    daemon_config_->provider_address_ = "";
    TRY_STATUS_PROMISE(promise, save_daemon_config());
    deploying_provider_ = {};
    provider_ = {};
    auto S = td::rmrf(db_root_ + "/provider");
    if (S.is_error()) {
      LOG(ERROR) << "Failed to delete provider directory: " << S;
    }
    promise.set_result(create_serialize_tl_object<ton_api::storage_daemon_success>());
  }

  void run_control_query(ton_api::storage_daemon_getProviderParams &query, td::Promise<td::BufferSlice> promise) {
    if (!query.address_.empty()) {
      TRY_RESULT_PROMISE_PREFIX(promise, address, ContractAddress::parse(query.address_), "Invalid address: ");
      init_tonlib_client();
      StorageProvider::get_provider_params(tonlib_client_.get(), address, promise.wrap([](ProviderParams params) {
        return serialize_tl_object(params.tl(), true);
      }));
    }
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    td::actor::send_closure(provider_, &StorageProvider::get_params,
                            promise.wrap([](ProviderParams params) { return serialize_tl_object(params.tl(), true); }));
  }

  void run_control_query(ton_api::storage_daemon_setProviderParams &query, td::Promise<td::BufferSlice> promise) {
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    TRY_RESULT_PROMISE(promise, params, ProviderParams::create(query.params_));
    td::actor::send_closure(
        provider_, &StorageProvider::set_params, std::move(params),
        promise.wrap([](td::Unit) mutable { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  template <class T>
  void run_control_query(T &query, td::Promise<td::BufferSlice> promise) {
    promise.set_error(td::Status::Error("unknown query"));
  }

  void run_control_query(ton_api::storage_daemon_getProviderInfo &query, td::Promise<td::BufferSlice> promise) {
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    td::actor::send_closure(provider_, &StorageProvider::get_provider_info, query.with_balances_, query.with_contracts_,
                            promise.wrap([](tl_object_ptr<ton_api::storage_daemon_providerInfo> info) {
                              return serialize_tl_object(info, true);
                            }));
  }

  void run_control_query(ton_api::storage_daemon_setProviderConfig &query, td::Promise<td::BufferSlice> promise) {
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    td::actor::send_closure(
        provider_, &StorageProvider::set_provider_config, StorageProvider::Config(query.config_),
        promise.wrap([](td::Unit) { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  void run_control_query(ton_api::storage_daemon_withdraw &query, td::Promise<td::BufferSlice> promise) {
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    TRY_RESULT_PROMISE_PREFIX(promise, address, ContractAddress::parse(query.contract_), "Invalid address: ");
    td::actor::send_closure(provider_, &StorageProvider::withdraw, address, promise.wrap([](td::Unit) {
      return create_serialize_tl_object<ton_api::storage_daemon_success>();
    }));
  }

  void run_control_query(ton_api::storage_daemon_sendCoins &query, td::Promise<td::BufferSlice> promise) {
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    TRY_RESULT_PROMISE_PREFIX(promise, address, ContractAddress::parse(query.address_), "Invalid address: ");
    td::RefInt256 amount = td::string_to_int256(query.amount_);
    if (amount.is_null()) {
      promise.set_error(td::Status::Error("Invalid amount"));
      return;
    }
    td::actor::send_closure(
        provider_, &StorageProvider::send_coins, address, amount, std::move(query.message_),
        promise.wrap([](td::Unit) { return create_serialize_tl_object<ton_api::storage_daemon_success>(); }));
  }

  void run_control_query(ton_api::storage_daemon_closeStorageContract &query, td::Promise<td::BufferSlice> promise) {
    if (provider_.empty()) {
      promise.set_error(td::Status::Error("No storage provider"));
      return;
    }
    TRY_RESULT_PROMISE_PREFIX(promise, address, ContractAddress::parse(query.address_), "Invalid address: ");
    td::actor::send_closure(provider_, &StorageProvider::close_storage_contract, address, promise.wrap([](td::Unit) {
      return create_serialize_tl_object<ton_api::storage_daemon_success>();
    }));
  }

 private:
  static void fill_torrent_info_short(Torrent &torrent, ton_api::storage_daemon_torrent &obj) {
    obj.hash_ = torrent.get_hash();
    obj.root_dir_ = torrent.get_root_dir();
    if (torrent.inited_info()) {
      const Torrent::Info &info = torrent.get_info();
      obj.flags_ = 1;
      if (torrent.inited_header()) {
        obj.flags_ |= 2;
      }
      obj.total_size_ = info.file_size;
      obj.description_ = info.description;
      if (torrent.inited_header()) {
        obj.included_size_ = torrent.get_included_size();
        obj.files_count_ = torrent.get_files_count().unwrap();
        obj.dir_name_ = torrent.get_header().dir_name;
      }
      obj.downloaded_size_ = torrent.get_included_ready_size();
      obj.completed_ = torrent.is_completed();
    } else {
      obj.flags_ = 0;
      obj.downloaded_size_ = 0;
      obj.completed_ = false;
    }
    if (torrent.get_fatal_error().is_error()) {
      obj.flags_ |= 4;
      obj.fatal_error_ = torrent.get_fatal_error().message().str();
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
                              obj->active_upload_ = state.active_upload;
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
                                obj->torrent_->active_upload_ = state.active_upload;
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
  bool client_mode_;
  std::string global_config_;
  std::string db_root_;
  td::uint16 control_port_;
  bool enable_storage_provider_;

  tl_object_ptr<ton_api::storage_daemon_config> daemon_config_;
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

  td::actor::ActorOwn<tonlib::TonlibClientWrapper> tonlib_client_;
  td::actor::ActorOwn<StorageProvider> provider_;
  td::optional<FabricContractInit> deploying_provider_;

  void init_tonlib_client() {
    if (!tonlib_client_.empty()) {
      return;
    }
    auto r_conf_data = td::read_file(global_config_);
    r_conf_data.ensure();
    auto tonlib_options = tonlib_api::make_object<tonlib_api::options>(
        tonlib_api::make_object<tonlib_api::config>(r_conf_data.move_as_ok().as_slice().str(), "", false, false),
        tonlib_api::make_object<tonlib_api::keyStoreTypeInMemory>());
    tonlib_client_ = td::actor::create_actor<tonlib::TonlibClientWrapper>("tonlibclient", std::move(tonlib_options));
  }

  std::string daemon_config_file() {
    return db_root_ + "/config.json";
  }
};

}  // namespace ton

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::IPAddress ip_addr;
  bool client_mode = false;
  std::string global_config, db_root;
  td::uint16 control_port = 0;
  bool enable_storage_provider = false;

  td::OptionParser p;
  p.set_description("Server for seeding and downloading bags of files (torrents)\n");
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
  p.add_checked_option('I', "ip", "set <ip>:<port> for adnl. :<port> for client mode",
                       [&](td::Slice arg) -> td::Status {
                         if (ip_addr.is_valid()) {
                           return td::Status::Error("Duplicate ip address");
                         }
                         if (!arg.empty() && arg[0] == ':') {
                           TRY_RESULT(port, td::to_integer_safe<td::uint16>(arg.substr(1)));
                           TRY_STATUS(ip_addr.init_ipv4_port("127.0.0.1", port));
                           client_mode = true;
                         } else {
                           TRY_STATUS(ip_addr.init_host_port(arg.str()));
                         }
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
  p.add_option('P', "storage-provider", "run storage provider", [&]() { enable_storage_provider = true; });

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] {
    p.run(argc, argv).ensure();
    td::actor::create_actor<ton::StorageDaemon>("storage-daemon", ip_addr, client_mode, global_config, db_root,
                                                control_port, enable_storage_provider)
        .release();
  });
  while (scheduler.run(1)) {
  }
}
