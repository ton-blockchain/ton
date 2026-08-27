/*
    Deterministic, entirely in-memory state used by the collation/validation
    benchmark.  Fixture construction is deliberately outside timed regions.
*/
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "benchmark/common.h"
#include "block/block.h"
#include "td/utils/Status.h"
#include "validator/interfaces/block.h"
#include "validator/interfaces/external-message.h"
#include "validator/interfaces/shard.h"

namespace bench::collation {

// Externals injected into the measured block.  Both workloads use the very same
// account state; only the signed Wallet V5 payload differs.
enum class Workload {
  Jetton,   // Wallet V5 -> jetton wallet -> jetton wallet -> Wallet V5 notification
  Transfer  // Wallet V5 -> Wallet V5 plain TON transfer
};

// Transactions one transfer produces: four for the jetton chain (source wallet,
// source jetton wallet, recipient jetton wallet, recipient notification), two
// for a plain transfer (source wallet, recipient credit).
inline constexpr td::uint64 transactions_per_transfer(Workload workload) {
  return workload == Workload::Transfer ? 2 : 4;
}

struct FixtureConfig {
  td::uint64 accounts{1'000'000};
  td::uint64 transfers{128};
  td::uint64 seed{1};
  Workload workload{Workload::Jetton};
  // The masterchain state supplies configuration and the validator set.  The
  // basechain state is populated with the benchmark accounts.
  std::string zerostate_path;
  std::string base_state_path;
  std::string contracts_dir;

  Uint128 owner_balance{100'000'000'000ULL};            // 100 TON
  Uint128 jetton_wallet_ton_balance{1'000'000'000ULL};  // 1 TON
  Uint128 minter_ton_balance{1'000'000'000ULL};         // 1 TON
  Uint128 jetton_initial_balance{1'000'000'000'000'000ULL};
  Uint128 jetton_transfer_amount{1'000'000ULL};
  Uint128 message_value{50'000'000ULL};      // 0.05 TON
  Uint128 forward_ton_amount{1'000'000ULL};  // 0.001 TON notification to the recipient owner
};

struct Fixture {
  ton::ShardIdFull shard;
  ton::BlockIdExt prev_id;
  ton::BlockIdExt mc_id;
  td::Ref<vm::Cell> state_root;
  td::Ref<ton::validator::ShardState> prev_state;
  td::Ref<ton::validator::MasterchainState> mc_state;
  td::Ref<block::ValidatorSet> validator_set;
  // The modified masterchain zero state is first advanced by one real,
  // untimed collation.  mc_validator_set drives that bootstrap; mc_block_data
  // is populated by apply_masterchain_bootstrap and serves the resulting
  // block to basechain collation/validation without filesystem access.
  td::Ref<block::ValidatorSet> mc_validator_set;
  td::Ref<ton::validator::BlockData> mc_block_data;
  ton::Ed25519_PublicKey creator;
  std::vector<td::Ref<ton::validator::ExtMessage>> messages;
  td::uint32 gen_utime{0};

  // Present when the fixture was restored from a portable collation corpus.
  // These are generic, implementation-independent golden checks; unlike the
  // synthetic-only fields below they do not require regenerating accounts or
  // associating cells by hash.
  std::string corpus_id;
  std::optional<td::Bits256> expected_successor_state_root;
  std::optional<td::Bits256> expected_rand_seed;
  std::optional<td::uint64> target_gen_utime_ms;

  std::vector<block::StdAddress> source_owner_addresses;
  std::vector<block::StdAddress> source_jetton_addresses;
  std::vector<block::StdAddress> recipient_owner_addresses;
  std::vector<block::StdAddress> recipient_jetton_addresses;
  block::StdAddress minter_address;
  td::Ref<vm::Cell> w5_code;
  td::Ref<vm::Cell> jw_code;
  td::Ref<vm::Cell> minter_code;
  td::uint64 injected_accounts{0};
  td::uint64 preserved_accounts{0};
};

td::Result<Fixture> build_fixture(const FixtureConfig& config);

// Completes the untimed masterchain bootstrap.  build_fixture initially
// exposes the modified masterchain seqno-0 state in mc_id/mc_state; callers
// collate its real seqno-1 block, then pass that candidate here.  The helper
// applies its Merkle update, installs the resulting state/block, and refreshes
// the wc0 validator selection used by the measured workload.
td::Status apply_masterchain_bootstrap(Fixture& fixture, const ton::BlockCandidate& candidate);

}  // namespace bench::collation
