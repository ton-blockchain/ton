#pragma once

#include <functional>

#include "td/actor/PromiseFuture.h"
#include "td/actor/common.h"

#include "metrics-types.h"

namespace ton::metrics {

class Collector {
public:
  virtual MetricSet collect() = 0;
  virtual ~Collector() = default;
};

using MetricsPromise = td::Promise<MetricSet>;

class CollectorActor : public td::actor::Actor {
public:
  CollectorActor() = default;
  virtual void collect(td::Promise<MetricSet> P) = 0;
};

class CollectorWrapperActor : public CollectorActor {
public:
  CollectorWrapperActor() = default;
  void set_collector(std::shared_ptr<Collector> collector);

  void collect(td::Promise<MetricSet> P) final;

private:
  std::shared_ptr<Collector> wrapped_;
};

using SamplerLambda = std::function<std::vector<Sample>()>;

class LambdaGauge : public Collector {
public:
  LambdaGauge(std::string metric_name, SamplerLambda lambda, std::optional<std::string> help = std::nullopt);
  MetricSet collect() final;

private:
  std::string metric_name_;
  SamplerLambda lambda_;
  std::optional<std::string> help_;
};

class LambdaCounter : public Collector {
public:
  LambdaCounter(std::string metric_name, SamplerLambda lambda, std::optional<std::string> help = std::nullopt);
  MetricSet collect() final;

private:
  std::string metric_name_;
  SamplerLambda lambda_;
  std::optional<std::string> help_;
};

using CollectorLambda = std::function<std::vector<MetricFamily>()>;

class LambdaCollector : public Collector {
public:
  explicit LambdaCollector(CollectorLambda lambda);
  MetricSet collect() final;

private:
  CollectorLambda lambda_;
};

class MultiCollector : public Collector {
public:
  explicit MultiCollector(std::string prefix);
  MetricSet collect() final;

  void add_collector(std::shared_ptr<Collector> collector);

private:
  std::string prefix_;
  std::vector<std::shared_ptr<Collector>> collectors_;
};

}