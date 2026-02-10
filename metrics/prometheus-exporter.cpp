#include "td/actor/coro_utils.h"

#include "metrics-types.h"
#include "prometheus-exporter.h"

namespace ton {

td::actor::ActorOwn<PrometheusExporter> PrometheusExporter::listen(uint16_t port, std::string prefix) {
  return td::actor::create_actor<PrometheusExporter>(PSTRING() << "PROM@0.0.0.0:" << port, port, std::move(prefix));
}

void PrometheusExporter::add_collector_actor(td::actor::ActorId<metrics::CollectorActor> collector) {
  collectors_size_->add(1);
  collectors_.push_back(std::move(collector));
}

PrometheusExporter::PrometheusExporter(uint16_t port, std::string prefix) : port_(port), prefix_(std::move(prefix)) {
  set_collector(collector_);
  collector_->add_collector(collectors_size_);
  collector_->add_collector(collections_total_);
  collector_->add_collector(last_collection_duration_);
  collector_->add_collector(last_collection_timestamp_);
}

PrometheusExporter::HttpCallback::HttpCallback(td::actor::ActorId<PrometheusExporter> exporter)
  : exporter_(std::move(exporter)) {
}

void PrometheusExporter::HttpCallback::receive_request(RequestPtr request, PayloadPtr payload, td::Promise<HttpReturn> promise) {
  td::actor::send_closure(exporter_, &PrometheusExporter::on_request, std::move(request), std::move(payload), std::move(promise));
}

void PrometheusExporter::start_up(){
  auto callback = std::make_unique<HttpCallback>(actor_id(this));
  http_ = td::actor::create_actor<http::HttpServer>(PSTRING() << "HTTP@0.0.0.0:" << port_, port_, std::move(callback));
}

void PrometheusExporter::on_request(RequestPtr request, PayloadPtr, td::Promise<HttpReturn> promise) {
  std::unique_ptr<http::HttpResponse> response;

  bool ok = true;
  if (request->url() != "/metrics") {
    ok = false;
    response = http::HttpResponse::create("HTTP/1.1", 404, "Not Found", false, false).move_as_ok();
  } else if (request->method() != "GET") {
    ok = false;
    response = http::HttpResponse::create("HTTP/1.1", 405, "Method Not Allowed", false, false).move_as_ok();
  } else {
    response = http::HttpResponse::create("HTTP/1.1", 200, "OK", false, false).move_as_ok();
  }

  response->add_header({"Transfer-Encoding", "Chunked"});
  response->add_header({"Content-Type", "application/openmetrics-text; version=1.0.0; charset=utf-8"});
  response->complete_parse_header();

  auto payload = response->create_empty_payload().move_as_ok();
  promise.set_value(std::pair{std::move(response), payload});

  if (!ok) {
    payload->complete_parse();
    return;
  }

  auto now = td::Timestamp::now().at_unix();
  collections_total_->add(1);
  last_collection_timestamp_->set(now);
  td::actor::send_closure(actor_id(this), &PrometheusExporter::collect_all_metrics, td::make_promise([this, payload, then = now] (td::Result<metrics::MetricSet> R) {
    metrics::MetricSet whole_set = R.move_as_ok();
    auto exposition = metrics::Exposition {.prefix = prefix_, .whole_set = std::move(whole_set)};
    payload->add_chunk(td::BufferSlice{std::move(exposition).render()});
    payload->complete_parse();
    last_collection_duration_->set(td::Timestamp::now().at_unix() - then);
  }));
}

td::actor::Task<metrics::MetricSet> PrometheusExporter::collect_all_metrics() {
  metrics::MetricSet whole_set = {};
  for (const auto &collector : collectors_) {
    metrics::MetricSet metric_set = co_await td::actor::ask(collector, &metrics::CollectorActor::collect);
    whole_set = std::move(whole_set).join(std::move(metric_set));
  }
  co_return whole_set;
}

}