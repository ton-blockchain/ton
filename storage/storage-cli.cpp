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

#include "adnl/adnl.h"
#include "common/bigint.hpp"
#include "common/bitstring.h"
#include "dht/dht.h"
#include "keys/encryptor.h"
#include "overlay/overlay.h"

#include "td/utils/JsonBuilder.h"
#include "td/utils/port/signals.h"
#include "td/utils/Parser.h"
#include "td/utils/overloaded.h"
#include "td/utils/OptionParser.h"
#include "td/utils/PathView.h"
#include "td/utils/Random.h"
#include "td/utils/misc.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"

#include "td/actor/MultiPromise.h"
#include "terminal/terminal.h"

#include "Torrent.h"
#include "TorrentCreator.h"
#include "PeerManager.h"

#include "auto/tl/ton_api_json.h"

#include <iostream>
#include <limits>
#include <map>
#include <set>
#include "git.h"

struct StorageCliOptions {
  std::string config;
  bool enable_readline{true};
  std::string db_root{"dht-db/"};

  td::IPAddress addr;

  td::optional<std::string> cmd;
};

using AdnlCategory = td::int32;

class StorageCli : public td::actor::Actor {
 public:
  explicit StorageCli(StorageCliOptions options) : options_(std::move(options)) {
  }

 private:
  StorageCliOptions options_;
  td::actor::ActorOwn<td::TerminalIO> io_;
  //td::actor::ActorOwn<DhtServer> dht_server_;

  std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config_;

  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> adnl_network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::dht::Dht> dht_;
  td::actor::ActorOwn<ton::overlay::Overlays> overlays_;
  td::actor::ActorOwn<ton_rldp::Rldp> rldp_;
  //ton::PublicKeyHash default_dht_node_ = ton::PublicKeyHash::zero();
  ton::PublicKey public_key_;

  bool one_shot_{false};
  bool is_closing_{false};
  td::uint32 ref_cnt_{1};

  td::Status load_global_config() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(options_.config), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

    // TODO
    // add adnl static nodes
    //if (conf.adnl_) {
    //  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config,
    //                          std::move(conf.adnl_->static_nodes_));
    //}
    if (!conf.dht_) {
      return td::Status::Error(ton::ErrorCode::error, "does not contain [dht] section");
    }

    TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
    dht_config_ = std::move(dht);
    return td::Status::OK();
  }

  void start_up() override {
    class Cb : public td::TerminalIO::Callback {
     public:
      void line_cb(td::BufferSlice line) override {
        td::actor::send_closure(id_, &StorageCli::parse_line, std::move(line));
      }
      Cb(td::actor::ActorShared<StorageCli> id) : id_(std::move(id)) {
      }

     private:
      td::actor::ActorShared<StorageCli> id_;
    };
    if (options_.cmd) {
      one_shot_ = true;
      td::actor::send_closure(actor_id(this), &StorageCli::parse_line, td::BufferSlice(options_.cmd.unwrap()));
    } else {
      ref_cnt_++;
      io_ = td::TerminalIO::create("> ", options_.enable_readline, false, std::make_unique<Cb>(actor_shared(this)));
      td::actor::send_closure(io_, &td::TerminalIO::set_log_interface);
    }

    if (!options_.config.empty()) {
      init_network();
    }
  }

  void init_network() {
    load_global_config().ensure();

    td::mkdir(options_.db_root).ignore();
    keyring_ = ton::keyring::Keyring::create(options_.db_root + "/keyring");
    adnl_network_manager_ =
        ton::adnl::AdnlNetworkManager::create(td::narrow_cast<td::uint16>(options_.addr.get_port()));
    adnl_ = ton::adnl::Adnl::create(options_.db_root, keyring_.get());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, adnl_network_manager_.get());
    rldp_ = ton_rldp::Rldp::create(adnl_.get());

    auto key_path = options_.db_root + "/key.pub";
    auto r_public_key = td::read_file(key_path).move_fmap([](auto raw) { return ton::PublicKey::import(raw); });
    ;
    if (r_public_key.is_error()) {
      auto private_key = ton::PrivateKey(ton::privkeys::Ed25519::random());
      public_key_ = private_key.compute_public_key();
      td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(private_key), false,
                              td::Promise<td::Unit>([key_path, str = public_key_.export_as_slice()](auto) {
                                td::write_file(key_path, str.as_slice()).ensure();
                                LOG(INFO) << "New key was saved";
                              }));
    } else {
      public_key_ = r_public_key.move_as_ok();
    }
    auto short_id = public_key_.compute_short_id();
    LOG(ERROR) << "Create " << short_id;

    std::set<AdnlCategory> cats;
    cats = {0, 1, 2, 3};
    ton::adnl::AdnlCategoryMask cat_mask;
    for (auto cat : cats) {
      cat_mask[cat] = true;
    }
    td::actor::send_closure(adnl_network_manager_, &ton::adnl::AdnlNetworkManager::add_self_addr, options_.addr,
                            std::move(cat_mask), cats.size() ? 0 : 1);

    td::uint32 ts = static_cast<td::uint32>(td::Clocks::system());

    std::map<td::uint32, ton::adnl::AdnlAddressList> addr_lists_;
    for (auto cat : cats) {
      CHECK(cat >= 0);
      ton::adnl::AdnlAddress x = ton::adnl::AdnlAddressImpl::create(
          ton::create_tl_object<ton::ton_api::adnl_address_udp>(options_.addr.get_ipv4(), options_.addr.get_port()));
      addr_lists_[cat].add_addr(std::move(x));
      addr_lists_[cat].set_version(ts);
      addr_lists_[cat].set_reinit_date(ton::adnl::Adnl::adnl_start_time());
    }

    for (auto cat : cats) {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{public_key_}, addr_lists_[cat],
                              static_cast<td::uint8>(cat));
      td::actor::send_closure(rldp_, &ton_rldp::Rldp::add_id,
                              ton::adnl::AdnlNodeIdFull{public_key_}.compute_short_id());
    }

    dht_ =
        ton::dht::Dht::create(ton::adnl::AdnlNodeIdShort{short_id}, "" /*NO db for dht! No wrong cache - no problems*/,
                              dht_config_, keyring_.get(), adnl_.get())
            .move_as_ok();

    send_closure(adnl_, &ton::Adnl::register_dht_node, dht_.get());
    overlays_ = ton::overlay::Overlays::create(options_.db_root, keyring_.get(), adnl_.get(), dht_.get());
  }

  void exit(td::Result<td::Unit> res) {
    if (one_shot_) {
      td::TerminalIO::out() << "Done, exiting";
      std::_Exit(res.is_ok() ? 0 : 2);
    }
  }

  void parse_line(td::BufferSlice line) {
    if (is_closing_) {
      return;
    }
    td::ConstParser parser(line.as_slice());
    auto cmd = parser.read_word();
    if (cmd.empty()) {
      return;
    }

    td::Promise<td::Unit> cmd_promise = [line = line.clone(), timer = td::PerfWarningTimer(line.as_slice().str()),
                                         cli = actor_id(this)](td::Result<td::Unit> res) {
      if (res.is_ok()) {
        // on_ok
      } else {
        td::TerminalIO::out() << "Query {" << line.as_slice() << "} FAILED: \n\t" << res.error() << "\n";
      }
      send_closure(cli, &StorageCli::exit, std::move(res));
    };

    if (cmd == "help") {
      td::TerminalIO::out() << "help\tThis help\n";

      td::TerminalIO::out() << "create <dir/file>\tCreate torrent from a directory\n";
      td::TerminalIO::out() << "info <id>\tPrint info about loaded torrent\n";
      td::TerminalIO::out() << "load <file>\tLoad torrent file in memory\n";
      td::TerminalIO::out() << "addhash <hash>\tAdd torrent by hash (in hex)\n";
      td::TerminalIO::out() << "save <id> <file>\tSave torrent file\n";
      td::TerminalIO::out() << "start <id>\tStart torrent downloading/uploading\n";
      td::TerminalIO::out() << "seed <id>\tStart torrent uploading\n";
      td::TerminalIO::out() << "download <id>\tStart torrent and stop when it is completed\n";
      td::TerminalIO::out() << "stop <id>\tStop torrent downloading\n";

      td::TerminalIO::out() << "pause <id>\tPause active torrent downloading\n";
      td::TerminalIO::out() << "resume <id>\tResume active torrent downloading\n";
      td::TerminalIO::out() << "priority <id> <file_id> <priority>\tSet file priority(0..254) by file_id, use "
                               "file_id=* to set priority for all files\n";

      td::TerminalIO::out() << "exit\tExit\n";
      td::TerminalIO::out() << "quit\tExit\n";
    } else if (cmd == "exit" || cmd == "quit") {
      quit();
    } else if (cmd == "create") {
      torrent_create(parser.read_all(), std::move(cmd_promise));
    } else if (cmd == "info") {
      torrent_info(parser.read_all(), std::move(cmd_promise));
    } else if (cmd == "load") {
      cmd_promise.set_result(torrent_load(parser.read_all()).move_map([](auto &&x) { return td::Unit(); }));
    } else if (cmd == "addhash") {
      cmd_promise.set_result(torrent_add_by_hash(parser.read_all()).move_map([](auto &&x) { return td::Unit(); }));
    } else if (cmd == "save") {
      auto id = parser.read_word();
      parser.skip_whitespaces();
      auto file = parser.read_all();
      torrent_save(id, file, std::move(cmd_promise));
    } else if (cmd == "start") {
      auto id = parser.read_word();
      torrent_start(id, false, true, std::move(cmd_promise));
    } else if (cmd == "download") {
      auto id = parser.read_word();
      torrent_start(id, true, true, std::move(cmd_promise));
    } else if (cmd == "seed") {
      auto id = parser.read_word();
      torrent_start(id, false, false, std::move(cmd_promise));
    } else if (cmd == "stop") {
      auto id = parser.read_word();
      torrent_stop(id, std::move(cmd_promise));
    } else if (cmd == "pause") {
      auto id = parser.read_word();
      torrent_set_should_download(id, false, std::move(cmd_promise));
    } else if (cmd == "resume") {
      auto id = parser.read_word();
      torrent_set_should_download(id, true, std::move(cmd_promise));
    } else if (cmd == "priority") {
      torrent_set_priority(parser, std::move(cmd_promise));
    } else if (cmd == "get") {
      auto name = parser.read_word().str();
      auto key = ton::dht::DhtKey(public_key_.compute_short_id(), name, 0);
      send_closure(dht_, &ton::dht::Dht::get_value, std::move(key), cmd_promise.wrap([](auto &&res) {
        LOG(ERROR) << to_string(res.tl());
        return td::Unit();
      }));

    } else if (cmd == "set") {
      auto name = parser.read_word().str();
      parser.skip_whitespaces();
      auto value = parser.read_all().str();

      auto key = ton::dht::DhtKey(public_key_.compute_short_id(), name, 0);
      auto dht_update_rule = ton::dht::DhtUpdateRuleSignature::create().move_as_ok();
      ton::dht::DhtKeyDescription dht_key_description{key.clone(), public_key_, dht_update_rule, td::BufferSlice()};

      auto to_sing = dht_key_description.to_sign();
      send_closure(keyring_, &ton::keyring::Keyring::sign_message, public_key_.compute_short_id(), std::move(to_sing),
                   cmd_promise.send_closure(actor_id(this), &StorageCli::dht_set1, std::move(dht_key_description),
                                            td::BufferSlice(value)));
    } else {
      cmd_promise.set_error(td::Status::Error(PSLICE() << "Unkwnown query `" << cmd << "`"));
    }
    if (cmd_promise) {
      cmd_promise.set_value(td::Unit());
    }
  }

  void dht_set1(ton::dht::DhtKeyDescription dht_key_description, td::BufferSlice value,
                td::Result<td::BufferSlice> r_signature, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, signature, std::move(r_signature));
    dht_key_description.update_signature(std::move(signature));
    dht_key_description.check().ensure();

    auto ttl = static_cast<td::uint32>(td::Clocks::system() + 3600);
    ton::dht::DhtValue dht_value{dht_key_description.clone(), std::move(value), ttl, td::BufferSlice("")};
    auto to_sign = dht_value.to_sign();
    send_closure(keyring_, &ton::keyring::Keyring::sign_message, public_key_.compute_short_id(), std::move(to_sign),
                 promise.send_closure(actor_id(this), &StorageCli::dht_set2, std::move(dht_value)));
  }

  void dht_set2(ton::dht::DhtValue dht_value, td::Result<td::BufferSlice> r_signature, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, signature, std::move(r_signature));
    dht_value.update_signature(std::move(signature));
    dht_value.check().ensure();

    send_closure(dht_, &ton::dht::Dht::set_value, std::move(dht_value), promise.wrap([](auto &&res) {
      LOG(ERROR) << "OK";
      return td::Unit();
    }));
  }

  td::uint32 torrent_id_{0};
  struct Info {
    td::uint32 id;
    td::Bits256 hash;
    td::optional<ton::Torrent> torrent;
    td::actor::ActorOwn<PeerManager> peer_manager;
    td::actor::ActorOwn<ton::NodeActor> node;
  };
  std::map<td::uint32, Info> infos_;

  void torrent_create(td::Slice path_raw, td::Promise<td::Unit> promise) {
    auto path = td::trim(path_raw).str();
    ton::Torrent::Creator::Options options;
    options.piece_size = 128 * 1024;
    TRY_RESULT_PROMISE(promise, torrent, ton::Torrent::Creator::create_from_path(options, path));
    auto hash = torrent.get_hash();
    for (auto &it : infos_) {
      if (it.second.hash == hash) {
        promise.set_error(td::Status::Error(PSLICE() << "Torrent already loaded (#" << it.first << ")"));
        return;
      }
    }
    td::TerminalIO::out() << "Torrent #" << torrent_id_ << " created\n";
    td::TerminalIO::out() << "Torrent hash: " << torrent.get_hash().to_hex() << "\n";
    infos_.emplace(torrent_id_, Info{torrent_id_, hash, std::move(torrent), td::actor::ActorOwn<PeerManager>(),
                                     td::actor::ActorOwn<ton::NodeActor>()});
    torrent_id_++;

    promise.set_value(td::Unit());
  }

  td::Result<ton::Torrent *> to_torrent(td::Slice id_raw) {
    TRY_RESULT(id, td::to_integer_safe<td::uint32>(td::trim(id_raw)));
    auto it = infos_.find(id);
    if (it == infos_.end()) {
      return td::Status::Error(PSLICE() << "Invalid torrent id <" << id_raw << ">");
    }
    if (it->second.torrent) {
      return &it->second.torrent.value();
    }
    return nullptr;
  }

  td::Result<Info *> to_info(td::Slice id_raw) {
    TRY_RESULT(id, td::to_integer_safe<td::uint32>(td::trim(id_raw)));
    auto it = infos_.find(id);
    if (it == infos_.end()) {
      return td::Status::Error(PSLICE() << "Invalid torrent id <" << id_raw << ">");
    }
    return &it->second;
  }
  td::Result<Info *> to_info_or_load(td::Slice id_raw) {
    auto r_info = to_info(id_raw);
    if (r_info.is_ok()) {
      return r_info.ok();
    }
    return torrent_load(id_raw);
  }

  void torrent_info(td::Slice id_raw, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, info, to_info(id_raw));
    if (info->torrent) {
      td::TerminalIO::out() << info->torrent.value().get_stats_str();
      promise.set_value(td::Unit());
    } else {
      send_closure(info->node, &ton::NodeActor::get_stats_str, promise.wrap([](std::string stats) {
        td::TerminalIO::out() << stats;
        return td::Unit();
      }));
    }
  }

  td::actor::ActorOwn<PeerManager> create_peer_manager(td::Bits256 hash) {
    // create overlay network
    td::BufferSlice hash_str(hash.as_slice());
    ton::overlay::OverlayIdFull overlay_id(std::move(hash_str));
    auto adnl_id = ton::adnl::AdnlNodeIdShort{public_key_.compute_short_id()};
    return td::actor::create_actor<PeerManager>("PeerManager", adnl_id, std::move(overlay_id), false, overlays_.get(),
                                                adnl_.get(), rldp_.get());
  }

  void torrent_start(td::Slice id_raw, bool wait_download, bool should_download, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, ptr, to_info_or_load(id_raw));
    if (!ptr->torrent) {
      promise.set_error(td::Status::Error("torrent is already started"));
      return;
    }
    if (ptr->peer_manager.empty()) {
      ptr->peer_manager = create_peer_manager(ptr->torrent.value().get_hash());
    }
    ton::PeerId self_id = 1;

    class Callback : public ton::NodeActor::Callback {
     public:
      Callback(td::actor::ActorId<StorageCli> storage_cli, td::uint32 torrent_id, td::Promise<td::Unit> on_completed)
          : storage_cli_(std::move(storage_cli))
          , torrent_id_(std::move(torrent_id))
          , on_completed_(std::move(on_completed)) {
      }

      void on_completed() override {
        if (on_completed_) {
          on_completed_.set_value(td::Unit());
        }
        td::TerminalIO::out() << "Torrent #" << torrent_id_ << " completed\n";
      }

      void on_closed(ton::Torrent torrent) override {
        send_closure(storage_cli_, &StorageCli::got_torrent, torrent_id_, std::move(torrent));
      }

     private:
      td::actor::ActorId<StorageCli> storage_cli_;
      td::uint32 torrent_id_;
      td::Promise<td::Unit> on_completed_;
    };

    td::Promise<td::Unit> on_completed;
    if (wait_download) {
      on_completed = std::move(promise);
    }
    auto callback = td::make_unique<Callback>(actor_id(this), ptr->id, std::move(on_completed));
    auto context = PeerManager::create_callback(ptr->peer_manager.get());
    ptr->node =
        td::actor::create_actor<ton::NodeActor>(PSLICE() << "Node#" << self_id, self_id, ptr->torrent.unwrap(),
                                                std::move(callback), std::move(context), nullptr, should_download);
    td::TerminalIO::out() << "Torrent #" << ptr->id << " started\n";
    promise.release().release();
    if (promise) {
      promise.set_value(td::Unit());
    }
  }

  void on_torrent_completed(td::uint32 torrent_id) {
    td::TerminalIO::out() << "Torrent #" << torrent_id << " completed\n";
  }
  void got_torrent(td::uint32 torrent_id, ton::Torrent &&torrent) {
    infos_[torrent_id].torrent = std::move(torrent);
    td::TerminalIO::out() << "Torrent #" << torrent_id << " ready to start again\n";
  }

  void torrent_stop(td::Slice id_raw, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, ptr, to_info(id_raw));
    ptr->node.reset();
    ptr->peer_manager.reset();
    promise.set_value(td::Unit());
    td::TerminalIO::out() << "Torrent #" << ptr->id << " stopped\n";
  }

  void torrent_set_should_download(td::Slice id_raw, bool should_download, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, ptr, to_info(id_raw));
    if (ptr->node.empty()) {
      promise.set_error(td::Status::Error("Torrent is not active"));
      return;
    }
    send_closure(ptr->node, &ton::NodeActor::set_should_download, should_download);
    promise.set_value(td::Unit());
  }

  void torrent_set_priority(td::ConstParser &parser, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, ptr, to_info(parser.read_word()));
    if (ptr->node.empty()) {
      promise.set_error(td::Status::Error("Torrent is not active"));
      return;
    }
    auto file_id_str = parser.read_word();
    size_t file_id = 0;
    bool all = false;
    if (file_id_str == "*") {
      all = true;
    } else {
      TRY_RESULT_PROMISE_ASSIGN(promise, file_id, td::to_integer_safe<std::size_t>(file_id_str));
    }
    TRY_RESULT_PROMISE(promise, priority, td::to_integer_safe<td::uint8>(parser.read_word()));
    if (priority == 255) {
      promise.set_error(td::Status::Error("Priority = 255 is reserved"));
      return;
    }
    if (all) {
      send_closure(ptr->node, &ton::NodeActor::set_all_files_priority, priority,
                   promise.wrap([](bool) { return td::Unit(); }));
    } else {
      send_closure(ptr->node, &ton::NodeActor::set_file_priority_by_idx, file_id, priority,
                   promise.wrap([](bool) { return td::Unit(); }));
    }
    promise.set_value(td::Unit());
  }

  void torrent_save(td::Slice id_raw, td::Slice path, td::Promise<td::Unit> promise) {
    TRY_RESULT_PROMISE(promise, ptr, to_torrent(id_raw));
    auto meta = ptr->get_meta(ton::Torrent::GetMetaOptions().with_proof_depth_limit(10));
    TRY_STATUS_PROMISE(promise, td::write_file(path.str(), meta.serialize()));
    promise.set_value(td::Unit());
    td::TerminalIO::out() << "Torrent #" << id_raw << " saved\n";
  }

  td::Result<Info *> torrent_load(td::Slice path) {
    TRY_RESULT(data, td::read_file(PSLICE() << td::trim(path)));
    TRY_RESULT(meta, ton::TorrentMeta::deserialize(data));
    ton::Torrent::Options options;
    options.in_memory = false;
    options.root_dir = ".";
    options.validate = true;

    TRY_RESULT(torrent, ton::Torrent::open(options, data));

    auto hash = torrent.get_hash();
    for (auto &it : infos_) {
      if (it.second.hash == hash) {
        return td::Status::Error(PSLICE() << "Torrent already loaded (#" << it.first << ")");
      }
    }
    td::TerminalIO::out() << "Torrent #" << torrent_id_ << " created\n";
    td::TerminalIO::out() << "Torrent hash: " << torrent.get_hash().to_hex() << "\n";
    auto res =
        infos_.emplace(torrent_id_, Info{torrent_id_, hash, std::move(torrent), td::actor::ActorOwn<PeerManager>(),
                                         td::actor::ActorOwn<ton::NodeActor>()});
    torrent_id_++;
    return &res.first->second;
  }

  td::Result<Info *> torrent_add_by_hash(td::Slice hash_hex) {
    td::Bits256 hash;
    if (hash.from_hex(hash_hex) != 256) {
      return td::Status::Error("Failed to parse torrent hash");
    }
    ton::Torrent::Options options;
    options.in_memory = false;
    options.root_dir = ".";
    options.validate = false;

    TRY_RESULT(torrent, ton::Torrent::open(options, hash));

    for (auto &it : infos_) {
      if (it.second.hash == hash) {
        return td::Status::Error(PSLICE() << "Torrent already loaded (#" << it.first << ")");
      }
    }
    td::TerminalIO::out() << "Torrent #" << torrent_id_ << " created\n";
    td::TerminalIO::out() << "Torrent hash: " << torrent.get_hash().to_hex() << "\n";
    auto res =
        infos_.emplace(torrent_id_, Info{torrent_id_, hash, std::move(torrent), td::actor::ActorOwn<PeerManager>(),
                                         td::actor::ActorOwn<ton::NodeActor>()});
    torrent_id_++;
    return &res.first->second;
  }

  void hangup() override {
    quit();
  }
  void hangup_shared() override {
    CHECK(ref_cnt_ > 0);
    ref_cnt_--;
    //if (get_link_token() == 1) {
    //io_.reset();
    //}
    try_stop();
  }
  void try_stop() {
    if (is_closing_ && ref_cnt_ == 0) {
      stop();
    }
  }
  void quit() {
    is_closing_ = true;
    io_.reset();
    //client_.reset();
    ref_cnt_--;
    try_stop();
  }

  void tear_down() override {
    td::actor::SchedulerContext::get()->stop();
  }
};

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler();

  StorageCliOptions options;
  td::OptionParser p;
  p.set_description("experimental cli for ton storage");
  p.add_option('h', "help", "prints_help", [&]() {
    std::cout << (PSLICE() << p).c_str();
    std::exit(2);
  });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    auto verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 20) ? td::Status::OK() : td::Status::Error("verbosity must be 0..20");
  });
  p.add_option('V', "version", "shows storage-cli build information", [&]() {
    std::cout << "storage-cli build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('C', "config", "set ton config", [&](td::Slice arg) { options.config = arg.str(); });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) { options.db_root = fname.str(); });
  p.add_checked_option('I', "ip", "set ip:port", [&](td::Slice arg) {
    td::IPAddress addr;
    TRY_STATUS(addr.init_host_port(arg.str()));
    options.addr = addr;
    return td::Status::OK();
  });
  p.add_option('E', "execute", "execute one command", [&](td::Slice arg) { options.cmd = arg.str(); });
  p.add_checked_option('d', "dir", "working directory", [&](td::Slice arg) { return td::chdir(arg.str()); });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }

  td::actor::Scheduler scheduler({3});
  scheduler.run_in_context([&] { td::actor::create_actor<StorageCli>("console", options).release(); });
  scheduler.run();
  return 0;
}
