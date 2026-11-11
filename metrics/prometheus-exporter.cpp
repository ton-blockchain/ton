/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"
#include "td/utils/ThreadSafeCounter.h"

#include "prometheus-exporter.h"

namespace ton {

td::actor::ActorOwn<PrometheusExporter> PrometheusExporter::create(std::string prefix) {
  return td::actor::create_actor<PrometheusExporter>(PSTRING() << "PROM@" << prefix, std::move(prefix));
}

PrometheusExporter::PrometheusExporter(std::string prefix) : prefix_(std::move(prefix)) {
}

PrometheusExporter::HttpCallback::HttpCallback(td::actor::ActorId<PrometheusExporter> exporter)
    : exporter_(std::move(exporter)) {
}

void PrometheusExporter::HttpCallback::receive_request(RequestPtr request, PayloadPtr payload,
                                                       http::ResponsePromise promise) {
  td::actor::send_closure(exporter_, &PrometheusExporter::on_request, std::move(request), std::move(payload),
                          std::move(promise));
}

void PrometheusExporter::listen(td::IPAddress addr) {
  CHECK(http_.empty());
  auto callback = std::make_unique<HttpCallback>(actor_id(this));
  http_ = td::actor::create_actor<http::HttpServer>(PSTRING() << "HTTP@" << addr, addr, std::move(callback));
}

void PrometheusExporter::start_up() {
  add(actor_id(this), &PrometheusExporter::collect);
}

td::actor::Task<metrics::MetricSet> PrometheusExporter::gather() {
  metrics::Sink sink;
  auto root = metrics::Context{sink}.with_name(prefix_);  // every metric gets the top prefix (e.g. ton_)
  auto collectors = collectors_;  // add() may run and reallocate collectors_ across a suspension
  for (auto &collector : collectors) {
    co_await collector(root);
  }
  co_return std::move(sink).build();
}

// The registry names each site's pair "<site>.count" / "<site>.duration"; split that back into an
// `op` label so one site is one series per family.
static void emit_perf_family(metrics::Context ctx, td::Slice field) {
  ctx.open_family("counter", "total");
  td::NamedPerfCounter::get_default().for_each([&](td::Slice name, td::int64 value) {
    auto dot = name.rfind('.');
    if (dot != td::Slice::npos && name.substr(dot + 1) == field) {
      ctx.with_label("op", std::string_view(name.substr(0, dot).data(), dot)).push(static_cast<double>(value));
    }
  });
}

void PrometheusExporter::PerfCounters::collect(metrics::Context ctx) const {
  emit_perf_family(ctx.with_name("ops"), "count");
  emit_perf_family(ctx.with_name("op_ticks"), "duration");
}

td::actor::Task<> PrometheusExporter::collect_and_respond() {
  auto started_at = td::UTCClock::now();
  stats_.last_collection_timestamp.set(started_at);
  // Wrapped, not propagated: a collector that fails must not leave the single flight held and the
  // waiting scrapers unanswered, which would wedge this endpoint for good.
  auto r_set = co_await gather().wrap();

  // Taken only now, so a scrape that arrived mid-gather is served by this flight instead of starting
  // its own; emptying the queue is what releases the flight, hence nothing may suspend below.
  std::vector<http::ResponsePromise> waiting;
  waiting.swap(waiting_);

  if (r_set.is_error()) {
    LOG(ERROR) << "failed to collect metrics: " << r_set.error();
    for (auto &promise : waiting) {
      http::answer_error(http::status_internal_server_error, "", std::move(promise));
    }
    co_return {};
  }

  auto body = metrics::Exposition{.main_set = r_set.move_as_ok()}.render();
  stats_.last_collection_duration.set(td::UTCClock::now() - started_at);
  for (auto &promise : waiting) {
    // Built, filled and completed before the connection actor is handed it: the payload is never
    // shared while still mutable, so there is nothing for the two actors to race over.
    auto response = http::HttpResponse::create("HTTP/1.1", 200, "OK", false, false).move_as_ok();
    response->add_header({"Transfer-Encoding", "Chunked"});
    response->add_header({"Content-Type", "application/openmetrics-text; version=1.0.0; charset=utf-8"});
    response->complete_parse_header();
    auto payload = response->create_empty_payload().move_as_ok();
    payload->add_chunk(td::BufferSlice{body});
    payload->complete_parse();
    promise.set_value(std::pair{std::move(response), std::move(payload)});
  }
  co_return {};
}

void PrometheusExporter::on_request(RequestPtr request, PayloadPtr payload, http::ResponsePromise promise) {
  if (request->url() != "/metrics") {
    http::answer_error(http::status_not_found, "", std::move(promise));
    return;
  }
  if (request->method() != "GET") {
    http::answer_error(http::status_method_not_allowed, "", std::move(promise));
    return;
  }

  stats_.collections.inc();
  bool idle = waiting_.empty();
  waiting_.push_back(std::move(promise));
  if (idle) {
    collect_and_respond().start().detach("prometheus collect");
  }
}

}  // namespace ton
