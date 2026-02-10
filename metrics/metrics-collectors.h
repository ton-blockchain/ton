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

template<typename ValueType>
class AtomicGauge : public Collector {
public:
  explicit AtomicGauge(std::string name, std::optional<std::string> help = std::nullopt);
  MetricSet collect() final;

  void set(ValueType value);
  void add(ValueType value);

private:
  const std::string name_;
  const std::optional<std::string> help_;
  std::atomic<ValueType> value_ = {ValueType()};
};

template<typename ValueType>
class AtomicCounter : public Collector {
public:
  explicit AtomicCounter(std::string name, std::optional<std::string> help = std::nullopt);
  MetricSet collect() final;

  void set(ValueType value);
  void add(ValueType value);

private:
  const std::string name_;
  const std::optional<std::string> help_;
  std::atomic<ValueType> value_ = {ValueType()};
};

template <typename ValueType>
AtomicGauge<ValueType>::AtomicGauge(std::string name, std::optional<std::string> help) : name_(std::move(name)), help_(std::move(help)) {
}

template <typename ValueType>
MetricSet AtomicGauge<ValueType>::collect() {
  auto value = value_.load();
  return {{MetricFamily::make_scalar(name_, "gauge", value, help_)}};
}

template <typename ValueType>
void AtomicGauge<ValueType>::set(ValueType value) {
  value_.store(value);
}

template <typename ValueType>
void AtomicGauge<ValueType>::add(ValueType value){
  value_.fetch_add(value);
}


template <typename ValueType>
AtomicCounter<ValueType>::AtomicCounter(std::string name, std::optional<std::string> help) : name_(std::move(name)), help_(std::move(help)) {
}

template <typename ValueType>
MetricSet AtomicCounter<ValueType>::collect() {
  auto value = value_.load();
  return {{MetricFamily::make_scalar(name_, "counter", value, help_)}};
}

template <typename ValueType>
void AtomicCounter<ValueType>::set(ValueType value){
  auto old = value_.exchange(value);
  CHECK(value >= old);
}

template <typename ValueType>
void AtomicCounter<ValueType>::add(ValueType value) {
  CHECK(value >= 0);
  value_.fetch_add(value);
}

}