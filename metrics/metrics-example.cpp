#include <metrics/prometheus-exporter.h>

using namespace ton;

class ExampleActor : public td::actor::Actor, public virtual metrics::CollectorWrapper {
public:
  ExampleActor() {
    add_collector(collector_.get());
    td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, time_counter_);
    td::actor::send_closure(collector_.get(), &metrics::MultiCollector::add_sync_collector, stack_gauge_);
  }

  void collect(metrics::MetricsPromise P) override;

private:
  td::actor::ActorOwn<metrics::MultiCollector> collector_ = metrics::MultiCollector::create("example");
  std::shared_ptr<metrics::LambdaCounter> time_counter_ = std::make_shared<metrics::LambdaCounter>("current_time_seconds", [] {
    return std::vector{metrics::Sample{.label_set = {}, .value = td::Timestamp::now().at_unix()}};
  }, "Number of seconds passed since January 1, 1970");
  std::shared_ptr<metrics::LambdaGauge> stack_gauge_ = std::make_shared<metrics::LambdaGauge>("current_stack_top_bytes", [] {
    void *stack_top = &stack_top;
    auto stack_top_addr = reinterpret_cast<uintptr_t>(stack_top);
    return std::vector{metrics::Sample{.label_set = {}, .value = static_cast<double>(stack_top_addr)}};
  });
};

void ExampleActor::collect(metrics::MetricsPromise P) {
  CollectorWrapper::collect(std::move(P));
}

int main() {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::actor::Scheduler scheduler({1});
  td::actor::ActorOwn<PrometheusExporter> exporter;
  td::actor::ActorOwn<ExampleActor> example;
  scheduler.run_in_context([&] {
    exporter = PrometheusExporter::listen();
    void (PrometheusExporter::* f)(td::actor::ActorId<PrometheusExporter>) = &PrometheusExporter::register_collector<PrometheusExporter>;
    td::actor::send_closure(exporter.get(), f, exporter.get());

    example = td::actor::create_actor<ExampleActor>("example");
    td::actor::send_closure(exporter.get(), &PrometheusExporter::register_collector<ExampleActor>, example.get());
  });
  scheduler.run();
}