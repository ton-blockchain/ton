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
#include "td/utils/port/user.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/Random.h"
#include "td/utils/FileLog.h"
#include "git.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "auto/tl/ton_api_json.h"
#include "adnl/adnl.h"
#include "lite-client/QueryTraits.h"
#include "lite-client/ext-client.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>

using namespace ton;

class ProxyLiteserver : public td::actor::Actor {
 public:
  ProxyLiteserver(std::string global_config, std::string db_root, td::uint16 port)
      : global_config_(std::move(global_config)), db_root_(std::move(db_root)), port_(port) {
  }

  void start_up() override {
    LOG_CHECK(db_root_ != "") << "db root is not set";
    td::mkdir(db_root_).ensure();
    db_root_ = td::realpath(db_root_).move_as_ok();
    keyring_ = keyring::Keyring::create(db_root_ + "/keyring");

    td::Status S = prepare_local_config();
    if (S.is_error()) {
      LOG(FATAL) << "Local config error: " << S;
    }

    S = create_ext_client();
    if (S.is_error()) {
      LOG(FATAL) << S;
    }

    create_ext_server();
  }

  td::Status prepare_local_config() {
    auto r_conf_data = td::read_file(config_file());
    if (r_conf_data.is_ok()) {
      auto conf_data = r_conf_data.move_as_ok();
      TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");
      TRY_STATUS_PREFIX(ton_api::from_json(*config_, conf_json.get_object()), "json does not fit TL scheme: ");
      TRY_RESULT_PREFIX_ASSIGN(port_, td::narrow_cast_safe<td::uint16>(config_->port_), "invalid port: ");
      TRY_RESULT_PREFIX_ASSIGN(id_, adnl::AdnlNodeIdFull::create(config_->id_), "invalid id: ");
    } else {
      LOG(WARNING) << "First launch, creating local config";
      if (port_ == 0) {
        return td::Status::Error("port is not set");
      }
      config_->port_ = port_;
      auto pk = PrivateKey{privkeys::Ed25519::random()};
      id_ = adnl::AdnlNodeIdFull{pk.compute_public_key()};
      config_->id_ = id_.tl();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), false, [](td::Result<td::Unit> R) {
        if (R.is_error()) {
          LOG(FATAL) << "Failed to store private key";
        }
      });

      auto s = td::json_encode<std::string>(td::ToJson(*config_), true);
      TRY_STATUS_PREFIX(td::write_file(config_file(), s), "failed to write file: ");
    }
    return td::Status::OK();
  }

  td::Status create_ext_client() {
    std::vector<liteclient::ExtClient::LiteServer> servers;
    TRY_RESULT_PREFIX(global_config_data, td::read_file(global_config_), "Failed to read global config: ");
    TRY_RESULT_PREFIX(global_config_json, td::json_decode(global_config_data.as_slice()),
                      "Failed to parse global config: ");
    ton::ton_api::liteclient_config_global gc;
    ton::ton_api::from_json(gc, global_config_json.get_object()).ensure();

    size_t size = gc.liteservers_.size() + gc.liteservers_v2_.size();
    if (size == 0) {
      return td::Status::Error("No liteservers in global config");
    }

    for (auto& s : gc.liteservers_) {
      td::IPAddress addr;
      addr.init_host_port(td::IPAddress::ipv4_to_str(s->ip_), s->port_).ensure();
      liteclient::ExtClient::LiteServer serv;
      serv.address = addr;
      serv.adnl_id = ton::adnl::AdnlNodeIdFull::create(s->id_).move_as_ok();
      servers.push_back(std::move(serv));
    }
    for (auto& s : gc.liteservers_v2_) {
      td::IPAddress addr;
      addr.init_host_port(td::IPAddress::ipv4_to_str(s->ip_), s->port_).ensure();
      liteclient::ExtClient::LiteServer serv;
      serv.address = addr;
      serv.adnl_id = ton::adnl::AdnlNodeIdFull::create(s->id_).move_as_ok();
      serv.is_full = false;
      for (auto& shard : s->shards_) {
        serv.shards.emplace_back(shard->workchain_, (ton::ShardId)shard->shard_);
        CHECK(serv.shards.back().is_valid_ext());
      }
      servers.push_back(std::move(serv));
    }
    class Callback : public liteclient::ExtClient::Callback {};
    ext_client_ = liteclient::ExtClient::create(std::move(servers), td::make_unique<Callback>());
    return td::Status::OK();
  }

  void create_ext_server() {
    adnl_ = adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &adnl::Adnl::add_id, id_, ton::adnl::AdnlAddressList{}, (td::uint8)255);
    td::actor::send_closure(adnl_, &adnl::Adnl::create_ext_server,
                            std::vector<adnl::AdnlNodeIdShort>{id_.compute_short_id()}, std::vector<td::uint16>{port_},
                            [SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<adnl::AdnlExtServer>> R) {
                              R.ensure();
                              td::actor::send_closure(SelfId, &ProxyLiteserver::created_ext_server, R.move_as_ok());
                            });
  }

  void created_ext_server(td::actor::ActorOwn<adnl::AdnlExtServer> s) {
    ext_server_ = std::move(s);
    LOG(WARNING) << "Started proxy liteserver on port " << port_;

    class AdnlCallback : public adnl::Adnl::Callback {
     public:
      AdnlCallback(td::actor::ActorId<liteclient::ExtClient> client) : client_(client) {
      }

      void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
      }
      void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        td::actor::create_actor<QueryWorker>("worker", client_, std::move(data), std::move(promise)).release();
      }

     private:
      td::actor::ActorId<liteclient::ExtClient> client_;
    };
    td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, id_.compute_short_id(),
                            adnl::Adnl::int_to_bytestring(lite_api::liteServer_query::ID),
                            std::make_unique<AdnlCallback>(ext_client_.get()));
  }

  class QueryWorker : public td::actor::Actor {
   public:
    QueryWorker(td::actor::ActorId<liteclient::ExtClient> client, td::BufferSlice data,
                td::Promise<td::BufferSlice> promise)
        : client_(std::move(client)), data_(std::move(data)), promise_(std::move(promise)) {
    }

    void start_up() override {
      auto data = data_.clone();
      auto F = fetch_tl_object<lite_api::liteServer_query>(data, true);
      if (F.is_ok()) {
        data = std::move(F.move_as_ok()->data_);
      } else {
        auto G = fetch_tl_prefix<lite_api::liteServer_queryPrefix>(data, true);
        if (G.is_error()) {
          fatal_error(G.move_as_error());
          return;
        }
      }
      fetch_tl_prefix<lite_api::liteServer_waitMasterchainSeqno>(data, true).ignore();
      auto F2 = fetch_tl_object<ton::lite_api::Function>(std::move(data), true);
      if (F2.is_error()) {
        fatal_error(F2.move_as_error());
        return;
      }
      auto query = F2.move_as_ok();
      lite_api::downcast_call(*query, [&](auto& obj) { shard_ = liteclient::get_query_shard(obj); });

      LOG(INFO) << "Got query: shard=" << shard_.to_str() << " size=" << data_.size();
      td::actor::send_closure(client_, &liteclient::ExtClient::send_query, "q", std::move(data_), shard_,
                              td::Timestamp::in(8.0), [SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
                                td::actor::send_closure(SelfId, &QueryWorker::got_result, std::move(R));
                              });
    }

    void got_result(td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        LOG(INFO) << "Query to shard=" << shard_.to_str() << ": " << R.error();
        promise_.set_value(create_serialize_tl_object<lite_api::liteServer_error>(
            R.error().code(), "gateway error: " + R.error().message().str()));
      } else {
        td::BufferSlice response = R.move_as_ok();
        LOG(INFO) << "Query to shard=" << shard_.to_str() << ": OK, size=" << response.size()
                  << " time=" << timer_.elapsed();
        promise_.set_value(std::move(response));
      }
      stop();
    }

    void fatal_error(td::Status S) {
      promise_.set_error(std::move(S));
      stop();
    }

   private:
    td::actor::ActorId<liteclient::ExtClient> client_;
    td::BufferSlice data_;
    td::Promise<td::BufferSlice> promise_;
    td::Timer timer_ = {};
    ShardIdFull shard_;
  };

 private:
  std::string global_config_;
  std::string db_root_;
  td::uint16 port_;

  tl_object_ptr<ton_api::proxyLiteserver_config> config_ = create_tl_object<ton_api::proxyLiteserver_config>();
  adnl::AdnlNodeIdFull id_;

  td::actor::ActorOwn<keyring::Keyring> keyring_;
  td::actor::ActorOwn<adnl::Adnl> adnl_;
  td::actor::ActorOwn<adnl::AdnlExtServer> ext_server_;
  td::actor::ActorOwn<liteclient::ExtClient> ext_client_;

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
  p.add_checked_option('p', "port", "liteserver port (use only on first launch)", [&](td::Slice arg) -> td::Status {
    TRY_RESULT_ASSIGN(port, td::to_integer_safe<td::uint16>(arg));
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
  p.add_checked_option('t', "threads", PSTRING() << "number of threads (default=" << 4 << ")",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT_ASSIGN(threads, td::to_integer_safe<td::uint32>(arg));
                         return td::Status::OK();
                       });

  p.run(argc, argv).ensure();
  td::actor::Scheduler scheduler({threads});

  scheduler.run_in_context(
      [&] { td::actor::create_actor<ProxyLiteserver>("proxy-liteserver", global_config, db_root, port).release(); });
  while (scheduler.run(1)) {
  }
}
