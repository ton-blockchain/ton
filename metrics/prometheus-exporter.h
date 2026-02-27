#pragma once

#include "http/http-server.h"
#include "td/actor/common.h"
#include "td/actor/coro_task.h"

#include "metrics-collectors.h"

namespace ton {
class PrometheusExporter final : public td::actor::Actor, public virtual metrics::CollectorWrapper {
 public:
  static td::actor::ActorOwn<PrometheusExporter> create(std::string prefix = "ton");

  void collect(metrics::MetricsPromise P) override;

  template <std::derived_from<metrics::AsyncCollector> A>
  void register_collector(td::actor::ActorId<A> collector);

  void listen(td::IPAddress addr);

  explicit PrometheusExporter(std::string prefix);

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

  using CollectorLambda = std::function<void(metrics::MetricsPromise)>;

  void on_request(RequestPtr request, PayloadPtr payload, td::Promise<HttpReturn> promise);

  std::string prefix_;
  td::actor::ActorOwn<http::HttpServer> http_ = {};
  td::actor::ActorOwn<metrics::MultiCollector> main_collector_ = metrics::MultiCollector::create(prefix_);

  metrics::MultiCollector::Own collector_ = metrics::MultiCollector::create("exporter");
  metrics::AtomicGauge<size_t>::Ptr collectors_ =
      metrics::AtomicGauge<size_t>::make("collectors", "Current number of exporter's added collectors.");
  metrics::AtomicCounter<size_t>::Ptr collections_total_ =
      metrics::AtomicCounter<size_t>::make("collections_total", "Total number of collection requests to the exporter.");
  metrics::AtomicGauge<double>::Ptr last_collection_duration_ = metrics::AtomicGauge<double>::make(
      "last_collection_duration_seconds", "Duration of the last collection request to the exporter.");
  metrics::AtomicGauge<double>::Ptr last_collection_timestamp_ = metrics::AtomicGauge<double>::make(
      "last_collection_timestamp_seconds", "Timestamp of the last collection request to the exporter.");
};

template <std::derived_from<metrics::AsyncCollector> A>
void PrometheusExporter::register_collector(td::actor::ActorId<A> collector) {
  collectors_->add(1);
  td::actor::send_closure(main_collector_.get(), &metrics::MultiCollector::add_async_collector<A>, collector);
}

}  // namespace ton
