/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

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

td::actor::Task<> PrometheusExporter::collect_and_respond() {
  auto started_at = td::UTCClock::now();
  stats_.last_collection_timestamp.set(started_at);
  metrics::MetricSet set = co_await gather();
  auto body = metrics::Exposition{.main_set = std::move(set)}.render();
  stats_.last_collection_duration.set(td::UTCClock::now() - started_at);

  auto waiting = std::move(waiting_);
  waiting_.clear();
  is_gathering_ = false;
  for (auto &payload : waiting) {
    payload->add_chunk(td::BufferSlice{body});
    payload->complete_parse();
  }
  co_return {};
}

void PrometheusExporter::on_request(RequestPtr request, PayloadPtr payload, http::ResponsePromise promise) {
  std::unique_ptr<http::HttpResponse> response;

  if (request->url() != "/metrics") {
    http::answer_error(http::status_not_found, "", std::move(promise));
    return;
  } else if (request->method() != "GET") {
    http::answer_error(http::status_method_not_allowed, "", std::move(promise));
    return;
  } else {
    response = http::HttpResponse::create("HTTP/1.1", 200, "OK", false, false).move_as_ok();
  }

  response->add_header({"Transfer-Encoding", "Chunked"});
  response->add_header({"Content-Type", "application/openmetrics-text; version=1.0.0; charset=utf-8"});
  response->complete_parse_header();

  auto payload_out = response->create_empty_payload().move_as_ok();
  promise.set_value(std::pair{std::move(response), payload_out});

  stats_.collections.inc();
  waiting_.push_back(std::move(payload_out));
  if (!is_gathering_) {
    is_gathering_ = true;
    collect_and_respond().start().detach("prometheus collect");
  }
}

}  // namespace ton
