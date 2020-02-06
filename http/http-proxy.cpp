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

    Copyright 2019-2020 Telegram Systems LLP
*/
#include "http/http-server.h"
#include "http/http-client.h"

#include "td/utils/port/signals.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/FileLog.h"

#include <algorithm>
#include <list>

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif

class HttpProxy;

class HttpRemote : public td::actor::Actor {
 public:
  struct Query {
    std::unique_ptr<ton::http::HttpRequest> request;
    std::shared_ptr<ton::http::HttpPayload> payload;
    td::Timestamp timeout;
    td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>, std::shared_ptr<ton::http::HttpPayload>>> promise;
  };
  HttpRemote(std::string domain, td::actor::ActorId<HttpProxy> proxy) : domain_(std::move(domain)), proxy_(proxy) {
  }
  void start_up() override {
    class Cb : public ton::http::HttpClient::Callback {
     public:
      Cb(td::actor::ActorId<HttpRemote> id) : id_(id) {
      }
      void on_ready() override {
        td::actor::send_closure(id_, &HttpRemote::set_ready, true);
      }
      void on_stop_ready() override {
        td::actor::send_closure(id_, &HttpRemote::set_ready, false);
      }

     private:
      td::actor::ActorId<HttpRemote> id_;
    };
    client_ = ton::http::HttpClient::create_multi(domain_, td::IPAddress(), 1, 1, std::make_shared<Cb>(actor_id(this)));
    fail_at_ = td::Timestamp::in(10.0);
    close_at_ = td::Timestamp::in(60.0);
  }
  void set_ready(bool ready) {
    if (ready == ready_) {
      return;
    }
    ready_ = ready;
    if (!ready) {
      fail_at_ = td::Timestamp::in(10.0);
      alarm_timestamp().relax(fail_at_);
    } else {
      fail_at_ = td::Timestamp::never();
      while (list_.size() > 0) {
        auto q = std::move(list_.front());
        list_.pop_front();
        td::actor::send_closure(client_, &ton::http::HttpClient::send_request, std::move(q.request),
                                std::move(q.payload), q.timeout, std::move(q.promise));
        close_at_ = td::Timestamp::in(60.0);
      }
    }
  }
  void receive_request(
      std::unique_ptr<ton::http::HttpRequest> request, std::shared_ptr<ton::http::HttpPayload> payload,
      td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>, std::shared_ptr<ton::http::HttpPayload>>>
          promise) {
    bool keep = request->keep_alive();
    auto P = td::PromiseCreator::lambda(
        [promise = std::move(promise),
         keep](td::Result<std::pair<std::unique_ptr<ton::http::HttpResponse>, std::shared_ptr<ton::http::HttpPayload>>>
                   R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
          } else {
            auto v = R.move_as_ok();
            v.first->set_keep_alive(keep);
            if (v.second->payload_type() != ton::http::HttpPayload::PayloadType::pt_empty &&
                !v.first->found_content_length() && !v.first->found_transfer_encoding()) {
              v.first->add_header(ton::http::HttpHeader{"Transfer-Encoding", "Chunked"});
            }
            promise.set_value(std::move(v));
          }
        });
    if (ready_) {
      td::actor::send_closure(client_, &ton::http::HttpClient::send_request, std::move(request), std::move(payload),
                              td::Timestamp::in(3.0), std::move(P));
      close_at_ = td::Timestamp::in(60.0);
    } else {
      list_.push_back(Query{std::move(request), std::move(payload), td::Timestamp::in(3.0), std::move(P)});
    }
  }

  void alarm() override;

 private:
  std::string domain_;
  bool ready_ = false;
  td::Timestamp fail_at_;
  td::Timestamp close_at_;
  td::actor::ActorOwn<ton::http::HttpClient> client_;

  std::list<Query> list_;

  td::actor::ActorId<HttpProxy> proxy_;
};

class HttpProxy : public td::actor::Actor {
 public:
  HttpProxy() {
  }

  void set_port(td::uint16 port) {
    if (port_ != 0) {
      LOG(ERROR) << "duplicate port";
      std::_Exit(2);
    }
    port_ = port;
  }

  void run() {
    if (port_ == 0) {
      LOG(ERROR) << "no port specified";
      std::_Exit(2);
    }

    class Cb : public ton::http::HttpServer::Callback {
     public:
      Cb(td::actor::ActorId<HttpProxy> proxy) : proxy_(proxy) {
      }
      void receive_request(
          std::unique_ptr<ton::http::HttpRequest> request, std::shared_ptr<ton::http::HttpPayload> payload,
          td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>, std::shared_ptr<ton::http::HttpPayload>>>
              promise) override {
        td::actor::send_closure(proxy_, &HttpProxy::receive_request, std::move(request), std::move(payload),
                                std::move(promise));
      }

     private:
      td::actor::ActorId<HttpProxy> proxy_;
    };

    server_ = ton::http::HttpServer::create(port_, std::make_shared<Cb>(actor_id(this)));
  }

  void receive_request(
      std::unique_ptr<ton::http::HttpRequest> request, std::shared_ptr<ton::http::HttpPayload> payload,
      td::Promise<std::pair<std::unique_ptr<ton::http::HttpResponse>, std::shared_ptr<ton::http::HttpPayload>>>
          promise) {
    auto host = request->host();
    if (host.size() == 0) {
      host = request->url();
      if (host.size() >= 7 && host.substr(0, 7) == "http://") {
        host = host.substr(7);
      } else if (host.size() >= 8 && host.substr(0, 8) == "https://") {
        host = host.substr(7);
      }
      auto p = host.find('/');
      if (p != std::string::npos) {
        host = host.substr(0, p);
      }
    } else {
      if (host.size() >= 7 && host.substr(0, 7) == "http://") {
        host = host.substr(7);
      } else if (host.size() >= 8 && host.substr(0, 8) == "https://") {
        host = host.substr(7);
      }
      auto p = host.find('/');
      if (p != std::string::npos) {
        host = host.substr(0, p);
      }
    }
    if (host.find(':') == std::string::npos) {
      host = host + ":80";
    }

    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    auto it = clients_.find(host);

    if (it == clients_.end()) {
      auto id = td::actor::create_actor<HttpRemote>("remote", host, actor_id(this));
      it = clients_.emplace(host, std::move(id)).first;
    }

    td::actor::send_closure(it->second, &HttpRemote::receive_request, std::move(request), std::move(payload),
                            std::move(promise));
  }

  void close_client(std::string host) {
    auto it = clients_.find(host);
    CHECK(it != clients_.end());
    clients_.erase(it);
  }

 private:
  td::uint16 port_;

  td::actor::ActorOwn<ton::http::HttpServer> server_;
  std::map<std::string, td::actor::ActorOwn<HttpRemote>> clients_;
};

void HttpRemote::alarm() {
  if (!ready_) {
    if (fail_at_ && fail_at_.is_in_past()) {
      LOG(INFO) << "closing outbound HTTP connection because of upper level request timeout";
      td::actor::send_closure(proxy_, &HttpProxy::close_client, domain_);
      stop();
      return;
    } else {
      alarm_timestamp().relax(fail_at_);
    }
  }
  if (close_at_ && close_at_.is_in_past()) {
    LOG(INFO) << "closing outbound HTTP connection because of idle timeout";
    td::actor::send_closure(proxy_, &HttpProxy::close_client, domain_);
    stop();
    return;
  }
  alarm_timestamp().relax(close_at_);
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<HttpProxy> x;
  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::OptionsParser p;
  p.set_description("simple http proxy");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
    return td::Status::OK();
  });
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('p', "port", "sets listening port", [&](td::Slice arg) -> td::Status {
    TRY_RESULT(port, td::to_integer_safe<td::uint16>(arg));
    td::actor::send_closure(x, &HttpProxy::set_port, port);
    return td::Status::OK();
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
#if TD_DARWIN || TD_LINUX
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    logger_ = td::FileLog::create(fname.str()).move_as_ok();
    td::log_interface = logger_.get();
    return td::Status::OK();
  });
#endif

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] { x = td::actor::create_actor<HttpProxy>("proxymain"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &HttpProxy::run); });
  while (scheduler.run(1)) {
  }

  return 0;
}
