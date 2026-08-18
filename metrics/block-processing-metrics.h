/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

#include "td/utils/Timer.h"

#include "collectors.h"

namespace ton::metrics {

enum class BlockChain : size_t { master, shard };
enum class BlockResult : size_t { ok, error };

// Lets BlockChain also be used as a Labeled<> axis; this file indexes it by hand.
inline constexpr LabelEnum<BlockChain, 2> ton_metric_label(BlockChain) {
  return {"chain", {{"master", "shard"}}};
}

inline constexpr LabelEnum<BlockResult, 2> ton_metric_label(BlockResult) {
  return {"result", {{"ok", "error"}}};
}

#define TON_BLOCK_PROCESSING_OPERATION_LIST(F) \
  F(collate)                                   \
  F(validate)
TON_METRIC_DEFINE_LABEL(BlockProcessingOperation, "operation", TON_BLOCK_PROCESSING_OPERATION_LIST)
#undef TON_BLOCK_PROCESSING_OPERATION_LIST

// Whether the collated block is the first slot of its producer's leader window: it inherits the
// previous producer's backlog and cannot be pipelined behind our own previous collation.
enum class FirstInWindow : size_t { no, yes };

inline constexpr LabelEnum<FirstInWindow, 2> ton_metric_label(FirstInWindow) {
  return {"first", {{"0", "1"}}};
}

#define TON_STORAGE_CACHE_OUTCOME_LIST(F) \
  F(hit)                                  \
  F(miss)                                 \
  F(small)
TON_METRIC_DEFINE_LABEL(StorageCacheOutcome, "outcome", TON_STORAGE_CACHE_OUTCOME_LIST)
#undef TON_STORAGE_CACHE_OUTCOME_LIST

#define TON_BLOCK_PROCESSING_CLOCK_LIST(F) \
  F(elapsed)                               \
  F(real)                                  \
  F(cpu)
TON_METRIC_DEFINE_LABEL(BlockProcessingClock, "clock", TON_BLOCK_PROCESSING_CLOCK_LIST)
#undef TON_BLOCK_PROCESSING_CLOCK_LIST

// Block totals need enough resolution for healthy sub-second work and enough range for the
// one-minute collation/validation deadlines. Keeping this histogram at total-operation granularity
// bounds it to 2 operations * 2 chains * 2 results * 3 clocks = 24 histogram cells (456 classic
// bucket/sum/count series) per exporter; the many detailed phase counters deliberately stay
// cumulative-only, and the window-slot axis lives on cheap counters instead.
inline constexpr std::array<double, 16> kBlockProcessingDurationBuckets = {
    0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0, 60.0, 120.0};

// Mergeable two-bucket retained maximum. A sample remains visible for between one and two
// intervals depending on where it lands, then expires without a timer or scrape-side mutation.
// Block-processing metrics are owned by one actor, but merge support keeps snapshot/delta tests and
// future aggregation honest as long as all inputs use the process-wide monotonic clock.
template <int IntervalSeconds = 10 * 60>
class RecentMax {
 public:
  static_assert(IntervalSeconds > 0, "RecentMax interval must be positive");

  void observe(double value) {
    observe_at(td::Clocks::monotonic(), value);
  }

  void observe_at(double now, double value) {
    if (!std::isfinite(now) || now < 0.0 || !std::isfinite(value) || value < 0.0) {
      return;
    }
    auto generation = generation_at(now);
    if (!generation) {
      return;
    }
    auto &bucket = buckets_[*generation & 1];
    if (bucket.generation != kNoGeneration && *generation < bucket.generation) {
      return;
    }
    if (bucket.generation != *generation) {
      bucket = Bucket{*generation, 0.0};
    }
    bucket.value = std::max(bucket.value, value);
  }

  double value() const {
    return value_at(td::Clocks::monotonic());
  }

  double value_at(double now) const {
    return recent_value_at(now).value_or(0.0);
  }

  std::optional<double> recent_value_at(double now) const {
    if (!std::isfinite(now) || now < 0.0) {
      return std::nullopt;
    }
    auto current = generation_at(now);
    if (!current) {
      return std::nullopt;
    }
    std::optional<double> result;
    for (const auto &bucket : buckets_) {
      if (bucket.generation == *current || (*current > 0 && bucket.generation == *current - 1)) {
        result = std::max(result.value_or(0.0), bucket.value);
      }
    }
    return result;
  }

  RecentMax &operator+=(const RecentMax &other) {
    for (const auto &source : other.buckets_) {
      if (source.generation == kNoGeneration) {
        continue;
      }
      auto &destination = buckets_[source.generation & 1];
      if (destination.generation == source.generation) {
        destination.value = std::max(destination.value, source.value);
      } else if (destination.generation == kNoGeneration || source.generation > destination.generation) {
        destination = source;
      }
    }
    return *this;
  }

  void collect(Context ctx) const {
    ctx.open_family("gauge");
    if (auto recent = recent_value_at(td::Clocks::monotonic())) {
      ctx.push(*recent);
    }
  }

 private:
  static constexpr td::uint64 kNoGeneration = std::numeric_limits<td::uint64>::max();

  static std::optional<td::uint64> generation_at(double now) {
    const double generation = now / static_cast<double>(IntervalSeconds);
    // Converting a floating-point value outside the destination integer's range is undefined.
    // `double(UINT64_MAX)` rounds to 2^64, so every representable value below this limit is safe.
    if (generation >= static_cast<double>(kNoGeneration)) {
      return std::nullopt;
    }
    return static_cast<td::uint64>(generation);
  }

  struct Bucket {
    td::uint64 generation = kNoGeneration;
    double value = 0.0;
  };

  std::array<Bucket, 2> buckets_;
};

enum class CollationExternalOutcome : size_t { filtered, skipped_backpressure, included, rejected, count };

struct CollationWork {
  td::uint64 transactions{0};
  td::uint64 gas{0};
  td::uint64 block_bytes{0};
  td::uint64 collated_data_bytes{0};
  td::uint64 ext_messages_offered{0};
};

struct CollationOutQueue {
  td::uint64 size{0};       // queue depth left behind by this collation
  td::uint64 cleaned{0};    // already-delivered messages dequeued by the cleanup pass
  td::uint64 processed{0};  // inbound internal messages imported from neighbor queues
  td::uint64 skipped{0};    // inbound internal messages we had already processed before
};

// Each F-list is the single source for a phase family: it generates the enum and the exported
// name here, and the work_time member read in ValidatorManagerImpl::log_*_query_stats. The
// enumerator must equal the WorkTimeStats field name; adding a phase is one line here plus the
// field itself.
#define TON_COLLATION_PHASE_LIST(F) \
  F(preinit)                        \
  F(queue_cleanup)                  \
  F(prelim_storage_stat)            \
  F(trx_tvm)                        \
  F(trx_storage_stat)               \
  F(trx_other)                      \
  F(final_storage_stat)             \
  F(enqueue_new_messages)           \
  F(combine_account_transactions)   \
  F(create_shard_state)             \
  F(create_block)                   \
  F(create_collated_data)           \
  F(create_block_candidate)         \
  F(dispatch_queue)                 \
  F(import_internals)               \
  F(import_externals)               \
  F(process_new_msgs)

#define TON_VALIDATION_PHASE_LIST(F) \
  F(unpack_block_candidate)          \
  F(process_mc_state)                \
  F(trx_tvm)                         \
  F(trx_storage_stat)                \
  F(trx_other)                       \
  F(check_transactions_other)        \
  F(unpack_state)                    \
  F(validate_block_tlb)              \
  F(unpack_block_data)               \
  F(precheck_account_updates)        \
  F(precheck_account_transactions)   \
  F(precheck_msg_queue)              \
  F(unpack_dispatch_queue)           \
  F(check_in_msg_descr)              \
  F(check_out_msg_descr)             \
  F(check_dispatch_queue)            \
  F(check_processed_upto)            \
  F(check_in_queue)                  \
  F(check_new_state)

enum class CollationPhase : size_t { TON_COLLATION_PHASE_LIST(TON_METRIC_LABEL_ENUMERATOR_) count };
enum class ValidationPhase : size_t { TON_VALIDATION_PHASE_LIST(TON_METRIC_LABEL_ENUMERATOR_) count };

class BlockProcessingMetrics {
 public:
  void add_collation(BlockChain chain, BlockResult result, FirstInWindow first, double elapsed,
                     const td::RealCpuTimer::Time &active, double wait_externals) {
    auto &cell = collation_[index(chain, result)];
    cell.total.add(elapsed, active);
    cell.wait_externals.add(wait_externals);
    if (result == BlockResult::ok) {
      // Counted over exactly the attempts collated_blocks{result="ok"} counts, so dividing the two
      // gives the mean duration per window slot without a window-slot axis on the histogram.
      collation_elapsed_.at(chain, first).add(elapsed);
    }
    observe_duration(BlockProcessingOperation::collate, chain, result, elapsed, active);
  }

  void add_collation_phase(BlockChain chain, BlockResult result, CollationPhase phase,
                           const td::RealCpuTimer::Time &time) {
    collation_[index(chain, result)].phases[static_cast<size_t>(phase)].add(time);
  }

  void add_validation(BlockChain chain, BlockResult result, double elapsed, const td::RealCpuTimer::Time &active,
                      double active_elapsed) {
    auto &cell = validation_[index(chain, result)];
    cell.total.add(elapsed, active);
    cell.active.add(active_elapsed);
    cell.waiting.add(elapsed - active_elapsed);
    observe_duration(BlockProcessingOperation::validate, chain, result, elapsed, active);
  }

  void add_validation_phase(BlockChain chain, BlockResult result, ValidationPhase phase,
                            const td::RealCpuTimer::Time &time) {
    validation_[index(chain, result)].phases[static_cast<size_t>(phase)].add(time);
  }

  void add_want_split(BlockChain chain) {
    want_split_[static_cast<size_t>(chain)].inc();
  }

  void add_overload(BlockChain chain, int reason) {
    overload_[static_cast<size_t>(chain)][overload_index(reason)].inc();
  }

  void add_collation_external(BlockChain chain, BlockResult result, CollationExternalOutcome outcome,
                              td::uint64 count) {
    collation_external_[index(chain, result)][static_cast<size_t>(outcome)].inc(count);
  }

  void add_collation_work(BlockChain chain, FirstInWindow first, const CollationWork &work) {
    collation_transactions_.at(chain, first).inc(work.transactions);
    collation_gas_.at(chain).inc(work.gas);
    collation_block_bytes_.at(chain).inc(work.block_bytes);
    collation_collated_data_bytes_.at(chain).inc(work.collated_data_bytes);
    collation_ext_messages_offered_.at(chain).inc(work.ext_messages_offered);
  }

  void add_collation_out_queue(BlockChain chain, const CollationOutQueue &queue) {
    auto index = static_cast<size_t>(chain);
    out_queue_size_[index] = queue.size;
    out_queue_cleaned_[index].inc(queue.cleaned);
    out_queue_processed_[index].inc(queue.processed);
    out_queue_skipped_[index].inc(queue.skipped);
  }

  void add_collation_storage_cache(BlockChain chain, StorageCacheOutcome outcome, td::uint64 lookups,
                                   td::uint64 cells) {
    storage_cache_lookups_.at(chain, outcome).inc(lookups);
    storage_cache_cells_.at(chain, outcome).inc(cells);
  }

  BlockProcessingMetrics &operator+=(const BlockProcessingMetrics &other) {
    duration_ += other.duration_;
    duration_recent_max_ += other.duration_recent_max_;
    collation_elapsed_ += other.collation_elapsed_;
    collation_transactions_ += other.collation_transactions_;
    collation_gas_ += other.collation_gas_;
    collation_block_bytes_ += other.collation_block_bytes_;
    collation_collated_data_bytes_ += other.collation_collated_data_bytes_;
    collation_ext_messages_offered_ += other.collation_ext_messages_offered_;
    storage_cache_lookups_ += other.storage_cache_lookups_;
    storage_cache_cells_ += other.storage_cache_cells_;
    for (size_t i = 0; i < collation_.size(); ++i) {
      collation_[i] += other.collation_[i];
      validation_[i] += other.validation_[i];
      for (size_t outcome = 0; outcome < collation_external_[i].size(); ++outcome) {
        collation_external_[i][outcome] += other.collation_external_[i][outcome];
      }
    }
    for (size_t chain = 0; chain < out_queue_size_.size(); ++chain) {
      // A depth snapshot cannot be added up: keep the worst of the merged observations.
      out_queue_size_[chain] = std::max(out_queue_size_[chain], other.out_queue_size_[chain]);
      out_queue_cleaned_[chain] += other.out_queue_cleaned_[chain];
      out_queue_processed_[chain] += other.out_queue_processed_[chain];
      out_queue_skipped_[chain] += other.out_queue_skipped_[chain];
      want_split_[chain] += other.want_split_[chain];
      for (size_t reason = 0; reason < overload_[chain].size(); ++reason) {
        overload_[chain][reason] += other.overload_[chain][reason];
      }
    }
    return *this;
  }

  void collect(Context ctx) const {
    auto block_processing = ctx.with_name("block_processing");
    auto timing = block_processing.with_name("seconds");
    timing.open_family("counter", "total");
    for (auto chain : {BlockChain::master, BlockChain::shard}) {
      for (auto result : {BlockResult::ok, BlockResult::error}) {
        collect_collation(timing, chain, result, collation_[index(chain, result)]);
        collect_validation(timing, chain, result, validation_[index(chain, result)]);
      }
    }
    block_processing.collect(duration_, "duration_seconds");
    block_processing.collect(duration_recent_max_, "duration_recent_max_seconds");
    collect_phase_recent_max(block_processing, td::Clocks::monotonic());

    auto collation = ctx.with_name("collation");
    auto external = collation.with_name("ext_messages");
    external.open_family("counter", "total");
    for (auto chain : {BlockChain::master, BlockChain::shard}) {
      for (auto result : {BlockResult::ok, BlockResult::error}) {
        const auto &cell = collation_external_[index(chain, result)];
        for (size_t outcome = 0; outcome < cell.size(); ++outcome) {
          external.with_label("chain", chain_name(chain))
              .with_label("result", result_name(result))
              .with_label("outcome", collation_external_outcome_names_[outcome])
              .push(double(cell[outcome].value()));
        }
      }
    }

    collation.collect(collation_elapsed_, "elapsed_seconds");
    collation.collect(collation_transactions_, "transactions");
    collation.collect(collation_gas_, "gas");
    collation.collect(collation_block_bytes_, "block_bytes");
    collation.collect(collation_collated_data_bytes_, "collated_data_bytes");
    collation.collect(collation_ext_messages_offered_, "ext_messages_offered");

    auto out_queue = collation.with_name("out_queue_size");
    out_queue.open_family("gauge");
    for (auto chain : {BlockChain::master, BlockChain::shard}) {
      out_queue.with_label("chain", chain_name(chain)).push(double(out_queue_size_[static_cast<size_t>(chain)]));
    }

    auto collect_by_chain = [&](std::string_view name, const std::array<Counter, 2> &values) {
      auto family = collation.with_name(name);
      family.open_family("counter", "total");
      for (auto chain : {BlockChain::master, BlockChain::shard}) {
        family.with_label("chain", chain_name(chain)).push(double(values[static_cast<size_t>(chain)].value()));
      }
    };
    collect_by_chain("out_queue_cleaned", out_queue_cleaned_);
    collect_by_chain("out_queue_processed", out_queue_processed_);
    collect_by_chain("out_queue_skipped", out_queue_skipped_);

    collation.collect(storage_cache_lookups_, "storage_cache_lookups");
    collation.collect(storage_cache_cells_, "storage_cache_cells");

    collect_by_chain("want_split", want_split_);

    auto overload = collation.with_name("overload");
    overload.open_family("counter", "total");
    for (auto chain : {BlockChain::master, BlockChain::shard}) {
      for (size_t reason = 0; reason < overload_names_.size(); ++reason) {
        overload.with_label("chain", chain_name(chain))
            .with_label("reason", overload_names_[reason])
            .push(double(overload_[static_cast<size_t>(chain)][reason].value()));
      }
    }
  }

 private:
  class CumulativeDouble {
   public:
    void add(double delta) {
      if (!std::isfinite(delta)) {
        return;
      }
      value_ += delta > 0.0 ? delta : 0.0;
      touched_ = true;
    }

    double value() const {
      return value_;
    }

    bool touched() const {
      return touched_;
    }

    void collect(Context ctx) const {
      ctx.open_family("counter", "total");
      ctx.push(value_);
    }

    CumulativeDouble &operator+=(const CumulativeDouble &other) {
      value_ += other.value_;
      touched_ = touched_ || other.touched_;
      return *this;
    }

   private:
    double value_{0.0};
    bool touched_{false};
  };

  struct RealCpu {
    CumulativeDouble real;
    CumulativeDouble cpu;

    void add(const td::RealCpuTimer::Time &time) {
      real.add(time.real);
      cpu.add(time.cpu);
    }

    RealCpu &operator+=(const RealCpu &other) {
      real += other.real;
      cpu += other.cpu;
      return *this;
    }
  };

  struct PhaseTime {
    RealCpu cumulative;
    RecentMax<> real_recent_max;
    RecentMax<> cpu_recent_max;

    void add(const td::RealCpuTimer::Time &time) {
      cumulative.add(time);
      observe_recent(real_recent_max, time.real);
      observe_recent(cpu_recent_max, time.cpu);
    }

    PhaseTime &operator+=(const PhaseTime &other) {
      cumulative += other.cumulative;
      real_recent_max += other.real_recent_max;
      cpu_recent_max += other.cpu_recent_max;
      return *this;
    }
  };

  static void observe_recent(RecentMax<> &recent, double seconds) {
    if (std::isfinite(seconds)) {
      recent.observe(std::max(seconds, 0.0));
    }
  }

  struct TotalTime : RealCpu {
    CumulativeDouble elapsed;

    void add(double elapsed_value, const td::RealCpuTimer::Time &active) {
      elapsed.add(elapsed_value);
      RealCpu::add(active);
    }

    TotalTime &operator+=(const TotalTime &other) {
      elapsed += other.elapsed;
      RealCpu::operator+=(other);
      return *this;
    }
  };

  struct CollationCell {
    TotalTime total;
    CumulativeDouble wait_externals;
    std::array<PhaseTime, static_cast<size_t>(CollationPhase::count)> phases;

    CollationCell &operator+=(const CollationCell &other) {
      total += other.total;
      wait_externals += other.wait_externals;
      for (size_t i = 0; i < phases.size(); ++i) {
        phases[i] += other.phases[i];
      }
      return *this;
    }
  };

  struct ValidationCell {
    TotalTime total;
    CumulativeDouble active;
    CumulativeDouble waiting;
    std::array<PhaseTime, static_cast<size_t>(ValidationPhase::count)> phases;

    ValidationCell &operator+=(const ValidationCell &other) {
      total += other.total;
      active += other.active;
      waiting += other.waiting;
      for (size_t i = 0; i < phases.size(); ++i) {
        phases[i] += other.phases[i];
      }
      return *this;
    }
  };

  void observe_duration(BlockProcessingOperation operation, BlockChain chain, BlockResult result, double elapsed,
                        const td::RealCpuTimer::Time &active) {
    auto observe = [&](BlockProcessingClock clock, double seconds) {
      if (!std::isfinite(seconds)) {
        return;
      }
      seconds = std::max(seconds, 0.0);
      duration_.at(operation, chain, result, clock).observe(seconds);
      duration_recent_max_.at(operation, chain, result, clock).observe(seconds);
    };
    observe(BlockProcessingClock::elapsed, elapsed);
    observe(BlockProcessingClock::real, active.real);
    observe(BlockProcessingClock::cpu, active.cpu);
  }

  static void push_phase_recent_max(Context ctx, std::string_view operation, BlockChain chain, BlockResult result,
                                    std::string_view phase, const PhaseTime &value, double now) {
    // Keep every link alive until push(): Context label links point at their parent's stack node.
    auto operation_ctx = ctx.with_label("operation", operation);
    auto chain_ctx = operation_ctx.with_label("chain", chain_name(chain));
    auto result_ctx = chain_ctx.with_label("result", result_name(result));
    auto phase_ctx = result_ctx.with_label("phase", phase);
    if (auto recent = value.real_recent_max.recent_value_at(now)) {
      phase_ctx.with_label("clock", "real").push(*recent);
    }
    if (auto recent = value.cpu_recent_max.recent_value_at(now)) {
      phase_ctx.with_label("clock", "cpu").push(*recent);
    }
  }

  void collect_phase_recent_max(Context ctx, double now) const {
    auto family = ctx.with_name("phase_recent_max_seconds");
    family.open_family("gauge");
    for (auto chain : {BlockChain::master, BlockChain::shard}) {
      for (auto result : {BlockResult::ok, BlockResult::error}) {
        const auto &collation = collation_[index(chain, result)];
        for (size_t phase = 0; phase < collation.phases.size(); ++phase) {
          push_phase_recent_max(family, "collate", chain, result, collation_phase_names_[phase],
                                collation.phases[phase], now);
        }
        const auto &validation = validation_[index(chain, result)];
        for (size_t phase = 0; phase < validation.phases.size(); ++phase) {
          push_phase_recent_max(family, "validate", chain, result, validation_phase_names_[phase],
                                validation.phases[phase], now);
        }
      }
    }
  }

  static constexpr auto collation_phase_names_ =
      std::to_array<std::string_view>({TON_COLLATION_PHASE_LIST(TON_METRIC_LABEL_NAME_)});
  static constexpr auto validation_phase_names_ =
      std::to_array<std::string_view>({TON_VALIDATION_PHASE_LIST(TON_METRIC_LABEL_NAME_)});

  static constexpr std::array<std::string_view, 5> overload_names_ = {"block_limits", "out_msg_queue", "long_collation",
                                                                      "dispatch_queue", "unknown"};

  static constexpr auto collation_external_outcome_names_ = std::to_array<std::string_view>({
      "filtered",
      "skipped_backpressure",
      "included",
      "rejected",
  });
  static_assert(collation_external_outcome_names_.size() == static_cast<size_t>(CollationExternalOutcome::count));

  static constexpr size_t index(BlockChain chain, BlockResult result) {
    return static_cast<size_t>(chain) * 2 + static_cast<size_t>(result);
  }

  static constexpr std::string_view chain_name(BlockChain chain) {
    return chain == BlockChain::master ? "master" : "shard";
  }

  static constexpr std::string_view result_name(BlockResult result) {
    return result == BlockResult::ok ? "ok" : "error";
  }

  static constexpr size_t overload_index(int reason) {
    return reason >= 1 && reason <= 4 ? static_cast<size_t>(reason - 1) : 4;
  }

  static void push(Context ctx, std::string_view operation, BlockChain chain, BlockResult result,
                   std::string_view phase, std::string_view clock, const CumulativeDouble &value) {
    if (value.touched()) {
      ctx.with_label("operation", operation)
          .with_label("chain", chain_name(chain))
          .with_label("result", result_name(result))
          .with_label("phase", phase)
          .with_label("clock", clock)
          .push(value.value());
    }
  }

  static void push(Context ctx, std::string_view operation, BlockChain chain, BlockResult result,
                   std::string_view phase, const RealCpu &value) {
    push(ctx, operation, chain, result, phase, "real", value.real);
    push(ctx, operation, chain, result, phase, "cpu", value.cpu);
  }

  static void collect_collation(Context ctx, BlockChain chain, BlockResult result, const CollationCell &cell) {
    push(ctx, "collate", chain, result, "total", "elapsed", cell.total.elapsed);
    push(ctx, "collate", chain, result, "total", cell.total);
    push(ctx, "collate", chain, result, "wait_externals", "elapsed", cell.wait_externals);
    for (size_t phase = 0; phase < cell.phases.size(); ++phase) {
      push(ctx, "collate", chain, result, collation_phase_names_[phase], cell.phases[phase].cumulative);
    }
  }

  static void collect_validation(Context ctx, BlockChain chain, BlockResult result, const ValidationCell &cell) {
    push(ctx, "validate", chain, result, "total", "elapsed", cell.total.elapsed);
    push(ctx, "validate", chain, result, "total", cell.total);
    push(ctx, "validate", chain, result, "active", "elapsed", cell.active);
    push(ctx, "validate", chain, result, "waiting", "elapsed", cell.waiting);
    for (size_t phase = 0; phase < cell.phases.size(); ++phase) {
      push(ctx, "validate", chain, result, validation_phase_names_[phase], cell.phases[phase].cumulative);
    }
  }

  using Duration = Histogram<kBlockProcessingDurationBuckets>;

  std::array<CollationCell, 4> collation_;
  std::array<ValidationCell, 4> validation_;
  Labeled<Duration, BlockProcessingOperation, BlockChain, BlockResult, BlockProcessingClock> duration_;
  Labeled<RecentMax<>, BlockProcessingOperation, BlockChain, BlockResult, BlockProcessingClock> duration_recent_max_;
  std::array<std::array<Counter, static_cast<size_t>(CollationExternalOutcome::count)>, 4> collation_external_;
  Labeled<CumulativeDouble, BlockChain, FirstInWindow> collation_elapsed_;
  Labeled<Counter, BlockChain, FirstInWindow> collation_transactions_;
  Labeled<Counter, BlockChain> collation_gas_;
  Labeled<Counter, BlockChain> collation_block_bytes_;
  Labeled<Counter, BlockChain> collation_collated_data_bytes_;
  Labeled<Counter, BlockChain> collation_ext_messages_offered_;
  std::array<td::uint64, 2> out_queue_size_{};
  std::array<Counter, 2> out_queue_cleaned_;
  std::array<Counter, 2> out_queue_processed_;
  std::array<Counter, 2> out_queue_skipped_;
  Labeled<Counter, BlockChain, StorageCacheOutcome> storage_cache_lookups_;
  Labeled<Counter, BlockChain, StorageCacheOutcome> storage_cache_cells_;
  std::array<Counter, 2> want_split_;
  std::array<std::array<Counter, 5>, 2> overload_;
};

}  // namespace ton::metrics
