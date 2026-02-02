#include "metrics-collectors.h"

#include <functional>

namespace ton::metrics {

void CollectorWrapperActor::collect(td::Promise<MetricSet> P) {
  CHECK(wrapped_ != nullptr);
  P.set_value(wrapped_->collect());
}

void CollectorWrapperActor::set_collector(std::shared_ptr<Collector> collector) {
  CHECK(wrapped_ == nullptr);
  wrapped_ = std::move(collector);
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

MetricSet MultiCollector::collect() {
  auto whole_set = MetricSet {};
  for (const auto &collector : collectors_) {
    whole_set = std::move(whole_set).join(collector->collect());
  }
  return std::move(whole_set).wrap(prefix_);
}

void MultiCollector::add_collector(std::shared_ptr<Collector> collector) {
  collectors_.push_back(std::move(collector));
}

}