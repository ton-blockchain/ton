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
#pragma once

#include "metrics/collectors.h"
#include "td/actor/actor.h"
#include "td/actor/coro_task.h"
#include "td/net/TcpListener.h"

#include "http.h"

namespace ton {

namespace http {

class HttpInboundConnection;

class HttpServer : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;

    virtual void receive_request(std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload,
                                 ResponsePromise promise) = 0;

    virtual void on_connection_close() {
    }
  };

  HttpServer(td::IPAddress address, std::shared_ptr<Callback> callback);

  HttpServer(td::uint16 port, std::shared_ptr<Callback> callback)
      : HttpServer(make_any_address(port), std::move(callback)) {
  }

  void start_up() override;
  void accepted(td::SocketFd fd);

  static td::actor::ActorOwn<HttpServer> create(td::uint16 port, std::shared_ptr<Callback> callback) {
    return td::actor::create_actor<HttpServer>("httpserver", port, std::move(callback));
  }

  td::actor::Task<> collect(metrics::Context ctx) {
    ctx.collect(stats_, "http_server");
    co_return {};
  }

  struct Stats {
    metrics::Gauge<td::uint64> connections_active;
    metrics::Counter connections;
    metrics::Counter requests;
    metrics::DynLabel<"code", td::int32, metrics::Counter> responses;

    void collect(metrics::Context ctx) const {
      ctx.collect(connections_active, "connections_active");
      ctx.collect(connections, "connections");
      ctx.collect(requests, "requests");
      ctx.collect(responses, "responses");
    }
  };

 private:
  class StatsCountingCallback;

  void update_stats_on_new_request();
  void update_stats_on_response(std::optional<td::int32> code);
  void update_stats_on_connection_close();

  td::IPAddress address_;
  std::shared_ptr<Callback> callback_;

  td::actor::ActorOwn<td::TcpInfiniteListener> listener_;

  Stats stats_;

  static td::IPAddress make_any_address(td::uint16 port);
};

}  // namespace http

}  // namespace ton
