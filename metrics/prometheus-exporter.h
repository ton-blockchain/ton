/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <functional>
#include <vector>

#include "http/http-server.h"
#include "td/actor/common.h"
#include "td/actor/coro_utils.h"

#include "collectors.h"

namespace ton {

class PrometheusExporter final : public td::actor::Actor {
 public:
  static td::actor::ActorOwn<PrometheusExporter> create(std::string prefix = "ton");

  explicit PrometheusExporter(std::string prefix);

  // Register an actor whose `collect_fn` coroutine fills the shared Context.
  template <std::derived_from<td::actor::Actor> A>
  void add(td::actor::ActorId<A> actor, td::actor::Task<> (A::*collect_fn)(metrics::Context ctx)) {
    stats_.collectors.add(1);
    collectors_.push_back([actor, collect_fn](metrics::Context ctx) -> td::actor::Task<> {
      co_await td::actor::ask(actor, collect_fn, ctx);
      co_return {};
    });
  }

  void listen(td::IPAddress addr);

  td::actor::Task<> collect(metrics::Context ctx) {
    ctx.collect(stats_, "exporter");
    {
      auto server_ctx = ctx.with_label("server", "exporter");
      co_await td::actor::ask(http_.get(), &http::HttpServer::collect, server_ctx);
    }
    co_return {};
  }

 private:
  using RequestPtr = std::unique_ptr<http::HttpRequest>;
  using PayloadPtr = std::shared_ptr<http::HttpPayload>;

  class HttpCallback : public http::HttpServer::Callback {
   public:
    explicit HttpCallback(td::actor::ActorId<PrometheusExporter> exporter);

    void receive_request(RequestPtr request, PayloadPtr payload, http::ResponsePromise promise) override;

   private:
    td::actor::ActorId<PrometheusExporter> exporter_;
  };
  friend HttpCallback;

  void start_up() override;

  void on_request(RequestPtr request, PayloadPtr payload, http::ResponsePromise promise);

  // Run every registered collector into a single Sink under the top prefix.
  td::actor::Task<metrics::MetricSet> gather();
  // Gather + render OpenMetrics text into the response payload.
  td::actor::Task<> collect_and_respond(PayloadPtr payload, td::UTCTime started_at);

  struct Stats {
    metrics::Gauge<td::uint64> collectors;
    metrics::Counter collections;
    metrics::Gauge<td::UTCClock::duration> last_collection_duration;
    metrics::Gauge<td::UTCTime> last_collection_timestamp;

    void collect(metrics::Context ctx) const {
      ctx.collect(collectors, "collectors");
      ctx.collect(collections, "collections");
      ctx.collect(last_collection_duration, "last_collection_duration");
      ctx.collect(last_collection_timestamp, "last_collection_timestamp");
    }
  };

  Stats stats_;
  std::vector<std::function<td::actor::Task<>(metrics::Context)>> collectors_;

  std::string prefix_;
  td::actor::ActorOwn<http::HttpServer> http_ = {};
};

}  // namespace ton
