#include "td/actor/coro_utils.h"

#include "metrics-types.h"
#include "prometheus-exporter.h"

namespace ton {

td::actor::ActorOwn<PrometheusExporter> PrometheusExporter::listen(uint16_t port, std::string prefix) {
  return td::actor::create_actor<PrometheusExporter>(PSTRING() << "PROM@0.0.0.0:" << port, port, std::move(prefix));
}

void PrometheusExporter::collect(metrics::MetricsPromise P) {
  CollectorWrapper::collect(std::move(P));
}

PrometheusExporter::PrometheusExporter(uint16_t port, std::string prefix) : port_(port), prefix_(std::move(prefix)) {
  add_collector(collector_.get());
  td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, collectors_);
  td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, collections_total_);
  td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, last_collection_duration_);
  td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, last_collection_timestamp_);
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
  td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_async_collector<http::HttpServer>, http_.get());
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
  td::actor::send_closure(main_collector_.get(), &metrics::MultiCollector::collect, td::make_promise([this, payload, then = now] (td::Result<metrics::MetricSet> R) {
    metrics::MetricSet whole_set = R.move_as_ok();
    auto exposition = metrics::Exposition {.main_set = std::move(whole_set)};
    payload->add_chunk(td::BufferSlice{std::move(exposition).render()});
    payload->complete_parse();
    last_collection_duration_->set(td::Timestamp::now().at_unix() - then);
  }));
}

}