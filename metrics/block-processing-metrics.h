/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <array>
#include <string_view>

#include "td/utils/Timer.h"

#include "collectors.h"

namespace ton::metrics {

enum class BlockChain : size_t { master, shard };
enum class BlockResult : size_t { ok, error };

enum class CollationExternalOutcome : size_t { filtered, skipped_backpressure, included, rejected, count };

struct CollationWork {
  td::uint64 transactions{0};
  td::uint64 gas{0};
  td::uint64 block_bytes{0};
  td::uint64 collated_data_bytes{0};
  td::uint64 ext_messages_offered{0};
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
  void add_collation(BlockChain chain, BlockResult result, double elapsed, const td::RealCpuTimer::Time &active,
                     double wait_externals) {
    auto &cell = collation_[index(chain, result)];
    cell.total.add(elapsed, active);
    cell.wait_externals.add(wait_externals);
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

  void add_collation_work(BlockChain chain, const CollationWork &work) {
    auto index = static_cast<size_t>(chain);
    collation_transactions_[index].inc(work.transactions);
    collation_gas_[index].inc(work.gas);
    collation_block_bytes_[index].inc(work.block_bytes);
    collation_collated_data_bytes_[index].inc(work.collated_data_bytes);
    collation_ext_messages_offered_[index].inc(work.ext_messages_offered);
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

    auto collect_work = [&](std::string_view name, const std::array<Counter, 2> &values) {
      auto family = collation.with_name(name);
      family.open_family("counter", "total");
      for (auto chain : {BlockChain::master, BlockChain::shard}) {
        family.with_label("chain", chain_name(chain)).push(double(values[static_cast<size_t>(chain)].value()));
      }
    };
    collect_work("transactions", collation_transactions_);
    collect_work("gas", collation_gas_);
    collect_work("block_bytes", collation_block_bytes_);
    collect_work("collated_data_bytes", collation_collated_data_bytes_);
    collect_work("ext_messages_offered", collation_ext_messages_offered_);

    auto want_split = collation.with_name("want_split");
    want_split.open_family("counter", "total");
    for (auto chain : {BlockChain::master, BlockChain::shard}) {
      want_split.with_label("chain", chain_name(chain)).push(double(want_split_[static_cast<size_t>(chain)].value()));
    }

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
      value_ += delta > 0.0 ? delta : 0.0;
      touched_ = true;
    }

    double value() const {
      return value_;
    }

    bool touched() const {
      return touched_;
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
  };

  struct TotalTime : RealCpu {
    CumulativeDouble elapsed;

    void add(double elapsed_value, const td::RealCpuTimer::Time &active) {
      elapsed.add(elapsed_value);
      RealCpu::add(active);
    }
  };

  struct CollationCell {
    TotalTime total;
    CumulativeDouble wait_externals;
    std::array<RealCpu, static_cast<size_t>(CollationPhase::count)> phases;
  };

  struct ValidationCell {
    TotalTime total;
    CumulativeDouble active;
    CumulativeDouble waiting;
    std::array<RealCpu, static_cast<size_t>(ValidationPhase::count)> phases;
  };

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
      push(ctx, "collate", chain, result, collation_phase_names_[phase], cell.phases[phase]);
    }
  }

  static void collect_validation(Context ctx, BlockChain chain, BlockResult result, const ValidationCell &cell) {
    push(ctx, "validate", chain, result, "total", "elapsed", cell.total.elapsed);
    push(ctx, "validate", chain, result, "total", cell.total);
    push(ctx, "validate", chain, result, "active", "elapsed", cell.active);
    push(ctx, "validate", chain, result, "waiting", "elapsed", cell.waiting);
    for (size_t phase = 0; phase < cell.phases.size(); ++phase) {
      push(ctx, "validate", chain, result, validation_phase_names_[phase], cell.phases[phase]);
    }
  }

  std::array<CollationCell, 4> collation_;
  std::array<ValidationCell, 4> validation_;
  std::array<std::array<Counter, static_cast<size_t>(CollationExternalOutcome::count)>, 4> collation_external_;
  std::array<Counter, 2> collation_transactions_;
  std::array<Counter, 2> collation_gas_;
  std::array<Counter, 2> collation_block_bytes_;
  std::array<Counter, 2> collation_collated_data_bytes_;
  std::array<Counter, 2> collation_ext_messages_offered_;
  std::array<Counter, 2> want_split_;
  std::array<std::array<Counter, 5>, 2> overload_;
};

}  // namespace ton::metrics
