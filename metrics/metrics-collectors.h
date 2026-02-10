#pragma once

#include <functional>

#include "td/actor/ActorId.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/common.h"
#include "td/actor/coro_task.h"

#include "metrics-types.h"

namespace ton::metrics {

class Collector {
public:
  virtual MetricSet collect() = 0;
  virtual ~Collector() = default;
};

using MetricsPromise = td::Promise<MetricSet>;

// Also implies inheritance from `td::actor::Actor`.
// However, we cannot inherit actor class right here,
// because this inheritance should be virtual (but it is not virtual in other places).
class AsyncCollector {
public:
  virtual void collect(MetricsPromise P) = 0;
  virtual ~AsyncCollector() = default;
};

using AsyncCollectorClosure = std::function<void(MetricsPromise)>;

// Actors with simple metrics collection should virtually inherit this instead.
// Also, they **MUST** trivially override `collect` function, because send_closure implementation is not really handy
// (otherwise `&A::collect` will have type `void (CollectorWrapper::*)(MetricsPromise)`, which is unrelated to
//  `td::actor::Actor` -- so `send_closure`'s inference of member function class will fail).
class CollectorWrapper : public AsyncCollector {
public:
  CollectorWrapper() = default;
  void collect(MetricsPromise P) override;

  template<typename A>
  void add_collector(td::actor::ActorId<A> collector);

private:
  td::actor::Task<MetricSet> collect_coro();

  std::vector<AsyncCollectorClosure> collector_closures_;
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

class MultiCollector : public td::actor::Actor, public AsyncCollector {
public:
  explicit MultiCollector(std::string prefix);
  void collect(MetricsPromise P) override;

  void add_sync_collector(std::shared_ptr<Collector> collector);

  template<std::derived_from<AsyncCollector> A>
  void add_async_collector(td::actor::ActorId<A> collector);

  static td::actor::ActorOwn<MultiCollector> create(std::string prefix);

private:
  std::string prefix_;
  std::vector<std::shared_ptr<Collector>> sync_collectors_ = {};
  std::unique_ptr<CollectorWrapper> async_collector_ = std::make_unique<CollectorWrapper>();
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

template <typename A>
void CollectorWrapper::add_collector(td::actor::ActorId<A> collector) {
  collector_closures_.push_back([collector] (MetricsPromise P) {
    td::actor::send_closure(collector, &A::collect, std::move(P));
  });
}

template <std::derived_from<AsyncCollector> A>
void MultiCollector::add_async_collector(td::actor::ActorId<A> collector) {
  async_collector_->add_collector(std::move(collector));
}

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