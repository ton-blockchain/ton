#pragma once

#include "http/http-server.h"
#include "td/actor/common.h"
#include "td/actor/coro_task.h"

#include "metrics-collectors.h"

namespace ton {
class PrometheusExporter final : public td::actor::Actor, public virtual metrics::CollectorWrapper {
public:
  static td::actor::ActorOwn<PrometheusExporter> listen(uint16_t port = 9777, std::string prefix = "ton");

  void collect(metrics::MetricsPromise P) override;

  template<std::derived_from<AsyncCollector> A>
  void register_collector(td::actor::ActorId<A> collector);

  explicit PrometheusExporter(uint16_t port, std::string prefix);

private:
  using RequestPtr = std::unique_ptr<http::HttpRequest>;
  using ResponsePtr = std::unique_ptr<http::HttpResponse>;
  using PayloadPtr = std::shared_ptr<http::HttpPayload>;
  using HttpReturn = std::pair<ResponsePtr, PayloadPtr>;

  // To avoid bugs.
  using CollectorWrapper::add_collector;

  class HttpCallback : public http::HttpServer::Callback {
  public:
    explicit HttpCallback(td::actor::ActorId<PrometheusExporter> exporter);

    void receive_request(RequestPtr request, PayloadPtr payload, td::Promise<HttpReturn> promise) override;

  private:
    td::actor::ActorId<PrometheusExporter> exporter_;
  };
  friend HttpCallback;

  void start_up() override;

  using CollectorLambda = std::function<void(metrics::MetricsPromise)>;

  void on_request(RequestPtr request, PayloadPtr payload, td::Promise<HttpReturn> promise);

  uint16_t port_;
  std::string prefix_;
  td::actor::ActorOwn<http::HttpServer> http_ = {};
  td::actor::ActorOwn<metrics::MultiCollector> main_collector_ = metrics::MultiCollector::create(prefix_);

  td::actor::ActorOwn<metrics::MultiCollector> collector_ = metrics::MultiCollector::create("exporter");
  std::shared_ptr<metrics::AtomicGauge<size_t>> collectors_ = std::make_shared<metrics::AtomicGauge<size_t>>("collectors", "Current number of exporter's added collectors.");
  std::shared_ptr<metrics::AtomicCounter<size_t>> collections_total_ = std::make_shared<metrics::AtomicCounter<size_t>>("collections_total", "Total number of collection requests to the exporter.");
  std::shared_ptr<metrics::AtomicGauge<double>> last_collection_duration_ = std::make_shared<metrics::AtomicGauge<double>>("last_collection_duration_seconds", "Duration of the last collection request to the exporter.");
  std::shared_ptr<metrics::AtomicGauge<double>> last_collection_timestamp_ = std::make_shared<metrics::AtomicGauge<double>>("last_collection_timestamp_seconds", "Timestamp of the last collection request to the exporter.");
};

template <std::derived_from<metrics::AsyncCollector> A>
void PrometheusExporter::register_collector(td::actor::ActorId<A> collector) {
  collectors_->add(1);
  td::actor::send_closure(main_collector_.get(), &metrics::MultiCollector::add_async_collector<A>, collector);
}

}