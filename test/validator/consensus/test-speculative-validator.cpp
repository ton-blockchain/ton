/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/bus.h"
#include "td/actor/TestScheduler.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"

namespace ton::validator::consensus::test {
namespace {

BlockIdExt block_id(WorkchainId workchain, ShardId shard, BlockSeqno seqno) {
  return {workchain, shard, seqno, td::Bits256::zero(), td::Bits256::zero()};
}

CandidateRef candidate(CandidateId id, ParentId parent, BlockIdExt block, bool empty = false) {
  if (empty) {
    return td::make_ref<Candidate>(id, parent, PeerValidatorId{0}, block, td::BufferSlice{});
  }
  BlockCandidate payload;
  payload.id = block;
  payload.pubkey = Ed25519_PublicKey{td::Bits256::zero()};
  payload.collated_file_hash = td::Bits256::zero();
  payload.data = td::BufferSlice("block bytes");
  payload.collated_data = td::BufferSlice("collated bytes");
  return td::make_ref<Candidate>(id, parent, PeerValidatorId{0}, std::move(payload), td::BufferSlice{});
}

struct ValidationCalls {
  size_t count = 0;
  std::optional<BlockCandidate> block;
  std::optional<ValidateParams> params;
  double ok_from = 0;
  bool fail = false;
};

class RecordingManager final : public ManagerFacade {
 public:
  explicit RecordingManager(ValidationCalls& calls) : calls_(calls) {
  }

  td::actor::Task<ValidateCandidateResult> validate_block_candidate(BlockCandidate block, ValidateParams params,
                                                                    td::Timestamp) override {
    ++calls_.count;
    calls_.block = std::move(block);
    calls_.params = std::move(params);
    if (calls_.fail) {
      co_return td::Status::Error(ErrorCode::notready, "Missing proof dependency");
    }
    co_return CandidateAccept{.ok_from_utime = calls_.ok_from, .can_validate_child = true};
  }

  td::actor::Task<BlockCandidate> collate_block(CollateParams, td::CancellationToken) override {
    UNREACHABLE();
    co_return td::Status::Error("Unexpected collation");
  }

  td::actor::Task<> accept_block(BlockIdExt, td::Ref<BlockData>, size_t, td::Ref<block::BlockSignatureSet>, int, int,
                                 bool, bool) override {
    UNREACHABLE();
    co_return {};
  }

  td::actor::Task<td::Ref<vm::Cell>> wait_block_state_root(BlockIdExt, td::Timestamp) override {
    UNREACHABLE();
    co_return td::Status::Error("Unexpected state lookup");
  }

  td::actor::Task<td::Ref<BlockData>> wait_block_data(BlockIdExt, td::Timestamp) override {
    UNREACHABLE();
    co_return td::Status::Error("Unexpected block lookup");
  }

  td::actor::Task<double> get_sync_delay() override {
    UNREACHABLE();
    co_return 0.0;
  }

 private:
  ValidationCalls& calls_;
};

enum class RequestCase { Eligible, Ordinary, Empty, Masterchain, ParentHash, SlotGap, ChildShard, ParentShard, SeqnoGap };

void check_dispatch(RequestCase request_case, bool manager_fails = false) {
  td::actor::TestScheduler scheduler;
  td::actor::Runtime runtime;
  BlockValidator::register_in(runtime);
  ValidationCalls calls;
  calls.fail = manager_fails;

  scheduler.run([&]() -> td::actor::Task<> {
    auto manager = td::actor::create_actor<RecordingManager>("recording-manager", calls);
    auto bus = std::make_shared<Bus>();
    bus->shard = {basechainId, shardIdAll};
    PeerValidator local{};
    local.idx = PeerValidatorId{0};
    local.short_id = PublicKeyHash{td::Bits256::zero()};
    bus->local_id = local;
    bus->manager = manager.get();
    const CandidateId parent{6, td::Bits256::zero()};
    CandidateId child{7, td::Bits256::zero()};
    ParentId claimed_parent = parent;
    auto parent_block = block_id(basechainId, shardIdAll, 100);
    auto child_block = block_id(basechainId, shardIdAll, 101);
    const auto min_mc = block_id(masterchainId, shardIdAll, 200);

    switch (request_case) {
      case RequestCase::Ordinary:
        parent_block = block_id(basechainId, shardIdAll, 0);
        child_block = block_id(basechainId, shardIdAll, 1);
        break;
      case RequestCase::Masterchain:
        bus->shard = {masterchainId, shardIdAll};
        parent_block = block_id(masterchainId, shardIdAll, 100);
        child_block = block_id(masterchainId, shardIdAll, 101);
        break;
      case RequestCase::ParentHash:
        claimed_parent->hash.as_slice()[0] = 1;
        break;
      case RequestCase::SlotGap:
        ++child.slot;
        break;
      case RequestCase::ChildShard:
        child_block = block_id(basechainId, shardIdAll >> 1, 101);
        break;
      case RequestCase::ParentShard:
        parent_block = block_id(basechainId, shardIdAll >> 1, 100);
        break;
      case RequestCase::SeqnoGap:
        child_block = block_id(basechainId, shardIdAll, 102);
        break;
      default:
        break;
    }
    auto block = candidate(child, claimed_parent, child_block, request_case == RequestCase::Empty);
    auto handle = runtime.start(std::move(bus), "speculative-validator");
    co_await scheduler.wait_sync_work();
    calls.ok_from = td::Timestamp::in(60).at_unix();

    td::actor::StartedTask<ValidateCandidateResult> validation;
    if (request_case == RequestCase::Ordinary) {
      auto state = td::make_ref<ChainState>(
          ChainState::ZerostateTip{parent_block, vm::CellBuilder{}.finalize_novm()}, min_mc);
      validation = handle.publish<ValidationRequest>(std::move(state), block).start();
    } else {
      validation = handle.publish<SpeculativeValidationRequest>(block, parent, parent_block, min_mc).start();
    }
    co_await scheduler.wait_sync_work();
    // The caller must receive the result while its timestamp is still in the future.
    EXPECT(validation.await_ready());
    auto result = co_await std::move(validation).wrap();
    if (request_case != RequestCase::Eligible && request_case != RequestCase::Ordinary) {
      EXPECT(result.is_error());
      EXPECT_EQ(result.error().code(), ErrorCode::notready);
      EXPECT_EQ(calls.count, 0u);
    } else {
      EXPECT_EQ(calls.count, 1u);
      EXPECT_EQ(calls.block->id, child_block);
      EXPECT_EQ(calls.block->data.as_slice(), "block bytes");
      EXPECT_EQ(calls.block->collated_data.as_slice(), "collated bytes");
      EXPECT_EQ(calls.params->shard, (ShardIdFull{basechainId, shardIdAll}));
      EXPECT_EQ(calls.params->prev.size(), 1u);
      EXPECT_EQ(calls.params->prev[0], parent_block);
      EXPECT_EQ(calls.params->min_masterchain_block_id, min_mc);
      EXPECT_EQ(calls.params->local_validator_id, local.short_id);
      EXPECT_EQ(calls.params->require_full_collated_data, request_case != RequestCase::Ordinary);
      EXPECT_EQ(calls.params->prev_block_state_roots.size(), request_case == RequestCase::Ordinary ? 1u : 0u);
      EXPECT(!calls.params->is_fake);
      if (manager_fails) {
        EXPECT(result.is_error());
        EXPECT_EQ(result.error().code(), ErrorCode::notready);
      } else {
        EXPECT(result.is_ok());
        EXPECT(result.ok().has<CandidateAccept>());
        EXPECT_EQ(result.ok().get<CandidateAccept>().ok_from_utime, calls.ok_from);
        EXPECT(result.ok().get<CandidateAccept>().can_validate_child);
        EXPECT(!td::Timestamp::at_unix(calls.ok_from).is_in_past());
      }
    }

    handle.publish<StopRequested>();
    handle = {};
    manager.reset();
    co_await scheduler.wait_sync_work();
    co_return {};
  });
}

TEST(SpeculativeValidator, IneligibleRequestsNeverReachManager) {
  for (auto request_case : {RequestCase::Empty, RequestCase::Masterchain, RequestCase::ParentHash, RequestCase::SlotGap,
                            RequestCase::ChildShard, RequestCase::ParentShard, RequestCase::SeqnoGap}) {
    check_dispatch(request_case);
  }
}

TEST(SpeculativeValidator, ExactContextAndRawFutureTimestamp) {
  check_dispatch(RequestCase::Eligible);
}

TEST(SpeculativeValidator, OrdinaryValidationReturnsRawFutureTimestamp) {
  check_dispatch(RequestCase::Ordinary);
}

TEST(SpeculativeValidator, MissingDependencyRemainsRetryable) {
  check_dispatch(RequestCase::Eligible, true);
}

}  // namespace
}  // namespace ton::validator::consensus::test
