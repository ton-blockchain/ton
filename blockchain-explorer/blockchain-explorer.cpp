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

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "adnl/adnl-ext-client.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/user.h"
#include "td/utils/port/FileFd.h"
#include "ton/ton-tl.hpp"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-auto.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "block/mc-config.h"
#include "blockchain-explorer.hpp"
#include "blockchain-explorer-http.hpp"
#include "blockchain-explorer-query.hpp"

#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/cp0.h"

#include "auto/tl/lite_api.h"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"

#include <microhttpd.h>

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#include <fcntl.h>
#endif
#include <iostream>
#include <sstream>

int verbosity;

td::actor::Scheduler* scheduler_ptr;

static std::string urldecode(td::Slice from, bool decode_plus_sign_as_space) {
  size_t to_i = 0;

  td::BufferSlice x{from.size()};
  auto to = x.as_slice();

  for (size_t from_i = 0, n = from.size(); from_i < n; from_i++) {
    if (from[from_i] == '%' && from_i + 2 < n) {
      int high = td::hex_to_int(from[from_i + 1]);
      int low = td::hex_to_int(from[from_i + 2]);
      if (high < 16 && low < 16) {
        to[to_i++] = static_cast<char>(high * 16 + low);
        from_i += 2;
        continue;
      }
    }
    to[to_i++] = decode_plus_sign_as_space && from[from_i] == '+' ? ' ' : from[from_i];
  }

  return to.truncate(to_i).str();
}

class HttpQueryRunner {
 public:
  HttpQueryRunner(std::function<void(td::Promise<MHD_Response*>)> func) {
    auto P = td::PromiseCreator::lambda([Self = this](td::Result<MHD_Response*> R) {
      if (R.is_ok()) {
        Self->finish(R.move_as_ok());
      } else {
        Self->finish(nullptr);
      }
    });
    mutex_.lock();
    scheduler_ptr->run_in_context_external([&]() { func(std::move(P)); });
  }
  void finish(MHD_Response* response) {
    response_ = response;
    mutex_.unlock();
  }
  MHD_Response* wait() {
    mutex_.lock();
    mutex_.unlock();
    return response_;
  }

 private:
  std::function<void(td::Promise<MHD_Response*>)> func_;
  MHD_Response* response_;
  std::mutex mutex_;
};

class CoreActor : public CoreActorInterface {
 private:
  std::string global_config_ = "ton-global.config";

  std::vector<td::actor::ActorOwn<ton::adnl::AdnlExtClient>> clients_;

  td::uint32 http_port_ = 80;
  MHD_Daemon* daemon_ = nullptr;

  td::IPAddress remote_addr_;
  ton::PublicKey remote_public_key_;

  bool hide_ips_ = false;

  std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback(td::uint32 idx) {
    class Callback : public ton::adnl::AdnlExtClient::Callback {
     public:
      void on_ready() override {
        td::actor::send_closure(id_, &CoreActor::conn_ready, idx_);
      }
      void on_stop_ready() override {
        td::actor::send_closure(id_, &CoreActor::conn_closed, idx_);
      }
      Callback(td::actor::ActorId<CoreActor> id, td::uint32 idx) : id_(std::move(id)), idx_(idx) {
      }

     private:
      td::actor::ActorId<CoreActor> id_;
      td::uint32 idx_;
    };

    return std::make_unique<Callback>(actor_id(this), idx);
  }

  std::shared_ptr<RemoteNodeStatus> new_result_;
  td::int32 attempt_ = 0;
  td::int32 waiting_ = 0;

  std::vector<bool> ready_;

  void run_queries();
  void got_result(td::uint32 idx, td::int32 attempt, td::Result<td::BufferSlice> data);
  void send_query(td::uint32 idx);

  void add_result() {
    if (new_result_) {
      auto ts = static_cast<td::int32>(new_result_->ts_.at_unix());
      results_.emplace(ts, std::move(new_result_));
    }
  }

  void alarm() override {
    auto t = static_cast<td::int32>(td::Clocks::system() / 60);
    if (t <= attempt_) {
      alarm_timestamp() = td::Timestamp::at_unix((attempt_ + 1) * 60);
      return;
    }
    if (waiting_ > 0 && new_result_) {
      add_result();
    }
    attempt_ = t;
    run_queries();
    alarm_timestamp() = td::Timestamp::at_unix((attempt_ + 1) * 60);
  }

 public:
  std::mutex queue_mutex_;
  std::mutex res_mutex_;
  std::map<td::int32, std::shared_ptr<RemoteNodeStatus>> results_;
  std::vector<td::IPAddress> addrs_;
  static CoreActor* instance_;
  td::actor::ActorId<CoreActor> self_id_;

  void conn_ready(td::uint32 idx) {
    ready_.at(idx) = true;
  }
  void conn_closed(td::uint32 idx) {
    ready_.at(idx) = false;
  }
  void set_global_config(std::string str) {
    global_config_ = str;
  }
  void set_http_port(td::uint32 port) {
    http_port_ = port;
  }
  void set_remote_addr(td::IPAddress addr) {
    remote_addr_ = addr;
  }
  void set_remote_public_key(td::BufferSlice file_name) {
    auto R = [&]() -> td::Result<ton::PublicKey> {
      TRY_RESULT_PREFIX(conf_data, td::read_file(file_name.as_slice().str()), "failed to read: ");
      return ton::PublicKey::import(conf_data.as_slice());
    }();

    if (R.is_error()) {
      LOG(FATAL) << "bad server public key: " << R.move_as_error();
    }
    remote_public_key_ = R.move_as_ok();
  }
  void set_hide_ips(bool value) {
    hide_ips_ = value;
  }

  void send_lite_query(td::uint32 idx, td::BufferSlice query, td::Promise<td::BufferSlice> promise);
  void send_lite_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) override {
    return send_lite_query(0, std::move(data), std::move(promise));
  }
  void get_last_result(td::Promise<std::shared_ptr<RemoteNodeStatus>> promise) override {
  }
  void get_results(td::uint32 max, td::Promise<RemoteNodeStatusList> promise) override {
    RemoteNodeStatusList r;
    r.ips = hide_ips_ ? std::vector<td::IPAddress>{addrs_.size()} : addrs_;
    auto it = results_.rbegin();
    while (it != results_.rend() && r.results.size() < max) {
      r.results.push_back(it->second);
      it++;
    }
    promise.set_value(std::move(r));
  }

  void start_up() override {
    instance_ = this;
    auto t = td::Clocks::system();
    attempt_ = static_cast<td::int32>(t / 60);
    auto next_t = (attempt_ + 1) * 60;
    alarm_timestamp() = td::Timestamp::at_unix(next_t);
    self_id_ = actor_id(this);
  }
  void tear_down() override {
    if (daemon_) {
      MHD_stop_daemon(daemon_);
      daemon_ = nullptr;
    }
  }

  CoreActor() {
  }

  static int get_arg_iterate(void* cls, enum MHD_ValueKind kind, const char* key, const char* value) {
    auto X = static_cast<std::map<std::string, std::string>*>(cls);
    if (key && value && std::strlen(key) > 0 && std::strlen(value) > 0) {
      X->emplace(key, urldecode(td::Slice{value}, false));
    }
    return MHD_YES;
  }

  struct HttpRequestExtra {
    HttpRequestExtra(MHD_Connection* connection, bool is_post) {
      if (is_post) {
        postprocessor = MHD_create_post_processor(connection, 1 << 14, iterate_post, static_cast<void*>(this));
      }
    }
    ~HttpRequestExtra() {
      MHD_destroy_post_processor(postprocessor);
    }
    static int iterate_post(void* coninfo_cls, enum MHD_ValueKind kind, const char* key, const char* filename,
                            const char* content_type, const char* transfer_encoding, const char* data, uint64_t off,
                            size_t size) {
      auto ptr = static_cast<HttpRequestExtra*>(coninfo_cls);
      ptr->total_size += strlen(key) + size;
      if (ptr->total_size > MAX_POST_SIZE) {
        return MHD_NO;
      }
      std::string k = key;
      if (ptr->opts[k].size() < off + size) {
        ptr->opts[k].resize(off + size);
      }
      td::MutableSlice(ptr->opts[k]).remove_prefix(off).copy_from(td::Slice(data, size));
      return MHD_YES;
    }
    MHD_PostProcessor* postprocessor;
    std::map<std::string, std::string> opts;
    td::uint64 total_size = 0;
  };

  static void request_completed(void* cls, struct MHD_Connection* connection, void** ptr,
                                enum MHD_RequestTerminationCode toe) {
    auto e = static_cast<HttpRequestExtra*>(*ptr);
    if (e) {
      delete e;
    }
  }

  static int process_http_request(void* cls, struct MHD_Connection* connection, const char* url, const char* method,
                                  const char* version, const char* upload_data, size_t* upload_data_size, void** ptr) {
    struct MHD_Response* response = nullptr;
    int ret;

    bool is_post = false;
    if (std::strcmp(method, "GET") == 0) {
      is_post = false;
    } else if (std::strcmp(method, "POST") == 0) {
      is_post = true;
    } else {
      return MHD_NO; /* unexpected method */
    }
    std::map<std::string, std::string> opts;
    if (!is_post) {
      if (!*ptr) {
        *ptr = static_cast<void*>(new HttpRequestExtra{connection, false});
        return MHD_YES;
      }
      if (0 != *upload_data_size)
        return MHD_NO; /* upload data in a GET!? */
    } else {
      if (!*ptr) {
        *ptr = static_cast<void*>(new HttpRequestExtra{connection, true});
        return MHD_YES;
      }
      auto e = static_cast<HttpRequestExtra*>(*ptr);
      if (0 != *upload_data_size) {
        CHECK(e->postprocessor);
        MHD_post_process(e->postprocessor, upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
      }
      for (auto& o : e->opts) {
        opts[o.first] = std::move(o.second);
      }
    }

    std::string url_s = url;

    *ptr = nullptr; /* clear context pointer */

    auto pos = url_s.rfind('/');
    std::string prefix;
    std::string command;
    if (pos == std::string::npos) {
      prefix = "";
      command = url_s;
    } else {
      prefix = url_s.substr(0, pos + 1);
      command = url_s.substr(pos + 1);
    }

    MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, get_arg_iterate, static_cast<void*>(&opts));

    if (command == "status") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryStatus>("blockinfo", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "block") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryBlockInfo>("blockinfo", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "search") {
      if (opts.count("roothash") + opts.count("filehash") > 0) {
        HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
          td::actor::create_actor<HttpQueryBlockInfo>("blockinfo", opts, prefix, std::move(promise)).release();
        }};
        response = g.wait();
      } else {
        HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
          td::actor::create_actor<HttpQueryBlockSearch>("blocksearch", opts, prefix, std::move(promise)).release();
        }};
        response = g.wait();
      }
    } else if (command == "last") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryViewLastBlock>("", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "download") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryBlockData>("downloadblock", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "viewblock") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryBlockView>("viewblock", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "account") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryViewAccount>("viewaccount", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "transaction") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryViewTransaction>("viewtransaction", opts, prefix, std::move(promise))
            .release();
      }};
      response = g.wait();
    } else if (command == "transaction2") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryViewTransaction2>("viewtransaction2", opts, prefix, std::move(promise))
            .release();
      }};
      response = g.wait();
    } else if (command == "config") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryConfig>("getconfig", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "send") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQuerySend>("send", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "sendform") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQuerySendForm>("sendform", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else if (command == "runmethod") {
      HttpQueryRunner g{[&](td::Promise<MHD_Response*> promise) {
        td::actor::create_actor<HttpQueryRunMethod>("runmethod", opts, prefix, std::move(promise)).release();
      }};
      response = g.wait();
    } else {
      ret = MHD_NO;
    }
    if (response) {
      ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
      MHD_destroy_response(response);
    } else {
      ret = MHD_NO;
    }

    return ret;
  }

  void run() {
    if (remote_public_key_.empty()) {
      auto G = td::read_file(global_config_).move_as_ok();
      auto gc_j = td::json_decode(G.as_slice()).move_as_ok();
      ton::ton_api::liteclient_config_global gc;
      ton::ton_api::from_json(gc, gc_j.get_object()).ensure();

      CHECK(gc.liteservers_.size() > 0);
      td::uint32 size = static_cast<td::uint32>(gc.liteservers_.size());
      ready_.resize(size, false);

      for (td::uint32 i = 0; i < size; i++) {
        auto& cli = gc.liteservers_[i];
        td::IPAddress addr;
        addr.init_host_port(td::IPAddress::ipv4_to_str(cli->ip_), cli->port_).ensure();
        addrs_.push_back(addr);
        clients_.emplace_back(ton::adnl::AdnlExtClient::create(ton::adnl::AdnlNodeIdFull::create(cli->id_).move_as_ok(),
                                                               addr, make_callback(i)));
      }
    } else {
      if (!remote_addr_.is_valid()) {
        LOG(FATAL) << "remote addr not set";
      }
      ready_.resize(1, false);
      addrs_.push_back(remote_addr_);
      clients_.emplace_back(ton::adnl::AdnlExtClient::create(ton::adnl::AdnlNodeIdFull{remote_public_key_},
                                                             remote_addr_, make_callback(0)));
    }
    daemon_ = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, static_cast<td::uint16>(http_port_), nullptr, nullptr,
                               &process_http_request, nullptr, MHD_OPTION_NOTIFY_COMPLETED, request_completed, nullptr,
                               MHD_OPTION_THREAD_POOL_SIZE, 16, MHD_OPTION_END);
    CHECK(daemon_ != nullptr);
  }
};

void CoreActor::got_result(td::uint32 idx, td::int32 attempt, td::Result<td::BufferSlice> R) {
  if (attempt != attempt_) {
    return;
  }
  if (R.is_error()) {
    waiting_--;
    if (waiting_ == 0) {
      add_result();
    }
    return;
  }
  auto data = R.move_as_ok();
  {
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_error>(data.clone(), true);
    if (F.is_ok()) {
      auto f = F.move_as_ok();
      auto err = td::Status::Error(f->code_, f->message_);
      waiting_--;
      if (waiting_ == 0) {
        add_result();
      }
      return;
    }
  }
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true);
  if (F.is_error()) {
    waiting_--;
    if (waiting_ == 0) {
      add_result();
    }
    return;
  }
  auto f = F.move_as_ok();
  new_result_->values_[idx] = ton::create_block_id(f->last_);
  waiting_--;
  CHECK(waiting_ >= 0);
  if (waiting_ == 0) {
    add_result();
  }
}

void CoreActor::send_query(td::uint32 idx) {
  if (!ready_[idx]) {
    return;
  }
  waiting_++;
  auto query = ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>();
  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(serialize_tl_object(query, true));

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), idx, attempt = attempt_](td::Result<td::BufferSlice> R) {
        td::actor::send_closure(SelfId, &CoreActor::got_result, idx, attempt, std::move(R));
      });
  td::actor::send_closure(clients_[idx], &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(q, true),
                          td::Timestamp::in(10.0), std::move(P));
}

void CoreActor::run_queries() {
  waiting_ = 0;
  new_result_ = std::make_shared<RemoteNodeStatus>(ready_.size(), td::Timestamp::at_unix(attempt_ * 60));
  for (td::uint32 i = 0; i < ready_.size(); i++) {
    send_query(i);
  }
  CHECK(waiting_ >= 0);
  if (waiting_ == 0) {
    add_result();
  }
}

void CoreActor::send_lite_query(td::uint32 idx, td::BufferSlice query, td::Promise<td::BufferSlice> promise) {
  if (!ready_[idx]) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "ext conn not ready"));
    return;
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
      return;
    }
    auto B = R.move_as_ok();
    {
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_error>(B.clone(), true);
      if (F.is_ok()) {
        auto f = F.move_as_ok();
        promise.set_error(td::Status::Error(f->code_, f->message_));
        return;
      }
    }
    promise.set_value(std::move(B));
  });
  auto q = ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(query));
  td::actor::send_closure(clients_[idx], &ton::adnl::AdnlExtClient::send_query, "query", serialize_tl_object(q, true),
                          td::Timestamp::in(10.0), std::move(P));
}

td::actor::ActorId<CoreActorInterface> CoreActorInterface::instance_actor_id() {
  auto instance = CoreActor::instance_;
  CHECK(instance);
  return instance->self_id_;
}

CoreActor* CoreActor::instance_ = nullptr;

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<CoreActor> x;

  td::OptionsParser p;
  p.set_description("TON Blockchain explorer");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('I', "hide-ips", "hides ips from status", [&]() {
    td::actor::send_closure(x, &CoreActor::set_hide_ips, true);
    return td::Status::OK();
  });
  p.add_option('u', "user", "change user", [&](td::Slice user) { return td::change_user(user); });
  p.add_option('C', "global-config", "file to read global config", [&](td::Slice fname) {
    td::actor::send_closure(x, &CoreActor::set_global_config, fname.str());
    return td::Status::OK();
  });
  p.add_option('a', "addr", "connect to ip:port", [&](td::Slice arg) {
    td::IPAddress addr;
    TRY_STATUS(addr.init_host_port(arg.str()));
    td::actor::send_closure(x, &CoreActor::set_remote_addr, addr);
    return td::Status::OK();
  });
  p.add_option('p', "pub", "remote public key", [&](td::Slice arg) {
    td::actor::send_closure(x, &CoreActor::set_remote_public_key, td::BufferSlice{arg});
    return td::Status::OK();
  });
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 9) ? td::Status::OK() : td::Status::Error("verbosity must be 0..9");
  });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
    td::set_signal_handler(td::SignalType::HangUp, [](int sig) {
#if TD_DARWIN || TD_LINUX
      close(0);
      setsid();
#endif
    }).ensure();
    return td::Status::OK();
  });
  p.add_option('H', "http-port", "listen on http port", [&](td::Slice arg) {
    td::actor::send_closure(x, &CoreActor::set_http_port, td::to_integer<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_option('L', "local-scripts", "use local copy of ajax/bootstrap/... JS", [&]() {
    local_scripts = true;
    return td::Status::OK();
  });
#if TD_DARWIN || TD_LINUX
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    auto FileLog = td::FileFd::open(td::CSlice(fname.str().c_str()),
                                    td::FileFd::Flags::Create | td::FileFd::Flags::Append | td::FileFd::Flags::Write)
                       .move_as_ok();

    dup2(FileLog.get_native_fd().fd(), 1);
    dup2(FileLog.get_native_fd().fd(), 2);
    return td::Status::OK();
  });
#endif

  vm::init_op_cp0();

  td::actor::Scheduler scheduler({2});
  scheduler_ptr = &scheduler;
  scheduler.run_in_context([&] { x = td::actor::create_actor<CoreActor>("testnode"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] {
    td::actor::send_closure(x, &CoreActor::run);
    x.release();
  });
  scheduler.run();

  return 0;
}
