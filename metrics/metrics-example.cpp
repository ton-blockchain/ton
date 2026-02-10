#include <metrics/prometheus-exporter.h>

using namespace ton;

class ExampleActor : public metrics::CollectorWrapperActor {
public:
  ExampleActor() {
    collectors_->add_collector(time_counter_);
    collectors_->add_collector(stack_gauge_);
    set_collector(collectors_);
  }

private:
  std::shared_ptr<metrics::MultiCollector> collectors_ = std::make_shared<metrics::MultiCollector>("example");
  std::shared_ptr<metrics::LambdaCounter> time_counter_ = std::make_shared<metrics::LambdaCounter>("current_time_seconds", [] {
    return std::vector{metrics::Sample{.label_set = {}, .value = td::Timestamp::now().at_unix()}};
  }, "Number of seconds passed since January 1, 1970");
  std::shared_ptr<metrics::LambdaGauge> stack_gauge_ = std::make_shared<metrics::LambdaGauge>("current_stack_top_bytes", [] {
    void *stack_top = &stack_top;
    auto stack_top_addr = reinterpret_cast<uintptr_t>(stack_top);
    return std::vector{metrics::Sample{.label_set = {}, .value = static_cast<double>(stack_top_addr)}};
  });
};

int main() {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::actor::Scheduler scheduler({1});
  td::actor::ActorOwn<PrometheusExporter> exporter;
  td::actor::ActorOwn<ExampleActor> example;
  scheduler.run_in_context([&] {
    exporter = PrometheusExporter::listen();
    td::actor::send_closure(exporter.get(), &PrometheusExporter::add_collector_actor, exporter.get());

    example = td::actor::create_actor<ExampleActor>("example");
    td::actor::send_closure(exporter.get(), &PrometheusExporter::add_collector_actor, example.get());
  });
  scheduler.run();
}