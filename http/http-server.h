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

#include "metrics/metrics-collectors.h"
#include "td/actor/actor.h"
#include "td/net/TcpListener.h"

#include "http.h"

namespace ton {

namespace http {

class HttpInboundConnection;

class HttpServer : public td::actor::Actor, public virtual metrics::CollectorWrapper {
 public:
  class Callback {
   public:
    virtual ~Callback() = default;
    virtual void receive_request(
        std::unique_ptr<HttpRequest> request, std::shared_ptr<HttpPayload> payload,
        td::Promise<std::pair<std::unique_ptr<HttpResponse>, std::shared_ptr<HttpPayload>>> promise) = 0;
  };

  HttpServer(td::uint16 port, std::shared_ptr<Callback> callback) : port_(port), callback_(std::move(callback)) {
    add_collector(collector_.get());
    td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, metrics_.connections);
    td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, metrics_.connections_total);
    td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, metrics_.requests_total);
    td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, metrics_.responses_total);
  }

  void collect(metrics::MetricsPromise P) override { CollectorWrapper::collect(std::move(P)); }

  void start_up() override;
  void accepted(td::SocketFd fd);

  static td::actor::ActorOwn<HttpServer> create(td::uint16 port, std::shared_ptr<Callback> callback) {
    return td::actor::create_actor<HttpServer>("httpserver", port, std::move(callback));
  }

  struct AllMetrics {
    metrics::AtomicGauge<size_t>::Ptr connections = std::make_shared<metrics::AtomicGauge<size_t>>("connections", "Current number of HTTP connections.");
    metrics::AtomicCounter<size_t>::Ptr connections_total = std::make_shared<metrics::AtomicCounter<size_t>>("connections_total", "Total number of HTTP connections encountered.");
    metrics::AtomicCounter<size_t>::Ptr requests_total = std::make_shared<metrics::AtomicCounter<size_t>>("requests_total", "Total number of HTTP requests received.");
    metrics::Labeled<td::uint32, metrics::AtomicCounter<size_t>>::Ptr responses_total = metrics::Labeled<td::uint32, metrics::AtomicCounter<size_t>>::make("code", "responses_total", "Total number of HTTP responses sent.");
  };

 private:
  td::uint16 port_;
  std::shared_ptr<Callback> callback_;

  td::actor::ActorOwn<td::TcpInfiniteListener> listener_;

  td::actor::ActorOwn<metrics::MultiCollector> collector_ = metrics::MultiCollector::create("http_server");
  AllMetrics metrics_{};
};

}  // namespace http

}  // namespace ton
