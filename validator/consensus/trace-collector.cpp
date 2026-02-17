/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "common/stats.h"

#include "bus.h"
#include "stats.h"

namespace ton::validator::consensus {

namespace {

class ConsensusTraceTag : public ton::stats::Tag {
 public:
  std::string_view name() const override {
    return "consensus-trace";
  }
};

ConsensusTraceTag consensus_trace;

class TraceCollectorImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  void start_up() override {
    recorder = ton::stats::recorder_for(consensus_trace);
    id = owning_bus()->session_id;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const TraceEvent> event) {
    events.push_back(create_tl_object<stats::tl::timestampedEvent>(event->event->ts(), event->event->to_tl()));
    alarm_timestamp().relax(td::Timestamp::in(5));
  }

  void tear_down() override {
    flush();
  }

  void alarm() override {
    flush();
  }

 private:
  void flush() {
    if (events.empty()) {
      return;
    }
    auto output = create_tl_object<stats::tl::events>(id, std::move(events));
    recorder->add(std::move(output));
  }

  ValidatorSessionId id;
  std::unique_ptr<ton::stats::Recorder> recorder;
  std::vector<stats::tl::TimestampedEventRef> events;
};

}  // namespace

void TraceCollector::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<TraceCollectorImpl>("TraceCollector");
}

}  // namespace ton::validator::consensus
