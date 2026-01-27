/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"
#include "stats.h"

namespace ton::validator::consensus::simplex {

namespace {

class FakeCatchainStatsTag : public ton::stats::Tag {
 public:
  std::string_view name() const override {
    return "fake-catchain";
  }
};

FakeCatchainStatsTag fake_catchain_stats;

class MetricCollectorImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto& bus = *owning_bus();
    collector.emplace(stats::MetricCollector{
        bus.session_id,
        bus.local_id.short_id,
        ton::stats::recorder_for(fake_catchain_stats),
    });
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const TraceEvent> event) {
    auto ev = event->event.get();
    using consensus::stats::CollectibleEvent;
    if (auto collectible = dynamic_cast<const CollectibleEvent<consensus::stats::MetricCollector>*>(ev)) {
      collectible->collect_to(*collector);
    } else if (auto collectible = dynamic_cast<const CollectibleEvent<stats::MetricCollector>*>(ev)) {
      collectible->collect_to(*collector);
    }
  }

 private:
  std::optional<stats::MetricCollector> collector;
};

}  // namespace

void MetricCollector::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<MetricCollectorImpl>("MetricCollector");
}

}  // namespace ton::validator::consensus::simplex
