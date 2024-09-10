/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission
    to link the code of portions of this program with the OpenSSL library.
    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the file(s),
    but you are not obligated to do so. If you do not wish to do so, delete this
    exception statement from your version. If you delete this exception statement
    from all source files in the program, then also delete it here.
*/
#include "td/utils/filesystem.h"
#include "td/actor/actor.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Random.h"
#include "td/utils/FileLog.h"
#include "git.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/lite_api.h"
#include "tl-utils/lite-utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "adnl/adnl.h"
#include "lite-client/ext-client.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include "td/utils/overloaded.h"

#include <iostream>
#include <map>
#include <auto/tl/lite_api.hpp>
#include "td/utils/tl_storers.h"

using namespace ton;

class ProxyLiteserver : public td::actor::Actor {
 public:
  ProxyLiteserver(std::string global_config, std::string db_root, td::uint16 port, PublicKeyHash public_key_hash)
      : global_config_(std::move(global_config))
      , db_root_(std::move(db_root))
      , port_(port)
      , public_key_hash_(public_key_hash) {
  }

  void start_up() override {
    LOG_CHECK(!db_root_.empty()) << "db root is not set";
    td::mkdir(db_root_).ensure();
    db_root_ = td::realpath(db_root_).move_as_ok();
    keyring_ = keyring::Keyring::create(db_root_ + "/keyring");

    if (public_key_hash_.is_zero()) {
      id_ = {};
      run();
    } else {
      td::actor::send_closure(keyring_, &keyring::Keyring::get_public_key, public_key_hash_,
                              [SelfId = actor_id(this)](td::Result<PublicKey> R) mutable {
                                if (R.is_error()) {
                                  LOG(FATAL) << "Failed to load public key: " << R.move_as_error();
                                }
                                td::actor::send_closure(SelfId, &ProxyLiteserver::got_public_key, R.move_as_ok());
                              });
    }
  }

  void got_public_key(PublicKey pub) {
    id_ = adnl::AdnlNodeIdFull{pub};
    run();
  }

  void run() {
    td::Status S = prepare_local_config();
    if (S.is_error()) {
      LOG(FATAL) << "Local config error: " << S;
    }

    S = parse_global_config();
    if (S.is_error()) {
      LOG(FATAL) << S;
    }

    run_clients();
    create_ext_server();
  }

  td::Status prepare_local_config() {
    auto r_conf_data = td::read_file(config_file());
    if (r_conf_data.is_ok()) {
      auto conf_data = r_conf_data.move_as_ok();
      TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");
      TRY_STATUS_PREFIX(ton_api::from_json(*config_, conf_json.get_object()), "json does not fit TL scheme: ");
      TRY_RESULT_PREFIX(cfg_port, td::narrow_cast_safe<td::uint16>(config_->port_), "invalid port: ");
      TRY_RESULT_PREFIX(cfg_id, adnl::AdnlNodeIdFull::create(config_->id_), "invalid id: ");
      bool rewrite_config = false;
      if (port_ == 0) {
        port_ = cfg_port;
      } else {
        rewrite_config |= (port_ != cfg_port);
      }
      if (id_.empty()) {
        id_ = std::move(cfg_id);
      } else {
        rewrite_config |= (id_ != cfg_id);
      }
      if (!rewrite_config) {
        return td::Status::OK();
      }
    } else {
      LOG(WARNING) << "First launch, creating local config";
    }
    if (port_ == 0) {
      return td::Status::Error("port is not set");
    }
    config_->port_ = port_;
    if (id_.empty()) {
      auto pk = PrivateKey{privkeys::Ed25519::random()};
      id_ = adnl::AdnlNodeIdFull{pk.compute_public_key()};
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), false, [](td::Result<td::Unit> R) {
        if (R.is_error()) {
          LOG(FATAL) << "Failed to store private key";
        }
      });
    }
    config_->id_ = id_.tl();

    auto s = td::json_encode<std::string>(td::ToJson(*config_), true);
    TRY_STATUS_PREFIX(td::write_file(config_file(), s), "failed to write file: ");
    LOG(WARNING) << "Writing config.json";
    return td::Status::OK();
  }

  td::Status parse_global_config() {
    TRY_RESULT_PREFIX(global_config_data, td::read_file(global_config_), "Failed to read global config: ");
    TRY_RESULT_PREFIX(global_config_json, td::json_decode(global_config_data.as_slice()),
                      "Failed to parse global config: ");
    ton_api::liteclient_config_global gc;
    TRY_STATUS_PREFIX(ton_api::from_json(gc, global_config_json.get_object()), "Failed to parse global config: ");
    TRY_RESULT_PREFIX(servers, liteclient::LiteServerConfig::parse_global_config(gc),
                      "Falied to parse liteservers in global config: ");
    if (servers.empty()) {
      return td::Status::Error("No liteservers in global config");
    }
    for (auto& s : servers) {
      servers_.emplace_back();
      servers_.back().config = std::move(s);
    }
    return td::Status::OK();
  }

  void run_clients() {
    class Callback : public adnl::AdnlExtClient::Callback {
     public:
      explicit Callback(td::actor::ActorId<ProxyLiteserver> id, size_t idx) : id_(std::move(id)), idx_(idx) {
      }
      void on_ready() override {
        td::actor::send_closure(id_, &ProxyLiteserver::on_client_status, idx_, true);
      }
      void on_stop_ready() override {
        td::actor::send_closure(id_, &ProxyLiteserver::on_client_status, idx_, false);
      }

     private:
      td::actor::ActorId<ProxyLiteserver> id_;
      size_t idx_;
    };

    for (size_t i = 0; i < servers_.size(); ++i) {
      Server& server = servers_[i];
      server.client = adnl::AdnlExtClient::create(server.config.adnl_id, server.config.addr,
                                                  std::make_unique<Callback>(actor_id(this), i));
      server.alive = false;
    }
  }

  void on_client_status(size_t idx, bool ready) {
    Server& server = servers_[idx];
    if (server.alive == ready) {
      return;
    }
    server.alive = ready;
    LOG(WARNING) << (ready ? "Connected to" : "Disconnected from") << " server #" << idx << " ("
                 << server.config.addr.get_ip_str() << ":" << server.config.addr.get_port() << ")";
  }

  void create_ext_server() {
    adnl_ = adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &adnl::Adnl::add_id, id_, adnl::AdnlAddressList{}, (td::uint8)255);

    class AdnlCallback : public adnl::Adnl::Callback {
     public:
      explicit AdnlCallback(td::actor::ActorId<ProxyLiteserver> id) : id_(id) {
      }

      void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::send_closure(id_, &ProxyLiteserver::receive_query, std::move(data), std::move(promise));
      }

     private:
      td::actor::ActorId<ProxyLiteserver> id_;
    };
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id_.compute_short_id(),
                            adnl::Adnl::int_to_bytestring(lite_api::liteServer_query::ID),
                            std::make_unique<AdnlCallback>(actor_id(this)));
    td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server, std::vector{id_.compute_short_id()},
                            std::vector{port_},
                            [SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
                              R.ensure();
                              td::actor::send_closure(SelfId, &ProxyLiteserver::created_ext_server, R.move_as_ok());
                            });
  }

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> s) {
    ext_server_ = std::move(s);
    LOG(WARNING) << "Started proxy liteserver on port " << port_;
    alarm();
  }

  td::Result<size_t> select_server(const liteclient::QueryInfo& query_info) {
    size_t best_idx = servers_.size();
    int cnt = 0;
    for (size_t i = 0; i < servers_.size(); ++i) {
      Server& server = servers_[i];
      if (!server.alive || !server.config.accepts_query(query_info)) {
        continue;
      }
      ++cnt;
      if (td::Random::fast(1, cnt) == 1) {
        best_idx = i;
      }
    }
    if (best_idx == servers_.size()) {
      return td::Status::Error(PSTRING() << "no liteserver for query " << query_info.to_str());
    }
    return best_idx;
  }

  void receive_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
    // Like in ValidatorManagerImpl::run_ext_query
    auto F = fetch_tl_object<lite_api::liteServer_query>(data, true);
    if (F.is_ok()) {
      data = std::move(F.move_as_ok()->data_);
    } else {
      auto G = fetch_tl_prefix<lite_api::liteServer_queryPrefix>(data, true);
      if (G.is_error()) {
        promise.set_error(G.move_as_error());
        return;
      }
    }

    tl_object_ptr<lite_api::liteServer_waitMasterchainSeqno> wait_mc_seqno_obj;
    auto E = fetch_tl_prefix<lite_api::liteServer_waitMasterchainSeqno>(data, true);
    if (E.is_ok()) {
      wait_mc_seqno_obj = E.move_as_ok();
    }
    liteclient::QueryInfo query_info = liteclient::get_query_info(data);
    ++ls_stats_[query_info.query_id];
    promise = [promise = std::move(promise), query_info, timer = td::Timer(),
               wait_mc_seqno =
                   (wait_mc_seqno_obj ? wait_mc_seqno_obj->seqno_ : 0)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_ok()) {
        LOG(INFO) << "Query " << query_info.to_str()
                  << (wait_mc_seqno ? PSTRING() << " (wait seqno " << wait_mc_seqno << ")" : "")
                  << ": OK, time=" << timer.elapsed() << ", response_size=" << R.ok().size();
        promise.set_value(R.move_as_ok());
        return;
      }
      LOG(INFO) << "Query " << query_info.to_str()
                << (wait_mc_seqno ? PSTRING() << " (wait seqno " << wait_mc_seqno << ")" : "") << ": " << R.error();
      promise.set_value(create_serialize_tl_object<lite_api::liteServer_error>(
          R.error().code(), "Gateway error: " + R.error().message().str()));
    };

    TRY_RESULT_PROMISE(promise, server_idx, select_server(query_info));
    Server& server = servers_[server_idx];
    LOG(INFO) << "Sending query " << query_info.to_str()
              << (wait_mc_seqno_obj ? PSTRING() << " (wait seqno " << wait_mc_seqno_obj->seqno_ << ")" : "")
              << ", size=" << data.size() << ", to server #" << server_idx << " (" << server.config.addr.get_ip_str()
              << ":" << server.config.addr.get_port() << ")";

    BlockSeqno wait_mc_seqno = wait_mc_seqno_obj ? wait_mc_seqno_obj->seqno_ : 0;
    wait_mc_seqno = std::max(wait_mc_seqno, last_known_masterchain_seqno_);
    if (server.last_known_masterchain_seqno < wait_mc_seqno) {
      int timeout_ms = wait_mc_seqno_obj ? wait_mc_seqno_obj->timeout_ms_ : 8000;
      data = serialize_tl_object(create_tl_object<lite_api::liteServer_waitMasterchainSeqno>(wait_mc_seqno, timeout_ms),
                                 true, std::move(data));
    }
    data = create_serialize_tl_object<lite_api::liteServer_query>(std::move(data));
    td::actor::send_closure(server.client, &adnl::AdnlExtClient::send_query, "q", std::move(data),
                            td::Timestamp::in(8.0),
                            [SelfId = actor_id(this), promise = std::move(promise), server_idx,
                             wait_mc_seqno](td::Result<td::BufferSlice> R) mutable {
                              if (R.is_ok()) {
                                td::actor::send_closure(SelfId, &ProxyLiteserver::process_query_response,
                                                        R.ok().clone(), server_idx, wait_mc_seqno);
                              }
                              promise.set_result(std::move(R));
                            });
  }

  void process_query_response(td::BufferSlice data, size_t server_idx, BlockSeqno wait_mc_seqno) {
    auto F = fetch_tl_object<lite_api::Object>(data, true);
    if (F.is_error() || F.ok()->get_id() == lite_api::liteServer_error::ID) {
      return;
    }
    BlockSeqno new_seqno = wait_mc_seqno;
    lite_api::downcast_call(*F.ok_ref(), td::overloaded(
                                             [&](lite_api::liteServer_masterchainInfo& f) {
                                               new_seqno = std::max<BlockSeqno>(new_seqno, f.last_->seqno_);
                                             },
                                             [&](lite_api::liteServer_masterchainInfoExt& f) {
                                               new_seqno = std::max<BlockSeqno>(new_seqno, f.last_->seqno_);
                                             },
                                             [&](lite_api::liteServer_accountState& f) {
                                               if (f.id_->workchain_ == masterchainId) {
                                                 new_seqno = std::max<BlockSeqno>(new_seqno, f.id_->seqno_);
                                               }
                                             },
                                             [&](auto& obj) {}));
    servers_[server_idx].last_known_masterchain_seqno =
        std::max(servers_[server_idx].last_known_masterchain_seqno, new_seqno);
    if (new_seqno > last_known_masterchain_seqno_) {
      last_known_masterchain_seqno_ = new_seqno;
      LOG(INFO) << "Last known masterchain seqno = " << new_seqno;
    }
  }

  void alarm() override {
    alarm_timestamp() = td::Timestamp::in(60.0);
    if (!ls_stats_.empty()) {
      td::StringBuilder sb;
      sb << "Liteserver stats (1 minute):";
      td::uint32 total = 0;
      for (const auto& p : ls_stats_) {
        sb << " " << lite_query_name_by_id(p.first) << ":" << p.second;
        total += p.second;
      }
      sb << " TOTAL:" << total;
      LOG(WARNING) << sb.as_cslice();
      ls_stats_.clear();
    }
  }

 private:
  std::string global_config_;
  std::string db_root_;
  td::uint16 port_;
  PublicKeyHash public_key_hash_;

  tl_object_ptr<ton_api::proxyLiteserver_config> config_ = create_tl_object<ton_api::proxyLiteserver_config>();
  adnl::AdnlNodeIdFull id_;

  td::actor::ActorOwn<keyring::Keyring> keyring_;
  td::actor::ActorOwn<adnl::Adnl> adnl_;
  td::actor::ActorOwn<adnl::AdnlExtServer> ext_server_;

  struct Server {
    liteclient::LiteServerConfig config;
    td::actor::ActorOwn<adnl::AdnlExtClient> client;
    bool alive = false;
    BlockSeqno last_known_masterchain_seqno = 0;
  };
  std::vector<Server> servers_;

  std::map<int, td::uint32> ls_stats_;  // lite_api ID -> count, 0 for unknown

  BlockSeqno last_known_masterchain_seqno_ = 0;
  tl_object_ptr<lite_api::liteServer_masterchainInfoExt> last_masterchain_info_;

  std::string config_file() const {
    return db_root_ + "/config.json";
  }
};

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);
  td::set_default_failure_signal_handler().ensure();

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  std::string global_config, db_root;
  td::uint16 port = 0;
  PublicKeyHash public_key_hash = PublicKeyHash::zero();
  td::uint32 threads = 4;

  td::OptionParser p;
  p.set_description("Proxy liteserver: distributes incoming queries to servers in global config\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('V', "version", "show build information", [&]() {
    std::cout << "proxy-liteserver build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option('p', "port", "liteserver port (required only on first launch)",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT_ASSIGN(port, td::to_integer_safe<td::uint16>(arg));
                         return td::Status::OK();
                       });
  p.add_checked_option(
      'A', "adnl-id",
      "liteserver public key hash in hex (optional). The corresponding private key is required in <db>/keyring/",
      [&](td::Slice arg) -> td::Status {
        td::Bits256 value;
        if (value.from_hex(arg) != 256) {
          return td::Status::Error("invalid adnl-id");
        }
        public_key_hash = PublicKeyHash{value};
        return td::Status::OK();
      });
  p.add_option('C', "global-config", "global TON configuration file",
               [&](td::Slice arg) { global_config = arg.str(); });
  p.add_option('D', "db", "db root", [&](td::Slice arg) { db_root = arg.str(); });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
    td::set_signal_handler(td::SignalType::HangUp, [](int) {
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
  p.add_checked_option('t', "threads", PSTRING() << "number of threads (default=" << 4 << ")",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT_ASSIGN(threads, td::to_integer_safe<td::uint32>(arg));
                         return td::Status::OK();
                       });

  p.run(argc, argv).ensure();
  td::actor::Scheduler scheduler({threads});

  scheduler.run_in_context([&] {
    td::actor::create_actor<ProxyLiteserver>("proxy-liteserver", global_config, db_root, port, public_key_hash)
        .release();
  });
  while (scheduler.run(1)) {
  }
}
