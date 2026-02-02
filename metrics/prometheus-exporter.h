#pragma once

#include "http/http-server.h"
#include "td/actor/common.h"
#include "td/actor/coro_task.h"

#include "metrics-collectors.h"

namespace ton {
class PrometheusExporter : public td::actor::Actor {
public:
  static td::actor::ActorOwn<PrometheusExporter> listen(uint16_t port = 9777, std::string prefix = "ton");

  void add_collector_actor(td::actor::ActorId<metrics::CollectorActor> collector);

  explicit PrometheusExporter(uint16_t port, std::string prefix);

private:
  using RequestPtr = std::unique_ptr<http::HttpRequest>;
  using ResponsePtr = std::unique_ptr<http::HttpResponse>;
  using PayloadPtr = std::shared_ptr<http::HttpPayload>;
  using HttpReturn = std::pair<ResponsePtr, PayloadPtr>;

  class HttpCallback : public http::HttpServer::Callback {
  public:
    explicit HttpCallback(td::actor::ActorId<PrometheusExporter> exporter);

    void receive_request(RequestPtr request, PayloadPtr payload, td::Promise<HttpReturn> promise) override;

  private:
    td::actor::ActorId<PrometheusExporter> exporter_;
  };
  friend HttpCallback;

  void start_up() override;

  void on_request(RequestPtr request, PayloadPtr payload, td::Promise<HttpReturn> promise);
  td::actor::Task<metrics::MetricSet> collect_all_metrics();

  uint16_t port_;
  std::string prefix_;
  td::actor::ActorOwn<http::HttpServer> http_ = {};
  std::vector<td::actor::ActorId<metrics::CollectorActor>> collectors_;
};
}