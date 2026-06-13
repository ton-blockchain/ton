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

    Copyright 2019-2020 Telegram Systems LLP
*/
#include "http-inbound-connection.h"
#include "http-server.h"

namespace ton {

namespace http {

class HttpServer::StatsCountingCallback : public HttpServer::Callback {
 public:
  StatsCountingCallback(td::actor::ActorId<HttpServer> server, std::shared_ptr<Callback> callback)
      : server_(server), callback_(std::move(callback)) {
  }

  virtual void receive_request(std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload,
                               ResponsePromise promise) override {
    td::actor::send_closure(server_, &HttpServer::update_stats_on_new_request);

    td::Promise P = [promise = std::move(promise), server = server_](td::Result<ResponsePair> result) mutable {
      auto code = result.is_ok() ? std::optional{result.ok().first->code()} : std::nullopt;
      td::actor::send_closure(server, &HttpServer::update_stats_on_response, code);

      promise.set_result(std::move(result));
    };
    callback_->receive_request(std::move(request), std::move(payload), std::move(P));
  }

  virtual void on_connection_close() override {
    td::actor::send_closure(server_, &HttpServer::update_stats_on_connection_close);
    callback_->on_connection_close();
  }

 private:
  td::actor::ActorId<HttpServer> server_;
  std::shared_ptr<Callback> callback_;
};

HttpServer::HttpServer(td::IPAddress address, std::shared_ptr<Callback> callback)
    : address_(address), callback_(std::move(callback)) {
}

void HttpServer::start_up() {
  callback_ = std::make_shared<StatsCountingCallback>(actor_id(this), std::move(callback_));

  class Callback : public td::TcpListener::Callback {
   private:
    td::actor::ActorId<HttpServer> id_;

   public:
    Callback(td::actor::ActorId<HttpServer> id) : id_(id) {
    }

    void accept(td::SocketFd fd) override {
      td::actor::send_closure(id_, &HttpServer::accepted, std::move(fd));
    }
  };

  listener_ = td::actor::create_actor<td::TcpInfiniteListener>(
      td::actor::ActorOptions().with_name("listener").with_poll(), address_.get_port(),
      std::make_unique<Callback>(actor_id(this)), address_.get_ip_host());
}

void HttpServer::accepted(td::SocketFd fd) {
  stats_.connections_active.add(1);
  stats_.connections.inc();
  td::actor::create_actor<HttpInboundConnection>(td::actor::ActorOptions().with_name("inhttpconn").with_poll(),
                                                 std::move(fd), callback_)
      .release();
}

void HttpServer::update_stats_on_new_request() {
  stats_.requests.inc();
}

void HttpServer::update_stats_on_response(std::optional<td::int32> code) {
  stats_.responses.at(code.value_or(-1)).inc();
}

void HttpServer::update_stats_on_connection_close() {
  stats_.connections_active.add(-1);
}

td::IPAddress HttpServer::make_any_address(td::uint16 port) {
  td::IPAddress addr;
  addr.init_ipv4_port("0.0.0.0", port).ensure();
  return addr;
}

}  // namespace http

}  // namespace ton
