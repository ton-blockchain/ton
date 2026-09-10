/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http/http-server.h"
#include "td/actor/common.h"
#include "td/actor/coro_utils.h"

#include "actor-metrics.h"
#include "collectors.h"

namespace ton {

// Collect-at-scrape Prometheus endpoint: a GET /metrics starts or joins one collection flight and
// is answered from that flight. Concurrent scrapes share its rendered exposition.
//
// ready() seals the collector set. Before the seal /metrics answers 503 and nothing is gathered;
// add() after the seal is a programming error.
class PrometheusExporter final : public td::actor::Actor {
 public:
  static td::actor::ActorOwn<PrometheusExporter> create(std::string prefix = "ton");

  explicit PrometheusExporter(std::string prefix);

  // Register a collector during start-up, before ready().
  template <std::derived_from<td::actor::Actor> A>
  void add(td::actor::ActorId<A> actor, td::actor::Task<> (A::*collect_fn)(metrics::Context ctx)) {
    CHECK(!ready_);
    stats_.collectors.add(1);
    collectors_.push_back([actor, collect_fn](metrics::Context ctx) -> td::actor::Task<> {
      co_await td::actor::ask(actor, collect_fn, ctx);
      co_return {};
    });
  }

  void listen(td::IPAddress addr);

  // Seal the collector set and start answering scrapes. Idempotent.
  void ready();

  td::actor::Task<> collect(metrics::Context ctx) {
    ctx.collect(stats_, "exporter");
    ctx.collect(perf_, "perf");
    ctx.collect(actors_, "actor");
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

  // Run the sealed collectors sequentially into one metric set.
  td::actor::Task<metrics::MetricSet> gather();
  // Gather once and answer everyone who asked while it was running.
  td::actor::Task<> collect_and_respond();

  struct Stats {
    metrics::Gauge<td::uint64> collectors;
    metrics::Counter collections;
    metrics::Gauge<td::SteadyClock::duration> last_collection_duration;

    void collect(metrics::Context ctx) const {
      ctx.collect(collectors, "collectors");
      ctx.collect(collections, "collections");
      ctx.collect(last_collection_duration, "last_collection_duration");
    }
  };

  Stats stats_;

  // Process-global TD_PERF_COUNTER totals. Durations are raw rdtsc ticks.
  struct PerfCounters {
    void collect(metrics::Context ctx) const;
  };
  PerfCounters perf_;
  metrics::ActorMetrics actors_;
  std::vector<std::function<td::actor::Task<>(metrics::Context)>> collectors_;

  // A gather fans out over every peer pair, connection and overlay, so concurrent or retrying
  // scrapers must share one: they all wait here and are served from its output. Non-empty exactly
  // while a gather is in flight, which is what makes the flight single.
  std::vector<http::ResponsePromise> waiting_;

  // Set by ready(); add() rejects every subsequent registration.
  bool ready_ = false;

  std::string prefix_;
  td::actor::ActorOwn<http::HttpServer> http_ = {};
};

}  // namespace ton
