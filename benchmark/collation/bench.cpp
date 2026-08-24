/*
    Manual all-in-memory benchmark for the production Collator and
    ValidateQuery.  See --help for the two independently measured modes.

    The fixture is a basechain state registered by a matching masterchain zero
    state.  It exercises a real million-account Patricia dictionary, Wallet V5
    and jetton-wallet TVM execution, Merkle updates, block serialization, and
    full validation.  No CellDb or filesystem request is allowed after fixture
    construction.
*/

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/collation/corpus.h"
#include "benchmark/collation/fixture.h"
#include "block/block-auto.h"
#include "block/block-db.h"
#include "block/block-parse.h"
#include "block/transaction.h"
#include "td/actor/actor.h"
#include "td/actor/coro_utils.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Timer.h"
#include "td/utils/crypto.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "validator/fabric.h"
#include "validator/manager-hardfork.hpp"
#include "validator/validator-options.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/vm.h"

namespace bench::collation {
namespace {

enum class Mode { Collate, Validate };
enum class ValidationVariant { None, Collated, Preloaded };

struct Config {
  FixtureConfig fixture;
  std::string corpus_in;
  std::string corpus_out;
  std::string candidate_in;
  td::optional<td::Bits256> rand_seed;
  Mode mode{Mode::Collate};
  int warmup{1};
  int iterations{15};
  int scheduler_threads{1};
  double timeout_s{120.0};
  bool parallel_validation{false};
  bool verbose{false};
  // --workload was given explicitly; a loaded corpus always wins, so the flag
  // only selects the generated workload and is cross-checked against a corpus.
  bool workload_selected{false};
  ValidationVariant validation_input{ValidationVariant::Collated};
};

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::Collate:
      return "collate";
    case Mode::Validate:
      return "validate";
  }
  UNREACHABLE();
}

const char* validation_variant_name(ValidationVariant variant) {
  switch (variant) {
    case ValidationVariant::None:
      return "none";
    case ValidationVariant::Collated:
      return "collated";
    case ValidationVariant::Preloaded:
      return "preloaded";
  }
  UNREACHABLE();
}

td::Result<Mode> parse_mode(td::Slice value) {
  if (value == "collate") {
    return Mode::Collate;
  }
  if (value == "validate") {
    return Mode::Validate;
  }
  return td::Status::Error("--mode must be collate or validate");
}

td::Result<Workload> parse_workload(td::Slice value) {
  if (value == "jetton") {
    return Workload::Jetton;
  }
  if (value == "transfer") {
    return Workload::Transfer;
  }
  return td::Status::Error("--workload must be jetton or transfer");
}

td::Result<ValidationVariant> parse_validation_input(td::Slice value) {
  if (value == "collated") {
    return ValidationVariant::Collated;
  }
  if (value == "preloaded") {
    return ValidationVariant::Preloaded;
  }
  return td::Status::Error("--validation-input must be collated or preloaded");
}

td::Result<td::Bits256> parse_bits256_hex(td::Slice value) {
  TRY_RESULT(decoded, td::hex_decode(value));
  if (decoded.size() != 32) {
    return td::Status::Error("expected exactly 32 hex-encoded bytes");
  }
  td::Bits256 result;
  result.as_slice().copy_from(decoded);
  return result;
}

td::Bits256 derive_corpus_rand_seed(td::uint64 seed) {
  constexpr char prefix[] = "ton-collation-corpus-seed";
  unsigned char input[sizeof(prefix) - 1 + 8]{};
  std::copy(prefix, prefix + sizeof(prefix) - 1, input);
  for (int i = 0; i < 8; ++i) {
    input[sizeof(prefix) - 1 + i] = static_cast<unsigned char>(seed >> (8 * i));
  }
  td::Bits256 result;
  td::sha256(td::Slice{input, sizeof(input)}, result.as_slice());
  return result;
}

td::Result<Config> parse_config(int argc, char** argv) {
  Config config;
  config.fixture.zerostate_path = TON_SOURCE_DIR "/benchmark/collation/fixtures/zerostate.boc";
  config.fixture.base_state_path = TON_SOURCE_DIR "/benchmark/collation/fixtures/basestate0.boc";
  config.fixture.contracts_dir = TON_SOURCE_DIR "/benchmark/contracts";

  td::OptionParser parser;
  parser.set_description(
      "All-in-memory production Collator/ValidateQuery benchmark over deterministic basechain Wallet V5 transfers "
      "(jetton or plain)");
  parser.add_option('h', "help", "print help", [&] {
    std::cout << (PSLICE() << parser).c_str() << '\n';
    std::exit(0);
  });
  parser.add_checked_option('m', "mode", "collate|validate (default collate)", [&](td::Slice value) {
    TRY_RESULT(mode, parse_mode(value));
    config.mode = mode;
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "validation-input", "collated|preloaded (default collated)", [&](td::Slice value) {
    TRY_RESULT(input, parse_validation_input(value));
    config.validation_input = input;
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "workload",
                            "jetton|transfer external payload to generate (default jetton; identical account "
                            "state).  --corpus-in takes the workload from the corpus instead",
                            [&](td::Slice value) {
                              TRY_RESULT(workload, parse_workload(value));
                              config.fixture.workload = workload;
                              config.workload_selected = true;
                              return td::Status::OK();
                            });
  parser.add_checked_option('n', "accounts", "injected accounts (default 1000000)", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(config.fixture.accounts, td::to_integer_safe<td::uint64>(value));
    return td::Status::OK();
  });
  parser.add_checked_option('x', "transfers", "wallet transfers per block (default 128, maximum 499)",
                            [&](td::Slice value) {
                              TRY_RESULT_ASSIGN(config.fixture.transfers, td::to_integer_safe<td::uint64>(value));
                              return td::Status::OK();
                            });
  parser.add_checked_option('i', "iterations", "measured iterations per selected mode (default 15)",
                            [&](td::Slice value) {
                              TRY_RESULT_ASSIGN(config.iterations, td::to_integer_safe<int>(value));
                              return td::Status::OK();
                            });
  parser.add_checked_option('w', "warmup", "unmeasured mode-specific iterations (default 1)", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(config.warmup, td::to_integer_safe<int>(value));
    return td::Status::OK();
  });
  parser.add_checked_option('t', "threads", "actor scheduler threads (default 1)", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(config.scheduler_threads, td::to_integer_safe<int>(value));
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "timeout", "query timeout in seconds (default 120)", [&](td::Slice value) {
    TRY_RESULT(timeout_s, td::to_integer_safe<int>(value));
    config.timeout_s = timeout_s;
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "seed", "deterministic uint64 fixture seed (default 1)", [&](td::Slice value) {
    TRY_RESULT_ASSIGN(config.fixture.seed, td::to_integer_safe<td::uint64>(value));
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "rand-seed", "fixed 32-byte block random seed in hex", [&](td::Slice value) {
    TRY_RESULT(seed, parse_bits256_hex(value));
    config.rand_seed = seed;
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "zerostate", "input masterchain zerostate BoC", [&](td::Slice value) {
    config.fixture.zerostate_path = value.str();
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "base-state", "input basechain zero-state BoC", [&](td::Slice value) {
    config.fixture.base_state_path = value.str();
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "contracts", "directory containing Wallet V5 and jetton contract BoCs",
                            [&](td::Slice value) {
                              config.fixture.contracts_dir = value.str();
                              return td::Status::OK();
                            });
  parser.add_checked_option('\0', "corpus-in", "load a versioned on-disk benchmark corpus", [&](td::Slice value) {
    config.corpus_in = value.str();
    return td::Status::OK();
  });
  parser.add_checked_option('\0', "corpus-out", "write the prepared fixture and golden candidates as a corpus",
                            [&](td::Slice value) {
                              config.corpus_out = value.str();
                              return td::Status::OK();
                            });
  parser.add_checked_option('\0', "candidate-in", "validate foreign block.boc + collated.boc against a corpus",
                            [&](td::Slice value) {
                              config.candidate_in = value.str();
                              return td::Status::OK();
                            });
  parser.add_option('\0', "parallel-validation", "enable parallel account validation",
                    [&] { config.parallel_validation = true; });
  parser.add_option('v', "verbose", "print per-iteration samples and per-stage attribution",
                    [&] { config.verbose = true; });
  TRY_STATUS(parser.run(argc, argv));

  if (!config.corpus_in.empty() && !config.corpus_out.empty()) {
    return td::Status::Error("--corpus-in and --corpus-out are mutually exclusive");
  }
  if (!config.candidate_in.empty() && config.corpus_in.empty()) {
    return td::Status::Error("--candidate-in requires --corpus-in");
  }
  if (!config.candidate_in.empty() && config.mode != Mode::Validate) {
    return td::Status::Error("--candidate-in requires --mode validate");
  }
  if (!config.corpus_out.empty() && !config.rand_seed) {
    config.rand_seed = derive_corpus_rand_seed(config.fixture.seed);
  }
  if (config.rand_seed && config.rand_seed.value().is_zero()) {
    return td::Status::Error("--rand-seed must not be all zeroes");
  }

  if (config.fixture.accounts == 0 || config.fixture.transfers == 0) {
    return td::Status::Error("--accounts and --transfers must be positive");
  }
  if (config.fixture.transfers >= 500) {
    return td::Status::Error("--transfers must be below the collator external queue capacity (500)");
  }
  if (config.fixture.transfers > (std::numeric_limits<td::uint64>::max() - 1) / 4 ||
      config.fixture.accounts < config.fixture.transfers * 4 + 1) {
    return td::Status::Error("--accounts must be at least four times --transfers plus the minter");
  }
  if (config.iterations <= 0 || config.warmup < 0 || config.scheduler_threads <= 0 || config.timeout_s <= 0.0) {
    return td::Status::Error("iterations, threads, and timeout must be positive; warmup must be non-negative");
  }
  return config;
}

class InMemoryManager final : public ton::validator::ValidatorManagerImpl {
 public:
  explicit InMemoryManager(const Fixture& fixture)
      : ValidatorManagerImpl(ton::validator::ValidatorManagerOptions::create(fixture.mc_id, fixture.mc_id),
                             fixture.mc_id, "")
      , mc_state_(fixture.mc_state)
      , prev_state_(fixture.prev_state)
      , mc_id_(fixture.mc_id)
      , prev_id_(fixture.prev_id)
      , base_shard_(fixture.shard)
      , mc_block_data_(fixture.mc_block_data)
      , messages_(fixture.messages) {
    auto mc_queue = mc_state_->message_queue();
    CHECK(mc_queue.is_ok());
    mc_message_queue_ = mc_queue.move_as_ok();
    auto prev_queue = prev_state_->message_queue();
    CHECK(prev_queue.is_ok());
    prev_message_queue_ = prev_queue.move_as_ok();
  }

  void wait_collation_stats(td::Promise<ton::validator::CollationStats> promise) {
    if (!collation_stats_.empty()) {
      auto stats = std::move(collation_stats_.front());
      collation_stats_.pop_front();
      promise.set_value(std::move(stats));
      return;
    }
    CHECK(!collation_stats_waiter_);
    collation_stats_waiter_ = std::move(promise);
  }

  void wait_validation_stats(td::Promise<ton::validator::ValidationStats> promise) {
    if (!validation_stats_.empty()) {
      auto stats = std::move(validation_stats_.front());
      validation_stats_.pop_front();
      promise.set_value(std::move(stats));
      return;
    }
    CHECK(!validation_stats_waiter_);
    validation_stats_waiter_ = std::move(promise);
  }

  void log_collate_query_stats(ton::validator::CollationStats stats) override {
    if (collation_stats_waiter_) {
      auto promise = std::move(collation_stats_waiter_);
      promise.set_value(std::move(stats));
    } else {
      collation_stats_.push_back(std::move(stats));
    }
  }

  void log_validate_query_stats(ton::validator::ValidationStats stats) override {
    if (validation_stats_waiter_) {
      auto promise = std::move(validation_stats_waiter_);
      promise.set_value(std::move(stats));
    } else {
      validation_stats_.push_back(std::move(stats));
    }
  }

  void get_top_masterchain_state(td::Promise<td::Ref<ton::validator::MasterchainState>> promise) override {
    promise.set_value(td::Ref<ton::validator::MasterchainState>{mc_state_});
  }

  void get_top_masterchain_block(td::Promise<ton::BlockIdExt> promise) override {
    promise.set_value(ton::BlockIdExt{mc_id_});
  }

  void get_top_masterchain_state_block(
      td::Promise<std::pair<td::Ref<ton::validator::MasterchainState>, ton::BlockIdExt>> promise) override {
    promise.set_value(std::make_pair(mc_state_, mc_id_));
  }

  void get_block_handle(ton::BlockIdExt id, bool, td::Promise<ton::validator::BlockHandle> promise) override {
    if (id != mc_id_ && id != prev_id_) {
      promise.set_error(td::Status::Error("in-memory manager has no block handle for " + id.to_str()));
      return;
    }
    auto handle = ton::validator::create_empty_block_handle(id);
    if (id.is_masterchain()) {
      handle->set_proof();
    }
    handle->flushed_upto(handle->version());
    promise.set_value(std::move(handle));
  }

  void wait_block_state(ton::validator::BlockHandle handle, td::uint32, td::Timestamp, bool,
                        td::Promise<td::Ref<ton::validator::ShardState>> promise) override {
    return_state(handle->id(), std::move(promise));
  }

  void wait_block_state_short(ton::BlockIdExt id, td::uint32, td::Timestamp, bool,
                              td::Promise<td::Ref<ton::validator::ShardState>> promise) override {
    return_state(id, std::move(promise));
  }

  void get_shard_state_from_db(ton::validator::ConstBlockHandle handle,
                               td::Promise<td::Ref<ton::validator::ShardState>> promise) override {
    return_state(handle->id(), std::move(promise));
  }

  void get_shard_state_from_db_short(ton::BlockIdExt id,
                                     td::Promise<td::Ref<ton::validator::ShardState>> promise) override {
    return_state(id, std::move(promise));
  }

  void wait_block_message_queue(ton::validator::BlockHandle handle, td::uint32, td::Timestamp,
                                td::Promise<td::Ref<ton::validator::MessageQueue>> promise) override {
    return_message_queue(handle->id(), std::move(promise));
  }

  void wait_block_message_queue_short(ton::BlockIdExt id, td::uint32, td::Timestamp,
                                      td::Promise<td::Ref<ton::validator::MessageQueue>> promise) override {
    return_message_queue(id, std::move(promise));
  }

  void wait_block_data(ton::validator::BlockHandle handle, td::uint32, td::Timestamp,
                       td::Promise<td::Ref<ton::validator::BlockData>> promise) override {
    return_block_data(handle->id(), std::move(promise));
  }

  void wait_block_data_short(ton::BlockIdExt id, td::uint32, td::Timestamp,
                             td::Promise<td::Ref<ton::validator::BlockData>> promise) override {
    return_block_data(id, std::move(promise));
  }

  void wait_neighbor_msg_queue_proofs(
      ton::ShardIdFull, std::vector<ton::BlockIdExt> blocks, td::Timestamp,
      td::Promise<std::map<ton::BlockIdExt, td::Ref<ton::validator::OutMsgQueueProof>>> promise) override {
    if (mc_state_.is_null() ||
        std::any_of(blocks.begin(), blocks.end(), [&](const auto& block) { return block != mc_id_; })) {
      promise.set_error(td::Status::Error("in-memory manager cannot provide a partial or unknown neighbor proof"));
      return;
    }
    std::map<ton::BlockIdExt, td::Ref<ton::validator::OutMsgQueueProof>> result;
    if (!blocks.empty()) {
      result.emplace(mc_id_, td::Ref<ton::validator::OutMsgQueueProof>{true, mc_id_, mc_state_->root_cell(),
                                                                       td::Ref<vm::Cell>{}, true});
    }
    promise.set_value(std::move(result));
  }

  void get_out_msg_queue_size(ton::BlockIdExt, td::Promise<td::uint64> promise) override {
    promise.set_value(0);
  }

  void get_shard_blocks_for_collator(
      ton::BlockIdExt, td::Promise<std::vector<td::Ref<ton::validator::ShardTopBlockDescription>>> promise) override {
    promise.set_value({});
  }

  void get_external_messages(ton::ShardIdFull shard,
                             std::unique_ptr<ton::validator::ExtMsgCallback> callback) override {
    auto feed = [](std::vector<td::Ref<ton::validator::ExtMessage>> messages,
                   std::unique_ptr<ton::validator::ExtMsgCallback> callback) -> td::actor::Task<> {
      for (auto& message : messages) {
        co_await callback->queue.push({std::move(message), 0});
      }
      callback->queue.close();
      co_return {};
    };
    feed(shard == base_shard_ ? messages_ : std::vector<td::Ref<ton::validator::ExtMessage>>{}, std::move(callback))
        .start()
        .detach();
  }

  void complete_external_messages(std::vector<ton::validator::ExtMessage::Hash>,
                                  std::vector<ton::validator::ExtMessage::Hash>) override {
  }

  void get_storage_stat_cache(td::Promise<std::function<td::Ref<vm::Cell>(const td::Bits256&)>> promise) override {
    // A cache hit would make iterations history-dependent.  The production
    // collator and validator already treat an unavailable cache as normal.
    promise.set_error(td::Status::Error("storage-stat cache disabled by in-memory benchmark"));
  }

  void update_storage_stat_cache(std::vector<std::pair<td::Ref<vm::Cell>, td::uint32>>) override {
  }

 private:
  void start_up() override {
    // Deliberately do not invoke ValidatorManagerImpl::start_up(): that path
    // creates a database actor and would invalidate the benchmark contract.
  }

  void return_state(ton::BlockIdExt id, td::Promise<td::Ref<ton::validator::ShardState>> promise) {
    if (id == prev_id_) {
      promise.set_value(td::Ref<ton::validator::ShardState>{prev_state_});
      return;
    }
    if (id == mc_id_) {
      promise.set_value(td::Ref<ton::validator::ShardState>{mc_state_});
      return;
    }
    promise.set_error(td::Status::Error("in-memory manager has no state for " + id.to_str()));
  }

  void return_message_queue(ton::BlockIdExt id, td::Promise<td::Ref<ton::validator::MessageQueue>> promise) {
    if (id == prev_id_) {
      promise.set_value(td::Ref<ton::validator::MessageQueue>{prev_message_queue_});
      return;
    }
    if (id == mc_id_) {
      promise.set_value(td::Ref<ton::validator::MessageQueue>{mc_message_queue_});
      return;
    }
    promise.set_error(td::Status::Error("in-memory manager has no message queue for " + id.to_str()));
  }

  void return_block_data(ton::BlockIdExt id, td::Promise<td::Ref<ton::validator::BlockData>> promise) {
    if (id == mc_id_ && mc_block_data_.not_null()) {
      promise.set_value(td::Ref<ton::validator::BlockData>{mc_block_data_});
      return;
    }
    promise.set_error(td::Status::Error("in-memory manager has no block data for " + id.to_str()));
  }

  td::Ref<ton::validator::MasterchainState> mc_state_;
  td::Ref<ton::validator::ShardState> prev_state_;
  ton::BlockIdExt mc_id_;
  ton::BlockIdExt prev_id_;
  ton::ShardIdFull base_shard_;
  td::Ref<ton::validator::BlockData> mc_block_data_;
  td::Ref<ton::validator::MessageQueue> mc_message_queue_;
  td::Ref<ton::validator::MessageQueue> prev_message_queue_;
  std::vector<td::Ref<ton::validator::ExtMessage>> messages_;
  std::deque<ton::validator::CollationStats> collation_stats_;
  std::deque<ton::validator::ValidationStats> validation_stats_;
  td::Promise<ton::validator::CollationStats> collation_stats_waiter_;
  td::Promise<ton::validator::ValidationStats> validation_stats_waiter_;
};

struct CollateOutcome {
  ton::BlockCandidate candidate;
  ton::validator::CollationStats stats;
  double wall_s{0.0};
};

struct ValidateOutcome {
  ton::validator::ValidationStats stats;
  double wall_s{0.0};
};

struct MasterchainBootstrapOutcome {
  ton::BlockCandidate candidate;
  ton::validator::CollationStats collation;
  ton::validator::ValidationStats validation;
  double wall_s{0.0};
};

struct Sample {
  Mode mode{Mode::Collate};
  ValidationVariant validation_variant{ValidationVariant::None};
  bool foreign_candidate{false};
  int iteration{0};
  td::optional<ton::validator::CollationStats> collation;
  td::optional<ton::validator::ValidationStats> validation;
  double collate_wall_s{0.0};
  double validate_wall_s{0.0};
};

struct RunOutput {
  std::vector<Sample> samples;
  double bootstrap_wall_s{0.0};
  ton::BlockIdExt bootstrapped_mc_id;
  td::uint64 bootstrap_block_bytes{0};
  td::uint64 bootstrap_collated_bytes{0};
  bool bootstrap_from_corpus{false};
  std::string exported_corpus_id;
};

struct StoredCorpusInput {
  ton::BlockCandidate full_candidate;
  ton::BlockCandidate preloaded_candidate;
  td::uint64 expected_transactions{0};
  td::uint64 expected_gas_used{0};
  td::optional<ton::BlockCandidate> foreign_candidate;
  ValidationVariant foreign_variant{ValidationVariant::None};
};

ton::validator::CollateParams make_collate_params(const Fixture& fixture, const Config& config) {
  auto options = td::Ref<ton::validator::CollatorOptions>{true};
  options.write().force_full_collated_data = true;
  const double target_utime = fixture.target_gen_utime_ms
                                  ? static_cast<double>(fixture.target_gen_utime_ms.value()) / 1000.0
                                  : static_cast<double>(fixture.gen_utime + 1);
  td::optional<td::Bits256> rand_seed = config.rand_seed;
  if (!rand_seed && fixture.expected_rand_seed) {
    rand_seed = fixture.expected_rand_seed.value();
  }
  return ton::validator::CollateParams{
      .shard = fixture.shard,
      .min_masterchain_block_id = fixture.mc_id,
      .prev = {fixture.prev_id},
      .creator = fixture.creator,
      .validator_set = fixture.validator_set,
      .collator_opts = std::move(options),
      .utime = target_utime,
      .hard_timeout = td::Timestamp::in(config.timeout_s),
      .prev_block_state_roots = {fixture.state_root},
      .rand_seed = rand_seed,
  };
}

ton::validator::ValidateParams make_validate_params(const Fixture& fixture, const Config& config) {
  return ton::validator::ValidateParams{
      .shard = fixture.shard,
      .min_masterchain_block_id = fixture.mc_id,
      .prev = {fixture.prev_id},
      .validator_set = fixture.validator_set,
      .parallel_validation = config.parallel_validation,
      .prev_block_state_roots = {fixture.state_root},
  };
}

td::Result<ton::validator::CollateParams> make_masterchain_bootstrap_collate_params(const Fixture& fixture,
                                                                                    const Config& config) {
  if (!fixture.mc_id.is_masterchain() || fixture.mc_id.seqno() != 0 || fixture.mc_state.is_null() ||
      fixture.mc_validator_set.is_null() || fixture.mc_block_data.not_null()) {
    return td::Status::Error("fixture is not ready for the masterchain seqno-1 bootstrap");
  }
  auto validators = fixture.mc_validator_set->export_vector();
  if (validators.empty()) {
    return td::Status::Error("masterchain bootstrap validator set is empty");
  }
  const auto previous_utime = fixture.mc_state->get_unix_time();
  if (previous_utime == std::numeric_limits<ton::UnixTime>::max()) {
    return td::Status::Error("masterchain bootstrap timestamp overflows UnixTime");
  }
  auto options = td::Ref<ton::validator::CollatorOptions>{true};
  options.write().force_full_collated_data = true;
  return ton::validator::CollateParams{
      .shard = fixture.mc_id.shard_full(),
      .min_masterchain_block_id = fixture.mc_id,
      .prev = {fixture.mc_id},
      .creator = validators.front().key,
      .validator_set = fixture.mc_validator_set,
      .collator_opts = std::move(options),
      .utime = static_cast<double>(previous_utime + 1),
      .hard_timeout = td::Timestamp::in(config.timeout_s),
      .prev_block_state_roots = {fixture.mc_state->root_cell()},
      .rand_seed = config.rand_seed,
  };
}

ton::validator::ValidateParams make_masterchain_bootstrap_validate_params(const Fixture& fixture,
                                                                          const Config& config) {
  return ton::validator::ValidateParams{
      .shard = fixture.mc_id.shard_full(),
      .min_masterchain_block_id = fixture.mc_id,
      .prev = {fixture.mc_id},
      .validator_set = fixture.mc_validator_set,
      .parallel_validation = config.parallel_validation,
      .prev_block_state_roots = {fixture.mc_state->root_cell()},
  };
}

td::Result<ton::BlockCandidate> make_preloaded_candidate(const ton::BlockCandidate& collated_candidate) {
  if (block::compute_file_hash(collated_candidate.data) != collated_candidate.id.file_hash) {
    return td::Status::Error("golden candidate block file hash is inconsistent");
  }
  if (block::compute_file_hash(collated_candidate.collated_data) != collated_candidate.collated_file_hash) {
    return td::Status::Error("golden candidate collated-data file hash is inconsistent");
  }

  TRY_RESULT(roots, vm::std_boc_deserialize_multi(collated_candidate.collated_data));
  std::vector<td::Ref<vm::Cell>> metadata_roots;
  metadata_roots.reserve(roots.size());
  size_t removed_merkle_proofs = 0;
  size_t removed_account_storage_proofs = 0;
  bool have_consensus_extra_data = false;
  for (auto& root : roots) {
    bool is_special = false;
    auto cs = vm::load_cell_slice_special(root, is_special);
    if (!cs.is_valid()) {
      return td::Status::Error("cannot inspect a collated-data root while deriving preloaded validation input");
    }
    if (is_special) {
      if (cs.special_type() != vm::Cell::SpecialType::MerkleProof) {
        return td::Status::Error("full candidate has an unexpected special collated-data root");
      }
      ++removed_merkle_proofs;
      continue;
    }
    if (block::gen::t_AccountStorageDictProof.has_valid_tag(cs)) {
      ++removed_account_storage_proofs;
      continue;
    }
    have_consensus_extra_data |= block::gen::t_ConsensusExtraData.has_valid_tag(cs);
    metadata_roots.push_back(std::move(root));
  }
  if (removed_merkle_proofs + removed_account_storage_proofs == 0) {
    return td::Status::Error("golden candidate has no full collated-data proofs to strip");
  }
  if (!have_consensus_extra_data) {
    return td::Status::Error("golden candidate has no ConsensusExtraData root to preserve");
  }

  TRY_RESULT(minimal_collated_data, vm::std_boc_serialize_multi(std::move(metadata_roots), 2));
  if (minimal_collated_data.empty()) {
    return td::Status::Error("preloaded validation candidate unexpectedly has empty metadata");
  }
  auto preloaded_candidate = collated_candidate.clone();
  preloaded_candidate.collated_data = std::move(minimal_collated_data);
  preloaded_candidate.collated_file_hash = block::compute_file_hash(preloaded_candidate.collated_data);

  if (preloaded_candidate.id != collated_candidate.id ||
      preloaded_candidate.data.as_slice() != collated_candidate.data.as_slice() ||
      preloaded_candidate.collated_file_hash == collated_candidate.collated_file_hash ||
      block::compute_file_hash(preloaded_candidate.collated_data) != preloaded_candidate.collated_file_hash) {
    return td::Status::Error("preloaded validation candidate failed block/hash consistency checks");
  }

  TRY_RESULT(check_roots, vm::std_boc_deserialize_multi(preloaded_candidate.collated_data));
  for (const auto& root : check_roots) {
    bool is_special = false;
    auto cs = vm::load_cell_slice_special(root, is_special);
    if (!cs.is_valid() || is_special || block::gen::t_AccountStorageDictProof.has_valid_tag(cs)) {
      return td::Status::Error("preloaded validation candidate still contains a full collated-data proof");
    }
  }
  return preloaded_candidate;
}

td::Result<bool> has_full_collated_data(const ton::BlockCandidate& candidate) {
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(candidate.collated_data));
  for (const auto& root : roots) {
    bool is_special = false;
    auto cs = vm::load_cell_slice_special(root, is_special);
    if (!cs.is_valid()) {
      return td::Status::Error("cannot inspect candidate collated-data roots");
    }
    if ((is_special && cs.special_type() == vm::Cell::SpecialType::MerkleProof) ||
        (!is_special && block::gen::t_AccountStorageDictProof.has_valid_tag(cs))) {
      return true;
    }
  }
  return false;
}

td::Status check_collation_stats(const ton::validator::CollationStats& stats, const Fixture& fixture,
                                 const Config& config) {
  if (stats.status.is_error()) {
    return stats.status.clone();
  }
  if (stats.ext_msgs_total != fixture.messages.size() || stats.ext_msgs_accepted != fixture.messages.size() ||
      stats.ext_msgs_filtered != 0 || stats.ext_msgs_rejected != 0) {
    return td::Status::Error(PSLICE() << "collator did not accept the complete transfer batch: total="
                                      << stats.ext_msgs_total << " accepted=" << stats.ext_msgs_accepted << " filtered="
                                      << stats.ext_msgs_filtered << " rejected=" << stats.ext_msgs_rejected);
  }
  const auto transaction_multiplier = transactions_per_transfer(config.fixture.workload);
  if (fixture.messages.size() > std::numeric_limits<td::uint32>::max() / transaction_multiplier) {
    return td::Status::Error("workload transaction-count expectation overflows uint32");
  }
  const auto expected_transactions = static_cast<td::uint32>(fixture.messages.size() * transaction_multiplier);
  if (stats.transactions != expected_transactions) {
    return td::Status::Error(PSLICE() << "workload produced " << stats.transactions << " transactions, expected "
                                      << expected_transactions << "; gas=" << stats.gas
                                      << " actual_bytes=" << stats.actual_bytes
                                      << " estimated_bytes=" << stats.estimated_bytes << " lt_delta=" << stats.lt_delta
                                      << " limit_categories=" << stats.cat_bytes << '/' << stats.cat_gas << '/'
                                      << stats.cat_lt_delta << '/' << stats.cat_collated_data_bytes);
  }
  return td::Status::OK();
}

td::Status check_validation_result(ton::validator::ValidateCandidateResult& result,
                                   const ton::validator::ValidationStats& stats) {
  bool accepted = false;
  std::string rejection;
  result.visit(td::overloaded([&](ton::validator::CandidateAccept) { accepted = true; },
                              [&](ton::validator::CandidateReject reject) { rejection = std::move(reject.reason); }));
  if (!accepted || !stats.valid) {
    return td::Status::Error(PSLICE() << "candidate validation failed: " << rejection << " " << stats.comment);
  }
  return td::Status::OK();
}

struct OwnerObservation {
  td::int64 ton_balance{0};
  td::uint32 seqno{0};
};

struct JettonWalletObservation {
  td::int64 ton_balance{0};
  Uint128 jetton_balance{0};
};

td::Result<std::unique_ptr<block::Account>> unpack_active_account(vm::AugmentedDictionary& accounts,
                                                                  const block::StdAddress& address, ton::UnixTime now,
                                                                  const td::Ref<vm::Cell>& expected_code,
                                                                  td::Slice kind) {
  auto account = std::make_unique<block::Account>(address.workchain, address.addr.bits());
  if (!account->unpack(accounts.lookup(address.addr), now, false) || account->status != block::Account::acc_active ||
      account->code.is_null() || account->data.is_null() || account->code->get_hash() != expected_code->get_hash()) {
    return td::Status::Error(PSLICE() << "cannot unpack active " << kind << " account " << address);
  }
  if (account->balance.grams.is_null() || !account->balance.grams->signed_fits_bits(64) ||
      account->balance.extra.not_null()) {
    return td::Status::Error(PSLICE() << kind << " account has unsupported TON balance " << address);
  }
  return account;
}

td::Result<td::Bits256> account_cell_hash(vm::AugmentedDictionary& accounts, const block::StdAddress& address) {
  auto entry = accounts.lookup(address.addr);
  if (entry.is_null() || entry->size_refs() < 1) {
    return td::Status::Error(PSLICE() << "cannot locate ShardAccount cell for " << address);
  }
  auto account_cell = entry->prefetch_ref();
  if (account_cell.is_null()) {
    return td::Status::Error(PSLICE() << "ShardAccount has no Account reference for " << address);
  }
  return td::Bits256{account_cell->get_hash().bits()};
}

td::Result<OwnerObservation> observe_owner(vm::AugmentedDictionary& accounts, const block::StdAddress& address,
                                           ton::UnixTime now, const td::Ref<vm::Cell>& expected_code) {
  TRY_RESULT(account, unpack_active_account(accounts, address, now, expected_code, "WalletV5"));
  auto cs = vm::load_cell_slice(account->data);
  td::uint32 seqno = 0;
  if (cs.fetch_ulong(1) != 1 || !cs.fetch_uint_to(32, seqno)) {
    return td::Status::Error(PSLICE() << "cannot unpack WalletV5 sequence number for " << address);
  }
  return OwnerObservation{account->balance.grams->to_long(), seqno};
}

td::Result<Uint128> fetch_uint128_coins(vm::CellSlice& cs) {
  if (!cs.have(4)) {
    return td::Status::Error("jetton balance has no VarUInteger length");
  }
  auto bytes = static_cast<unsigned>(cs.fetch_ulong(4));
  if (bytes >= 16 || !cs.have(bytes * 8)) {
    return td::Status::Error("jetton balance is not a valid VarUInteger 16");
  }
  Uint128 result = 0;
  for (unsigned i = 0; i < bytes; ++i) {
    result = (result << 8) | static_cast<Uint128>(cs.fetch_ulong(8));
  }
  return result;
}

bool unpack_expected_address(vm::CellSlice& cs, const block::StdAddress& expected) {
  block::StdAddress address;
  return block::tlb::t_MsgAddressInt.extract_std_address(cs, address, false) && address == expected;
}

td::Result<JettonWalletObservation> observe_jetton_wallet(vm::AugmentedDictionary& accounts,
                                                          const block::StdAddress& address,
                                                          const block::StdAddress& expected_owner,
                                                          const block::StdAddress& expected_minter, ton::UnixTime now,
                                                          const td::Ref<vm::Cell>& expected_code) {
  TRY_RESULT(account, unpack_active_account(accounts, address, now, expected_code, "jetton-wallet"));
  auto cs = vm::load_cell_slice(account->data);
  TRY_RESULT(jetton_balance, fetch_uint128_coins(cs));
  if (!unpack_expected_address(cs, expected_owner) || !unpack_expected_address(cs, expected_minter) ||
      cs.size_refs() != 1) {
    return td::Status::Error(PSLICE() << "jetton-wallet data has unexpected owner/master layout for " << address);
  }
  auto embedded_code = cs.fetch_ref();
  if (embedded_code.is_null() || embedded_code->get_hash() != expected_code->get_hash() || !cs.empty_ext()) {
    return td::Status::Error(PSLICE() << "jetton-wallet data has unexpected code/trailing data for " << address);
  }
  return JettonWalletObservation{account->balance.grams->to_long(), jetton_balance};
}

td::Result<td::int64> uint128_to_int64(Uint128 value, td::Slice field) {
  if (value > static_cast<Uint128>(std::numeric_limits<td::int64>::max())) {
    return td::Status::Error(PSLICE() << field << " does not fit signed 64-bit TON accounting");
  }
  return static_cast<td::int64>(value);
}

// Jetton workload: each pair moves jettons between two jetton wallets and pays
// the recipient owner a transfer notification.
td::Status verify_jetton_transfer_pairs(vm::AugmentedDictionary& previous_accounts,
                                        vm::AugmentedDictionary& next_accounts, ton::UnixTime now,
                                        const Fixture& fixture, const Config& config) {
  if (config.fixture.jetton_initial_balance < config.fixture.jetton_transfer_amount ||
      ~static_cast<Uint128>(0) - config.fixture.jetton_initial_balance < config.fixture.jetton_transfer_amount) {
    return td::Status::Error("fixture jetton balances cannot express the expected transfer outcome");
  }
  const Uint128 expected_source_jettons = config.fixture.jetton_initial_balance - config.fixture.jetton_transfer_amount;
  const Uint128 expected_recipient_jettons =
      config.fixture.jetton_initial_balance + config.fixture.jetton_transfer_amount;
  TRY_RESULT(owner_initial_ton, uint128_to_int64(config.fixture.owner_balance, "owner_balance"));
  TRY_RESULT(jetton_wallet_initial_ton,
             uint128_to_int64(config.fixture.jetton_wallet_ton_balance, "jetton_wallet_ton_balance"));
  TRY_RESULT(message_value, uint128_to_int64(config.fixture.message_value, "message_value"));
  TRY_RESULT(forward_ton_amount, uint128_to_int64(config.fixture.forward_ton_amount, "forward_ton_amount"));
  if (owner_initial_ton > std::numeric_limits<td::int64>::max() - forward_ton_amount) {
    return td::Status::Error("recipient-owner TON upper bound overflows signed 64-bit accounting");
  }
  const td::int64 maximum_recipient_owner_ton = owner_initial_ton + forward_ton_amount;
  if (jetton_wallet_initial_ton > std::numeric_limits<td::int64>::max() - message_value) {
    return td::Status::Error("jetton-wallet TON upper bound overflows signed 64-bit accounting");
  }
  const td::int64 maximum_jetton_wallet_ton = jetton_wallet_initial_ton + message_value;

  for (size_t i = 0; i < fixture.messages.size(); ++i) {
    TRY_RESULT(source_owner, observe_owner(next_accounts, fixture.source_owner_addresses[i], now, fixture.w5_code));
    TRY_RESULT(recipient_owner,
               observe_owner(next_accounts, fixture.recipient_owner_addresses[i], now, fixture.w5_code));
    TRY_RESULT(source_jetton,
               observe_jetton_wallet(next_accounts, fixture.source_jetton_addresses[i],
                                     fixture.source_owner_addresses[i], fixture.minter_address, now, fixture.jw_code));
    TRY_RESULT(recipient_jetton, observe_jetton_wallet(next_accounts, fixture.recipient_jetton_addresses[i],
                                                       fixture.recipient_owner_addresses[i], fixture.minter_address,
                                                       now, fixture.jw_code));

    if (source_owner.seqno != 1 || source_owner.ton_balance <= 0 || source_owner.ton_balance >= owner_initial_ton) {
      return td::Status::Error(PSLICE() << "source WalletV5 outcome is invalid at pair " << i << ": seqno="
                                        << source_owner.seqno << " ton_balance=" << source_owner.ton_balance);
    }
    TRY_RESULT(old_recipient_owner_hash, account_cell_hash(previous_accounts, fixture.recipient_owner_addresses[i]));
    TRY_RESULT(new_recipient_owner_hash, account_cell_hash(next_accounts, fixture.recipient_owner_addresses[i]));
    if (recipient_owner.seqno != 0 || old_recipient_owner_hash == new_recipient_owner_hash ||
        recipient_owner.ton_balance <= owner_initial_ton || recipient_owner.ton_balance > maximum_recipient_owner_ton) {
      return td::Status::Error(PSLICE() << "recipient WalletV5 notification outcome is invalid at pair " << i
                                        << ": seqno=" << recipient_owner.seqno
                                        << " ton_balance=" << recipient_owner.ton_balance);
    }
    if (source_jetton.jetton_balance != expected_source_jettons || source_jetton.ton_balance <= 0 ||
        source_jetton.ton_balance > maximum_jetton_wallet_ton) {
      return td::Status::Error(PSLICE() << "source jetton-wallet outcome is invalid at pair " << i
                                        << ": jettons=" << u128_to_dec(source_jetton.jetton_balance)
                                        << " ton_balance=" << source_jetton.ton_balance);
    }
    if (recipient_jetton.jetton_balance != expected_recipient_jettons || recipient_jetton.ton_balance <= 0 ||
        recipient_jetton.ton_balance > maximum_jetton_wallet_ton) {
      return td::Status::Error(PSLICE() << "recipient jetton-wallet outcome is invalid at pair " << i
                                        << ": jettons=" << u128_to_dec(recipient_jetton.jetton_balance)
                                        << " ton_balance=" << recipient_jetton.ton_balance);
    }
  }
  return td::Status::OK();
}

// Plain transfer workload: each pair moves TON between two Wallet V5 accounts
// and must leave both jetton wallets of the pair untouched.
td::Status verify_plain_transfer_pairs(vm::AugmentedDictionary& previous_accounts,
                                       vm::AugmentedDictionary& next_accounts, ton::UnixTime now,
                                       const Fixture& fixture, const Config& config) {
  TRY_RESULT(owner_initial_ton, uint128_to_int64(config.fixture.owner_balance, "owner_balance"));
  TRY_RESULT(message_value, uint128_to_int64(config.fixture.message_value, "message_value"));
  if (owner_initial_ton > std::numeric_limits<td::int64>::max() - message_value) {
    return td::Status::Error("recipient-owner TON upper bound overflows signed 64-bit accounting");
  }
  if (owner_initial_ton <= message_value) {
    return td::Status::Error("fixture owner balance cannot fund the expected plain transfer");
  }
  const td::int64 maximum_recipient_owner_ton = owner_initial_ton + message_value;
  const td::int64 maximum_source_owner_ton = owner_initial_ton - message_value;

  for (size_t i = 0; i < fixture.messages.size(); ++i) {
    TRY_RESULT(source_owner, observe_owner(next_accounts, fixture.source_owner_addresses[i], now, fixture.w5_code));
    TRY_RESULT(recipient_owner,
               observe_owner(next_accounts, fixture.recipient_owner_addresses[i], now, fixture.w5_code));

    if (source_owner.seqno != 1 || source_owner.ton_balance <= 0 ||
        source_owner.ton_balance >= maximum_source_owner_ton) {
      return td::Status::Error(PSLICE() << "source WalletV5 outcome is invalid at pair " << i << ": seqno="
                                        << source_owner.seqno << " ton_balance=" << source_owner.ton_balance);
    }
    TRY_RESULT(old_recipient_owner_hash, account_cell_hash(previous_accounts, fixture.recipient_owner_addresses[i]));
    TRY_RESULT(new_recipient_owner_hash, account_cell_hash(next_accounts, fixture.recipient_owner_addresses[i]));
    if (recipient_owner.seqno != 0 || old_recipient_owner_hash == new_recipient_owner_hash ||
        recipient_owner.ton_balance <= owner_initial_ton || recipient_owner.ton_balance > maximum_recipient_owner_ton) {
      return td::Status::Error(PSLICE() << "recipient WalletV5 transfer outcome is invalid at pair " << i << ": seqno="
                                        << recipient_owner.seqno << " ton_balance=" << recipient_owner.ton_balance);
    }
    for (const auto& jetton_address : {fixture.source_jetton_addresses[i], fixture.recipient_jetton_addresses[i]}) {
      TRY_RESULT(old_jetton_hash, account_cell_hash(previous_accounts, jetton_address));
      TRY_RESULT(new_jetton_hash, account_cell_hash(next_accounts, jetton_address));
      if (old_jetton_hash != new_jetton_hash) {
        return td::Status::Error(PSLICE() << "jetton-wallet Account cell changed during a plain transfer at pair " << i
                                          << ": " << jetton_address);
      }
    }
  }
  return td::Status::OK();
}

td::Status verify_transfer_outcome(const ton::BlockCandidate& candidate, const Fixture& fixture, const Config& config) {
  TRY_RESULT(block_data, ton::validator::create_block(candidate.id, candidate.data.clone()));
  block::gen::Block::Record block_record;
  if (!block::gen::unpack_cell(block_data->root_cell(), block_record)) {
    return td::Status::Error("cannot unpack golden candidate block");
  }
  TRY_STATUS(vm::MerkleUpdate::validate(block_record.state_update));
  TRY_STATUS(vm::MerkleUpdate::may_apply(fixture.state_root, block_record.state_update));
  TRY_RESULT(next_state_root, vm::MerkleUpdate::apply(fixture.state_root, block_record.state_update));
  if (fixture.expected_successor_state_root &&
      td::Bits256{next_state_root->get_hash().bits()} != fixture.expected_successor_state_root.value()) {
    return td::Status::Error(PSLICE() << "candidate successor state root " << next_state_root->get_hash().to_hex()
                                      << " differs from corpus expectation "
                                      << fixture.expected_successor_state_root->to_hex());
  }
  // A restored corpus deliberately carries protocol artifacts rather than the
  // generator's private keys and synthetic address derivation.  The exact
  // successor root above is its generic semantic oracle.
  if (fixture.source_owner_addresses.empty() && fixture.source_jetton_addresses.empty() &&
      fixture.recipient_owner_addresses.empty() && fixture.recipient_jetton_addresses.empty()) {
    return td::Status::OK();
  }
  block::ShardState previous_state;
  TRY_STATUS(previous_state.unpack_state(fixture.prev_id, fixture.state_root));
  block::ShardState next_state;
  TRY_STATUS(next_state.unpack_state(candidate.id, next_state_root));
  if (!previous_state.account_dict_ || !next_state.account_dict_) {
    return td::Status::Error("golden candidate previous/next state has no account dictionary");
  }
  const auto transfer_count = fixture.messages.size();
  if (fixture.source_owner_addresses.size() != transfer_count ||
      fixture.source_jetton_addresses.size() != transfer_count ||
      fixture.recipient_owner_addresses.size() != transfer_count ||
      fixture.recipient_jetton_addresses.size() != transfer_count) {
    return td::Status::Error("fixture transfer-address vectors are inconsistent");
  }
  if (fixture.w5_code.is_null() || fixture.jw_code.is_null()) {
    return td::Status::Error("fixture WalletV5/jetton-wallet code is unavailable during preflight");
  }
  if (config.fixture.workload == Workload::Transfer) {
    TRY_STATUS(verify_plain_transfer_pairs(*previous_state.account_dict_, *next_state.account_dict_, next_state.utime_,
                                           fixture, config));
  } else {
    TRY_STATUS(verify_jetton_transfer_pairs(*previous_state.account_dict_, *next_state.account_dict_, next_state.utime_,
                                            fixture, config));
  }

  TRY_RESULT(old_minter_hash, account_cell_hash(*previous_state.account_dict_, fixture.minter_address));
  TRY_RESULT(new_minter_hash, account_cell_hash(*next_state.account_dict_, fixture.minter_address));
  if (old_minter_hash != new_minter_hash) {
    return td::Status::Error("jetton minter Account cell changed during transfers");
  }
  return td::Status::OK();
}

td::actor::Task<CollateOutcome> collate_once(td::actor::ActorId<InMemoryManager> manager, const Fixture& fixture,
                                             const Config& config) {
  auto stats_task = td::actor::ask(manager, &InMemoryManager::wait_collation_stats);
  auto [candidate_task, candidate_promise] = td::actor::StartedTask<ton::BlockCandidate>::make_bridge();
  auto params = make_collate_params(fixture, config);
  td::Timer wall;
  ton::validator::run_collate_query(std::move(params), manager, {}, std::move(candidate_promise));
  auto candidate = co_await std::move(candidate_task);
  const double wall_s = wall.elapsed();
  auto stats = co_await std::move(stats_task);
  auto status = check_collation_stats(stats, fixture, config);
  if (status.is_error()) {
    co_return status;
  }
  co_return CollateOutcome{std::move(candidate), std::move(stats), wall_s};
}

td::actor::Task<ValidateOutcome> validate_once(td::actor::ActorId<InMemoryManager> manager,
                                               ton::BlockCandidate candidate, const Fixture& fixture,
                                               const Config& config) {
  const auto expected_block_id = candidate.id;
  const auto expected_block_bytes = candidate.data.size();
  const auto expected_collated_hash = candidate.collated_file_hash;
  const auto expected_collated_bytes = candidate.collated_data.size();
  auto stats_task = td::actor::ask(manager, &InMemoryManager::wait_validation_stats);
  auto [result_task, result_promise] = td::actor::StartedTask<ton::validator::ValidateCandidateResult>::make_bridge();
  auto params = make_validate_params(fixture, config);
  td::Timer wall;
  ton::validator::run_validate_query(std::move(candidate), std::move(params), manager,
                                     td::Timestamp::in(config.timeout_s), std::move(result_promise));
  auto result = co_await std::move(result_task);
  const double wall_s = wall.elapsed();
  auto stats = co_await std::move(stats_task);
  auto status = check_validation_result(result, stats);
  if (status.is_error()) {
    co_return status;
  }
  if (stats.block_id != expected_block_id || stats.actual_bytes != expected_block_bytes ||
      stats.collated_data_hash != expected_collated_hash ||
      stats.actual_collated_data_bytes != expected_collated_bytes) {
    co_return td::Status::Error("validation statistics do not match the selected raw candidate input");
  }
  co_return ValidateOutcome{std::move(stats), wall_s};
}

td::actor::Task<MasterchainBootstrapOutcome> bootstrap_masterchain_once(td::actor::ActorId<InMemoryManager> manager,
                                                                        const Fixture& fixture, const Config& config) {
  auto params_result = make_masterchain_bootstrap_collate_params(fixture, config);
  if (params_result.is_error()) {
    co_return params_result.move_as_error();
  }
  auto collation_stats_task = td::actor::ask(manager, &InMemoryManager::wait_collation_stats);
  auto [candidate_task, candidate_promise] = td::actor::StartedTask<ton::BlockCandidate>::make_bridge();
  td::Timer wall;
  ton::validator::run_collate_query(params_result.move_as_ok(), manager, {}, std::move(candidate_promise));
  auto candidate = co_await std::move(candidate_task);
  auto collation_stats = co_await std::move(collation_stats_task);
  if (collation_stats.status.is_error()) {
    co_return collation_stats.status.clone();
  }
  if (collation_stats.ext_msgs_total != 0 || collation_stats.ext_msgs_accepted != 0 ||
      collation_stats.ext_msgs_filtered != 0 || collation_stats.ext_msgs_rejected != 0) {
    co_return td::Status::Error("masterchain bootstrap unexpectedly consumed external messages");
  }
  if (!candidate.id.is_masterchain() || candidate.id.seqno() != fixture.mc_id.seqno() + 1 ||
      candidate.id.shard_full() != fixture.mc_id.shard_full() ||
      block::compute_file_hash(candidate.data) != candidate.id.file_hash ||
      block::compute_file_hash(candidate.collated_data) != candidate.collated_file_hash) {
    co_return td::Status::Error("masterchain bootstrap produced an inconsistent block candidate");
  }

  const auto expected_collated_hash = candidate.collated_file_hash;
  const auto expected_collated_bytes = candidate.collated_data.size();
  auto validation_stats_task = td::actor::ask(manager, &InMemoryManager::wait_validation_stats);
  auto [result_task, result_promise] = td::actor::StartedTask<ton::validator::ValidateCandidateResult>::make_bridge();
  ton::validator::run_validate_query(candidate.clone(), make_masterchain_bootstrap_validate_params(fixture, config),
                                     manager, td::Timestamp::in(config.timeout_s), std::move(result_promise));
  auto result = co_await std::move(result_task);
  auto validation_stats = co_await std::move(validation_stats_task);
  auto status = check_validation_result(result, validation_stats);
  if (status.is_error()) {
    co_return status;
  }
  if (validation_stats.collated_data_hash != expected_collated_hash ||
      validation_stats.actual_collated_data_bytes != expected_collated_bytes) {
    co_return td::Status::Error("masterchain bootstrap validation statistics do not match its candidate");
  }
  co_return MasterchainBootstrapOutcome{std::move(candidate), std::move(collation_stats), std::move(validation_stats),
                                        wall.elapsed()};
}

td::actor::Task<RunOutput> run_benchmark(Fixture fixture, Config config,
                                         td::optional<StoredCorpusInput> stored_corpus = {}) {
  RunOutput output;
  if (stored_corpus) {
    if (fixture.mc_state.is_null() || fixture.mc_block_data.is_null() || fixture.validator_set.is_null() ||
        fixture.prev_state.is_null() || fixture.state_root.is_null() || fixture.corpus_id.empty()) {
      co_return td::Status::Error("disk corpus did not restore a complete in-memory fixture");
    }
    output.bootstrap_from_corpus = true;
    output.bootstrapped_mc_id = fixture.mc_id;
    output.bootstrap_block_bytes = fixture.mc_block_data->data().size();
  } else {
    auto bootstrap_manager = td::actor::create_actor<InMemoryManager>("collation-bench-bootstrap-manager", fixture);
    auto bootstrap = co_await bootstrap_masterchain_once(bootstrap_manager.get(), fixture, config);
    bootstrap_manager.reset();
    auto bootstrap_status = apply_masterchain_bootstrap(fixture, bootstrap.candidate);
    if (bootstrap_status.is_error()) {
      co_return bootstrap_status;
    }
    if (fixture.mc_id != bootstrap.candidate.id || fixture.mc_state.is_null() || fixture.mc_block_data.is_null() ||
        fixture.validator_set.is_null()) {
      co_return td::Status::Error("masterchain bootstrap did not install a complete in-memory tip");
    }
    output.bootstrap_wall_s = bootstrap.wall_s;
    output.bootstrapped_mc_id = fixture.mc_id;
    output.bootstrap_block_bytes = bootstrap.candidate.data.size();
    output.bootstrap_collated_bytes = bootstrap.candidate.collated_data.size();
  }

  auto manager = td::actor::create_actor<InMemoryManager>("collation-bench-manager", fixture);

  // Correctness gate and validation-only candidates.  Proof stripping and all
  // validation here are excluded from every measured series.
  ton::BlockCandidate golden_candidate;
  td::optional<ton::validator::CollationStats> generated_golden_stats;
  if (stored_corpus) {
    auto& stored = stored_corpus.value();
    if (config.mode != Mode::Validate) {
      // Modes that exercise collation prove that this implementation can
      // reproduce the portable semantics before timing it. Candidate byte
      // identity is intentionally diagnostic only; the successor root,
      // transaction count and gas are the portable oracle. Validation-only
      // corpus runs avoid this cross-phase instruction/data-cache pollution.
      auto replay = co_await collate_once(manager.get(), fixture, config);
      if (replay.stats.transactions != stored.expected_transactions || replay.stats.gas != stored.expected_gas_used) {
        co_return td::Status::Error(PSLICE()
                                    << "corpus collation semantic mismatch: transactions=" << replay.stats.transactions
                                    << " gas=" << replay.stats.gas << " expected_transactions="
                                    << stored.expected_transactions << " expected_gas=" << stored.expected_gas_used);
      }
      auto replay_status = verify_transfer_outcome(replay.candidate, fixture, config);
      if (replay_status.is_error()) {
        co_return replay_status;
      }
    }
    golden_candidate = std::move(stored.full_candidate);
  } else {
    auto golden_collation = co_await collate_once(manager.get(), fixture, config);
    golden_candidate = std::move(golden_collation.candidate);
    generated_golden_stats = std::move(golden_collation.stats);
  }
  auto full_data_result = has_full_collated_data(golden_candidate);
  if (full_data_result.is_error()) {
    co_return full_data_result.move_as_error();
  }
  if (!full_data_result.move_as_ok()) {
    co_return td::Status::Error("golden collated candidate contains no full state/storage proof");
  }
  td::optional<ton::BlockCandidate> preloaded_candidate;
  auto preloaded_result = make_preloaded_candidate(golden_candidate);
  if (preloaded_result.is_error()) {
    co_return preloaded_result.move_as_error();
  }
  auto regenerated_preloaded = preloaded_result.move_as_ok();
  if (stored_corpus) {
    auto& stored = stored_corpus.value();
    if (stored.preloaded_candidate.id != regenerated_preloaded.id ||
        stored.preloaded_candidate.collated_file_hash != regenerated_preloaded.collated_file_hash ||
        stored.preloaded_candidate.data.as_slice() != regenerated_preloaded.data.as_slice() ||
        stored.preloaded_candidate.collated_data.as_slice() != regenerated_preloaded.collated_data.as_slice()) {
      co_return td::Status::Error("stored preloaded candidate differs from the canonical proof-stripped sidecar");
    }
    preloaded_candidate = std::move(stored.preloaded_candidate);
  } else {
    preloaded_candidate = std::move(regenerated_preloaded);
  }

  td::optional<ton::BlockCandidate> foreign_candidate;
  ValidationVariant foreign_variant = ValidationVariant::None;
  if (stored_corpus && stored_corpus.value().foreign_candidate) {
    foreign_candidate = std::move(stored_corpus.value().foreign_candidate.value());
    foreign_variant = stored_corpus.value().foreign_variant;
    auto foreign_status = verify_transfer_outcome(foreign_candidate.value(), fixture, config);
    if (foreign_status.is_error()) {
      co_return foreign_status;
    }
  }

  auto transfer_status = verify_transfer_outcome(golden_candidate, fixture, config);
  if (transfer_status.is_error()) {
    co_return transfer_status;
  }
  if (!config.corpus_out.empty()) {
    if (!generated_golden_stats) {
      co_return td::Status::Error("cannot export a corpus without freshly generated collation statistics");
    }
    // An exported corpus is consumed by other implementations, so both
    // published candidates must first be accepted by production validation.
    (void)co_await validate_once(manager.get(), golden_candidate.clone(), fixture, config);
    (void)co_await validate_once(manager.get(), preloaded_candidate.value().clone(), fixture, config);
    auto corpus_result = write_collation_corpus(
        config.corpus_out, config.fixture, fixture, golden_candidate, preloaded_candidate.value(),
        generated_golden_stats.value().transactions, generated_golden_stats.value().gas);
    if (corpus_result.is_error()) {
      co_return corpus_result.move_as_error();
    }
    output.exported_corpus_id = corpus_result.move_as_ok();
  }

  auto warm_collation = [&]() -> td::actor::Task<> {
    (void)co_await collate_once(manager.get(), fixture, config);
    co_return {};
  };
  auto warm_validation = [&]() -> td::actor::Task<> {
    const auto& prototype =
        foreign_candidate
            ? foreign_candidate.value()
            : (config.validation_input == ValidationVariant::Collated ? golden_candidate : preloaded_candidate.value());
    (void)co_await validate_once(manager.get(), prototype.clone(), fixture, config);
    co_return {};
  };
  for (int i = 0; i < config.warmup; ++i) {
    switch (config.mode) {
      case Mode::Collate:
        co_await warm_collation();
        break;
      case Mode::Validate:
        co_await warm_validation();
        break;
    }
  }

  auto run_collate_series = config.mode == Mode::Collate;
  auto run_validate_series = config.mode == Mode::Validate;
  td::optional<ton::BlockCandidate> measured_collation_check;

  if (run_collate_series) {
    for (int i = 0; i < config.iterations; ++i) {
      auto result = co_await collate_once(manager.get(), fixture, config);
      if (!measured_collation_check) {
        measured_collation_check = std::move(result.candidate);
      }
      Sample sample{.mode = Mode::Collate, .iteration = i, .collate_wall_s = result.wall_s};
      sample.collation = std::move(result.stats);
      output.samples.push_back(std::move(sample));
    }
    (void)co_await validate_once(manager.get(), measured_collation_check.value().clone(), fixture, config);
    transfer_status = verify_transfer_outcome(measured_collation_check.value(), fixture, config);
    if (transfer_status.is_error()) {
      co_return transfer_status;
    }
  }
  if (run_validate_series) {
    auto run_validation_sample = [&](ValidationVariant variant, int iteration) -> td::actor::Task<> {
      const auto& prototype =
          foreign_candidate ? foreign_candidate.value()
                            : (variant == ValidationVariant::Collated ? golden_candidate : preloaded_candidate.value());
      auto result = co_await validate_once(manager.get(), prototype.clone(), fixture, config);
      Sample sample{.mode = Mode::Validate,
                    .validation_variant = variant,
                    .foreign_candidate = static_cast<bool>(foreign_candidate),
                    .iteration = iteration,
                    .validate_wall_s = result.wall_s};
      sample.validation = std::move(result.stats);
      output.samples.push_back(std::move(sample));
      co_return {};
    };
    for (int i = 0; i < config.iterations; ++i) {
      co_await run_validation_sample(foreign_candidate ? foreign_variant : config.validation_input, i);
    }
  }

  manager.reset();
  co_return output;
}

template <class T>
class TaskRunner final : public td::actor::Actor {
 public:
  TaskRunner(td::actor::Task<T> task, std::shared_ptr<td::optional<td::Result<T>>> result)
      : task_(std::move(task)), result_(std::move(result)) {
  }

 private:
  void start_up() override {
    run().start().detach();
  }

  void tear_down() override {
    td::actor::SchedulerContext::get().stop();
  }

  td::actor::Task<> run() {
    result_->emplace(co_await std::move(task_).wrap());
    stop();
    co_return {};
  }

  td::actor::Task<T> task_;
  std::shared_ptr<td::optional<td::Result<T>>> result_;
};

template <class T>
td::Result<T> run_task(td::actor::Task<T> task, int scheduler_threads) {
  auto result = std::make_shared<td::optional<td::Result<T>>>();
  {
    td::actor::Scheduler scheduler({static_cast<size_t>(scheduler_threads)});
    scheduler.run_in_context([&] {
      td::actor::create_actor<TaskRunner<T>>("collation-validation-bench", std::move(task), result).release();
    });
    scheduler.run();
  }
  CHECK(static_cast<bool>(*result));
  return std::move(result->value());
}

double percentile(std::vector<double> values, double q) {
  CHECK(!values.empty());
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>(std::ceil(q * static_cast<double>(values.size() - 1)));
  return values[index];
}

double median(const std::vector<double>& values) {
  return percentile(values, 0.5);
}

double mean(const std::vector<double>& values) {
  CHECK(!values.empty());
  double sum = 0.0;
  for (double value : values) {
    sum += value;
  }
  return sum / static_cast<double>(values.size());
}

double mad(const std::vector<double>& values) {
  const double center = median(values);
  std::vector<double> deviations;
  deviations.reserve(values.size());
  for (double value : values) {
    deviations.push_back(std::abs(value - center));
  }
  return median(deviations);
}

using NamedTimes = std::vector<std::pair<const char*, td::RealCpuTimer::Time>>;

NamedTimes collation_stages(const ton::validator::CollationStats& stats) {
  const auto& w = stats.work_time;
  return {{"total", w.total},
          {"preinit", w.preinit},
          {"queue_cleanup", w.queue_cleanup},
          {"dispatch_queue", w.dispatch_queue},
          {"import_internals", w.import_internals},
          {"import_externals", w.import_externals},
          {"process_new_msgs", w.process_new_msgs},
          {"prelim_storage_stat", w.prelim_storage_stat},
          {"trx_tvm", w.trx_tvm},
          {"trx_storage_stat", w.trx_storage_stat},
          {"trx_other", w.trx_other},
          {"final_storage_stat", w.final_storage_stat},
          {"enqueue_new_messages", w.enqueue_new_messages},
          {"combine_account_transactions", w.combine_account_transactions},
          {"create_shard_state", w.create_shard_state},
          {"create_block", w.create_block},
          {"create_collated_data", w.create_collated_data},
          {"create_block_candidate", w.create_block_candidate}};
}

NamedTimes validation_stages(const ton::validator::ValidationStats& stats) {
  const auto& w = stats.work_time;
  return {{"total", w.total},
          {"unpack_block_candidate", w.unpack_block_candidate},
          {"process_mc_state", w.process_mc_state},
          {"trx_tvm", w.trx_tvm},
          {"trx_storage_stat", w.trx_storage_stat},
          {"trx_other", w.trx_other},
          {"check_transactions_other", w.check_transactions_other},
          {"unpack_state", w.unpack_state},
          {"validate_block_tlb", w.validate_block_tlb},
          {"unpack_block_data", w.unpack_block_data},
          {"precheck_account_updates", w.precheck_account_updates},
          {"precheck_account_transactions", w.precheck_account_transactions},
          {"precheck_msg_queue", w.precheck_msg_queue},
          {"unpack_dispatch_queue", w.unpack_dispatch_queue},
          {"check_in_msg_descr", w.check_in_msg_descr},
          {"check_out_msg_descr", w.check_out_msg_descr},
          {"check_dispatch_queue", w.check_dispatch_queue},
          {"check_processed_upto", w.check_processed_upto},
          {"check_in_queue", w.check_in_queue},
          {"check_new_state", w.check_new_state}};
}

void print_sample(const Sample& sample) {
  std::cout << "COLLATION-VALIDATION-BENCH phase=sample mode=" << mode_name(sample.mode)
            << " iteration=" << sample.iteration;
  if (sample.collation) {
    const auto& stats = sample.collation.value();
    std::cout << " collate_wall_ms=" << sample.collate_wall_s * 1000.0
              << " collate_active_ms=" << stats.work_time.total.real * 1000.0
              << " collate_cpu_ms=" << stats.work_time.total.cpu * 1000.0 << " transactions=" << stats.transactions
              << " ext_accepted=" << stats.ext_msgs_accepted << " gas=" << stats.gas
              << " block_bytes=" << stats.actual_bytes << " collated_bytes=" << stats.actual_collated_data_bytes;
  }
  if (sample.validation) {
    const auto& stats = sample.validation.value();
    std::cout << " validation_input=" << validation_variant_name(sample.validation_variant)
              << " candidate_source=" << (sample.foreign_candidate ? "foreign" : "golden")
              << " validate_wall_ms=" << sample.validate_wall_s * 1000.0
              << " validate_active_ms=" << stats.work_time.total.real * 1000.0
              << " validate_cpu_ms=" << stats.work_time.total.cpu * 1000.0
              << " validation_collated_bytes=" << stats.actual_collated_data_bytes
              << " validation_collated_hash=" << stats.collated_data_hash.to_hex();
  }
  std::cout << '\n';
}

void print_wall_summary(Mode mode, ValidationVariant variant, bool foreign_candidate, td::Slice metric,
                        const std::vector<double>& seconds) {
  if (seconds.empty()) {
    return;
  }
  std::cout << "COLLATION-VALIDATION-BENCH phase=summary mode=" << mode_name(mode);
  if (variant != ValidationVariant::None) {
    std::cout << " validation_input=" << validation_variant_name(variant)
              << " candidate_source=" << (foreign_candidate ? "foreign" : "golden");
  }
  std::cout << " metric=" << metric.str() << " mean_ms=" << mean(seconds) * 1000.0
            << " median_ms=" << median(seconds) * 1000.0 << " mad_ms=" << mad(seconds) * 1000.0
            << " p90_ms=" << percentile(seconds, 0.90) * 1000.0
            << " min_ms=" << *std::min_element(seconds.begin(), seconds.end()) * 1000.0 << '\n';
}

template <class GetStages>
void print_stage_summary(Mode mode, ValidationVariant variant, td::Slice query,
                         const std::vector<const Sample*>& samples, GetStages get_stages) {
  if (samples.empty()) {
    return;
  }
  auto first = get_stages(*samples.front());
  const bool foreign_candidate =
      std::all_of(samples.begin(), samples.end(), [](const auto* sample) { return sample->foreign_candidate; });
  for (size_t stage_index = 0; stage_index < first.size(); ++stage_index) {
    std::vector<double> real;
    std::vector<double> cpu;
    real.reserve(samples.size());
    cpu.reserve(samples.size());
    for (const auto* sample : samples) {
      auto stages = get_stages(*sample);
      CHECK(stages.size() == first.size());
      CHECK(td::Slice(stages[stage_index].first) == td::Slice(first[stage_index].first));
      real.push_back(stages[stage_index].second.real);
      if (std::isfinite(stages[stage_index].second.cpu)) {
        cpu.push_back(stages[stage_index].second.cpu);
      }
    }
    std::cout << "COLLATION-VALIDATION-BENCH phase=stage-summary mode=" << mode_name(mode);
    if (variant != ValidationVariant::None) {
      std::cout << " validation_input=" << validation_variant_name(variant)
                << " candidate_source=" << (foreign_candidate ? "foreign" : "golden");
    }
    std::cout << " query=" << query.str() << " stage=" << first[stage_index].first
              << " real_median_ms=" << median(real) * 1000.0 << " real_p90_ms=" << percentile(real, 0.90) * 1000.0;
    if (cpu.size() == samples.size()) {
      std::cout << " cpu_median_ms=" << median(cpu) * 1000.0 << " cpu_available=1";
    } else {
      std::cout << " cpu_median_ms=na cpu_available=0";
    }
    std::cout << '\n';
  }
}

void print_results(const RunOutput& output, bool verbose) {
  if (output.bootstrap_from_corpus) {
    std::cout << "COLLATION-VALIDATION-BENCH phase=bootstrap status=ok measured=0 kind=corpus-restored"
              << " wall_ms=0.000000 mc_id=" << output.bootstrapped_mc_id.to_str()
              << " block_bytes=" << output.bootstrap_block_bytes << " collated_bytes=0\n";
  } else {
    std::cout << "COLLATION-VALIDATION-BENCH phase=bootstrap status=ok measured=0 kind=masterchain-seqno-1"
              << " wall_ms=" << output.bootstrap_wall_s * 1000.0 << " mc_id=" << output.bootstrapped_mc_id.to_str()
              << " block_bytes=" << output.bootstrap_block_bytes
              << " collated_bytes=" << output.bootstrap_collated_bytes
              << " bootstrap_collated_data=consensus-metadata-only full_collated_data=0\n";
  }
  if (!output.exported_corpus_id.empty()) {
    std::cout << "COLLATION-VALIDATION-BENCH phase=corpus status=ok direction=write measured=0 corpus_id="
              << output.exported_corpus_id << '\n';
  }
  if (verbose) {
    for (const auto& sample : output.samples) {
      print_sample(sample);
    }
  }
  const std::vector<std::pair<Mode, ValidationVariant>> groups{
      {Mode::Collate, ValidationVariant::None},
      {Mode::Validate, ValidationVariant::Collated},
      {Mode::Validate, ValidationVariant::Preloaded},
  };
  for (const auto& [mode, variant] : groups) {
    std::vector<const Sample*> selected;
    std::vector<double> collate_walls, validate_walls;
    for (const auto& sample : output.samples) {
      if (sample.mode != mode || sample.validation_variant != variant) {
        continue;
      }
      selected.push_back(&sample);
      if (sample.collation) {
        collate_walls.push_back(sample.collate_wall_s);
      }
      if (sample.validation) {
        validate_walls.push_back(sample.validate_wall_s);
      }
    }
    if (selected.empty()) {
      continue;
    }
    const bool foreign_candidate =
        std::all_of(selected.begin(), selected.end(), [](const auto* sample) { return sample->foreign_candidate; });
    print_wall_summary(mode, variant, foreign_candidate, "collate_wall", collate_walls);
    print_wall_summary(mode, variant, foreign_candidate, "validate_wall", validate_walls);
    if (verbose && !collate_walls.empty()) {
      print_stage_summary(mode, variant, "collate", selected,
                          [](const Sample& sample) { return collation_stages(sample.collation.value()); });
    }
    if (verbose && !validate_walls.empty()) {
      print_stage_summary(mode, variant, "validate", selected,
                          [](const Sample& sample) { return validation_stages(sample.validation.value()); });
    }
  }
}

std::string current_rss() {
  auto stat = td::mem_stat();
  return stat.is_ok() ? td::to_string(stat.ok().resident_size_) : "na";
}

std::string error_token(td::Slice message) {
  auto result = message.str();
  for (char& c : result) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      c = '_';
    }
  }
  return result;
}

bool path_is_same_or_descendant(const std::string& path, const std::string& directory) {
  if (path == directory) {
    return true;
  }
  auto prefix = directory;
  if (!prefix.empty() && prefix.back() != TD_DIR_SLASH) {
    prefix += TD_DIR_SLASH;
  }
  return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace
}  // namespace bench::collation

int main(int argc, char** argv) {
#ifndef NDEBUG
  std::cerr << "COLLATION-VALIDATION-BENCH phase=config status=failed message=debug_build_is_not_supported\n";
  return 2;
#endif

  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(ERROR));
  td::set_default_failure_signal_handler().ensure();
  vm::init_vm().ensure();
  std::cout << std::fixed << std::setprecision(6) << std::unitbuf;

  auto config_result = bench::collation::parse_config(argc, argv);
  if (config_result.is_error()) {
    std::cerr << "COLLATION-VALIDATION-BENCH phase=config status=failed message="
              << bench::collation::error_token(config_result.error().message()) << '\n';
    return 2;
  }
  auto config = config_result.move_as_ok();

  td::Timer fixture_timer;
  bench::collation::Fixture fixture;
  td::optional<bench::collation::StoredCorpusInput> stored_corpus;
  if (!config.corpus_in.empty()) {
    auto corpus_result = bench::collation::load_collation_corpus(config.corpus_in);
    if (corpus_result.is_error()) {
      std::cerr << "COLLATION-VALIDATION-BENCH phase=corpus status=failed direction=read message="
                << bench::collation::error_token(corpus_result.error().message()) << '\n';
      return 1;
    }
    auto corpus = corpus_result.move_as_ok();
    if (!corpus.fixture.expected_rand_seed) {
      std::cerr << "COLLATION-VALIDATION-BENCH phase=corpus status=failed direction=read"
                   " message=corpus_has_no_fixed_random_seed\n";
      return 1;
    }
    if (config.rand_seed && config.rand_seed.value() != corpus.fixture.expected_rand_seed.value()) {
      std::cerr << "COLLATION-VALIDATION-BENCH phase=corpus status=failed direction=read"
                   " message=--rand-seed_conflicts_with_corpus\n";
      return 1;
    }
    // The corpus manifest is the only authority on what a measured run executes.
    if (config.workload_selected && config.fixture.workload != corpus.config.workload) {
      std::cerr << "COLLATION-VALIDATION-BENCH phase=corpus status=failed direction=read"
                   " message=--workload_conflicts_with_corpus\n";
      return 1;
    }
    config.fixture = corpus.config;
    config.rand_seed = corpus.fixture.expected_rand_seed.value();
    td::optional<ton::BlockCandidate> foreign_candidate;
    auto foreign_variant = bench::collation::ValidationVariant::None;
    if (!config.candidate_in.empty()) {
      auto corpus_path_result = td::realpath(config.corpus_in);
      auto candidate_path_result = td::realpath(config.candidate_in);
      if (corpus_path_result.is_error() || candidate_path_result.is_error()) {
        const auto& error = corpus_path_result.is_error() ? corpus_path_result.error() : candidate_path_result.error();
        std::cerr << "COLLATION-VALIDATION-BENCH phase=candidate status=failed direction=read message="
                  << bench::collation::error_token(error.message()) << '\n';
        return 1;
      }
      auto corpus_path = corpus_path_result.move_as_ok();
      auto candidate_path = candidate_path_result.move_as_ok();
      if (bench::collation::path_is_same_or_descendant(candidate_path, corpus_path)) {
        std::cerr << "COLLATION-VALIDATION-BENCH phase=candidate status=failed direction=read"
                     " message=--candidate-in_must_be_outside_the_corpus_directory\n";
        return 1;
      }
      const bool full = config.validation_input == bench::collation::ValidationVariant::Collated;
      auto foreign_result = bench::collation::load_foreign_collation_candidate(
          candidate_path, corpus.fixture,
          full ? bench::collation::CandidateSidecarKind::Full : bench::collation::CandidateSidecarKind::Preloaded,
          corpus.expected_transactions, corpus.expected_gas_used);
      if (foreign_result.is_error()) {
        std::cerr << "COLLATION-VALIDATION-BENCH phase=candidate status=failed direction=read message="
                  << bench::collation::error_token(foreign_result.error().message()) << '\n';
        return 1;
      }
      foreign_candidate = foreign_result.move_as_ok();
      foreign_variant =
          full ? bench::collation::ValidationVariant::Collated : bench::collation::ValidationVariant::Preloaded;
      std::cout << "COLLATION-VALIDATION-BENCH phase=candidate status=ok direction=read source=foreign measured=0"
                << " validation_input=" << bench::collation::validation_variant_name(foreign_variant)
                << " full_collated_data=" << full << " block_id=" << foreign_candidate.value().id.to_str()
                << " block_bytes=" << foreign_candidate.value().data.size()
                << " collated_bytes=" << foreign_candidate.value().collated_data.size()
                << " block_file_hash=" << foreign_candidate.value().id.file_hash.to_hex()
                << " collated_file_hash=" << foreign_candidate.value().collated_file_hash.to_hex() << '\n';
    }
    fixture = std::move(corpus.fixture);
    stored_corpus = bench::collation::StoredCorpusInput{
        .full_candidate = std::move(corpus.full_candidate),
        .preloaded_candidate = std::move(corpus.preloaded_candidate),
        .expected_transactions = corpus.expected_transactions,
        .expected_gas_used = corpus.expected_gas_used,
        .foreign_candidate = std::move(foreign_candidate),
        .foreign_variant = foreign_variant,
    };
    std::cout << "COLLATION-VALIDATION-BENCH phase=corpus status=ok direction=read measured=0 corpus_id="
              << fixture.corpus_id << " load_s=" << fixture_timer.elapsed() << '\n';
  } else {
    auto fixture_result = bench::collation::build_fixture(config.fixture);
    if (fixture_result.is_error()) {
      std::cerr << "COLLATION-VALIDATION-BENCH phase=fixture status=failed message="
                << bench::collation::error_token(fixture_result.error().message()) << '\n';
      return 1;
    }
    fixture = fixture_result.move_as_ok();
  }

  std::cout << "COLLATION-VALIDATION-BENCH phase=config status=ok workload="
            << bench::collation::collation_corpus_workload_name(config.fixture.workload).str()
            << " requested_mode=" << bench::collation::mode_name(config.mode) << " accounts=" << config.fixture.accounts
            << " transfers=" << config.fixture.transfers << " block_transactions="
            << fixture.messages.size() * bench::collation::transactions_per_transfer(config.fixture.workload)
            << " warmup=" << config.warmup << " iterations=" << config.iterations
            << " scheduler_threads=" << config.scheduler_threads
            << " validation_input=" << bench::collation::validation_variant_name(config.validation_input)
            << " parallel_validation=" << config.parallel_validation
            << " fixture_source=" << (config.corpus_in.empty() ? "generated" : "corpus") << " candidate_source="
            << (!config.candidate_in.empty() ? "foreign"
                                             : (config.corpus_in.empty() ? "generated-golden" : "corpus-golden"))
            << " fixed_rand_seed=" << (config.rand_seed ? config.rand_seed.value().to_hex() : "none")
            << " rss_bytes=" << bench::collation::current_rss() << '\n';
  std::cout << "COLLATION-VALIDATION-BENCH phase=fixture status=ok build_s=" << fixture_timer.elapsed()
            << " injected_accounts=" << fixture.injected_accounts
            << " preserved_accounts=" << fixture.preserved_accounts << " messages=" << fixture.messages.size()
            << " state_root=" << fixture.state_root->get_hash().to_hex() << " prev_id=" << fixture.prev_id.to_str()
            << " mc_id=" << fixture.mc_id.to_str() << " rss_bytes=" << bench::collation::current_rss() << '\n';

  auto output = bench::collation::run_task(
      bench::collation::run_benchmark(std::move(fixture), config, std::move(stored_corpus)), config.scheduler_threads);
  if (output.is_error()) {
    std::cerr << "COLLATION-VALIDATION-BENCH phase=run status=failed message="
              << bench::collation::error_token(output.error().message()) << '\n';
    return 1;
  }
  bench::collation::print_results(output.ok(), config.verbose);
  std::cout << "COLLATION-VALIDATION-BENCH phase=done status=ok rss_bytes=" << bench::collation::current_rss() << '\n';
  return 0;
}
