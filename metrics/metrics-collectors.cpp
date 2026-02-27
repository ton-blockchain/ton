#include <functional>

#include "metrics-collectors.h"

namespace ton::metrics {

void CollectorWrapper::collect(MetricsPromise P) {
  connect(std::move(P), collect_coro());
}

td::actor::Task<MetricSet> CollectorWrapper::collect_coro() {
  MetricSet whole_set = {};
  for (auto &f : collector_closures_) {
    auto [future, promise] = td::actor::StartedTask<metrics::MetricSet>::make_bridge();
    f(std::move(promise));
    auto metric_set = co_await std::move(future);
    whole_set = std::move(whole_set).join(std::move(metric_set));
  }
  co_return whole_set;
}

LambdaGauge::LambdaGauge(std::string metric_name, SamplerLambda lambda, std::optional<std::string> help)
    : metric_name_(std::move(metric_name)), lambda_(std::move(lambda)), help_(std::move(help)) {
}

MetricSet LambdaGauge::collect() {
  auto metric = Metric{.suffix = "", .label_set = {}, .samples = lambda_()};
  auto family = MetricFamily{.name = metric_name_, .type = "gauge", .help = help_, .metrics = {std::move(metric)}};
  return MetricSet{.families = {std::move(family)}};
}

LambdaCounter::LambdaCounter(std::string metric_name, SamplerLambda lambda, std::optional<std::string> help)
    : metric_name_(std::move(metric_name)), lambda_(std::move(lambda)), help_(std::move(help)) {
}

MetricSet LambdaCounter::collect() {
  // TODO(avevad): check monotonic increase (as required for a counter)
  auto metric = Metric{.suffix = "", .label_set = {}, .samples = lambda_()};
  auto family = MetricFamily{.name = metric_name_, .type = "counter", .help = help_, .metrics = {std::move(metric)}};
  return MetricSet{.families = {std::move(family)}};
}

LambdaCollector::LambdaCollector(CollectorLambda lambda) : lambda_(std::move(lambda)) {
}

MetricSet LambdaCollector::collect() {
  return {.families = lambda_()};
}

MultiCollector::MultiCollector(std::string prefix) : prefix_(std::move(prefix)) {
}

void MultiCollector::collect(MetricsPromise P) {
  MetricSet whole_set = {};
  for (auto &c : sync_collectors_) {
    auto metric_set = c->collect();
    whole_set = std::move(whole_set).join(std::move(metric_set));
  }
  async_collector_->collect(
      [prefix = this->prefix_, whole_set = std::move(whole_set), P = std::move(P)](td::Result<MetricSet> R) mutable {
        auto metric_set = R.move_as_ok();
        whole_set = std::move(whole_set).join(std::move(metric_set)).wrap(prefix);
        P.set_result(std::move(whole_set));
      });
}

void MultiCollector::add_sync_collector(std::shared_ptr<Collector> collector) {
  sync_collectors_.push_back(std::move(collector));
}

td::actor::ActorOwn<MultiCollector> MultiCollector::create(std::string prefix) {
  return td::actor::create_actor<MultiCollector>(PSTRING() << "MultiCollector:" << prefix, std::move(prefix));
}

}  // namespace ton::metrics
