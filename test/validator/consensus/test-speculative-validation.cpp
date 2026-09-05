/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <algorithm>
#include <map>
#include <set>

#include "consensus/simplex/bus.h"
#include "td/actor/TestScheduler.h"
#include "td/utils/tests.h"
#include "vm/cells/CellBuilder.h"

namespace ton::validator::consensus::simplex::test {
namespace {

const ShardIdFull kShard{0, shardIdAll};

CandidateId candidate_id(td::uint32 slot, bool conflicting = false) {
  return {slot, conflicting ? td::Bits256::ones() : td::Bits256::zero()};
}

BlockIdExt block_id(td::uint32 seqno, bool conflicting = false) {
  return {BlockId{kShard, seqno}, conflicting ? td::Bits256::ones() : td::Bits256::zero(), td::Bits256::zero()};
}

class FixtureBlock final : public BlockData {
 public:
  explicit FixtureBlock(BlockIdExt id) : id_(id) {
  }
  td::BufferSlice data() const override {
    return {};
  }
  FileHash file_hash() const override {
    return id_.file_hash;
  }
  BlockIdExt block_id() const override {
    return id_;
  }
  td::Ref<vm::Cell> root_cell() const override {
    return vm::CellBuilder{}.finalize_novm();
  }

 private:
  BlockIdExt id_;
};

class RemoteCollator final : public CollatorSchedule {
 public:
  PeerValidatorId expected_collator_for(td::uint32) const override {
    return PeerValidatorId{1};
  }
};

struct Harness {
  td::actor::Runtime runtime;
  BusHandle bus;
  std::map<td::uint32, CandidateRef> candidates;
  std::set<td::uint32> allowed_parents{0};
  std::set<td::uint32> held_stores;
  std::map<td::uint32, std::vector<td::Promise<WaitForParent::ReturnType>>> parent_waiters;
  std::map<td::uint32, std::vector<td::Promise<td::Unit>>> store_waiters;
  std::map<td::uint32, td::Promise<ValidateCandidateResult>> speculative_waiters;
  std::map<td::uint32, size_t> normal_calls;
  std::map<td::uint32, size_t> speculative_calls;
  std::map<td::uint32, size_t> store_calls;
  std::vector<CandidateId> notar_votes;
  std::vector<CandidateId> final_votes;
  std::set<td::uint32> skip_votes;
  std::vector<std::shared_ptr<SpeculativeValidationRequest>> speculative_requests;
  BlockIdExt min_mc{BlockId{masterchainId, shardIdAll, 10}, td::Bits256::zero(), td::Bits256::zero()};
  double parent_time = 0;
  bool parent_can_validate_child = true;
  bool conflicting_state = false;
  bool reject_parent = false;
  bool parent_conflict = false;

  CandidateRef candidate(td::uint32 slot, bool conflicting = false, bool empty = false) {
    ParentId parent;
    if (slot != 0) {
      parent = candidate_id(slot - 1);
    }
    std::variant<BlockIdExt, BlockCandidate> block;
    if (empty) {
      block = block_id(100 + slot);
    } else {
      BlockCandidate full;
      full.id = block_id(101 + slot, conflicting);
      block = std::move(full);
    }
    auto result = td::make_ref<Candidate>(candidate_id(slot, conflicting), parent, PeerValidatorId{1}, std::move(block),
                                          td::BufferSlice{});
    if (!conflicting) {
      candidates[slot] = result;
    }
    return result;
  }

  ChainStateRef state(ParentId parent) {
    auto id = parent ? candidates.at(parent->slot)->block_id() : block_id(100);
    if (conflicting_state && parent) {
      id.root_hash = td::Bits256::ones();
    }
    return td::make_ref<ChainState>(
        ChainState::NormalTip{td::make_ref<FixtureBlock>(id), vm::CellBuilder{}.finalize_novm()}, min_mc);
  }

  size_t votes(td::uint32 slot) const {
    return std::count_if(notar_votes.begin(), notar_votes.end(), [slot](const auto& id) { return id.slot == slot; });
  }

  void allow_parent(td::uint32 child) {
    allowed_parents.insert(child);
    for (auto& waiter : parent_waiters[child]) {
      waiter.set_value(std::nullopt);
    }
    parent_waiters.erase(child);
  }

  void store(td::uint32 slot) {
    held_stores.erase(slot);
    for (auto& waiter : store_waiters[slot]) {
      waiter.set_value(td::Unit{});
    }
    store_waiters.erase(slot);
  }

  void accept_speculation(td::uint32 slot, double ok_from = 0) {
    speculative_waiters.at(slot).set_value(CandidateAccept{.ok_from_utime = ok_from, .can_validate_child = true});
    speculative_waiters.erase(slot);
  }

  void certify(td::uint32 slot) {
    auto id = candidate_id(slot);
    bus.publish<NotarizationObserved>(
        id, td::make_ref<NotarCert>(NotarizeVote{id}, std::vector<NotarCert::VoteSignature>{}));
    allow_parent(slot + 1);
  }

  void stop() {
    bus.publish<StopRequested>();
  }
};

Harness* active = nullptr;

// Only the external dependencies are mocked: each scenario runs the production Consensus actor.
class ControlledDependencies final : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  td::actor::Task<WaitForParent::ReturnType> process(BusHandle, std::shared_ptr<WaitForParent> request) {
    auto slot = request->candidate->id.slot;
    if (active->allowed_parents.contains(slot)) {
      if (slot != 0 && active->parent_conflict) {
        co_return td::make_ref<Misbehavior>();
      }
      co_return std::nullopt;
    }
    auto [task, promise] = td::actor::StartedTask<WaitForParent::ReturnType>::make_bridge();
    active->parent_waiters[slot].push_back(std::move(promise));
    co_return co_await std::move(task);
  }

  template <>
  td::actor::Task<ResolveState::ReturnType> process(BusHandle, std::shared_ptr<ResolveState> request) {
    co_return ResolveState::Result{active->state(request->id),
                                   request->id ? std::optional{active->parent_time} : std::nullopt};
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<StoreCandidate> request) {
    auto slot = request->candidate->id.slot;
    ++active->store_calls[slot];
    if (active->held_stores.contains(slot)) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      active->store_waiters[slot].push_back(std::move(promise));
      co_await std::move(task);
    }
    co_return {};
  }

  template <>
  td::actor::Task<ValidateCandidateResult> process(BusHandle, std::shared_ptr<ValidationRequest> request) {
    ++active->normal_calls[request->candidate->id.slot];
    if (active->reject_parent && request->candidate->id.slot == 0) {
      co_return CandidateReject{.reason = "invalid parent", .proof = {}};
    }
    co_return CandidateAccept{.ok_from_utime = active->parent_time,
                              .can_validate_child = active->parent_can_validate_child};
  }

  template <>
  td::actor::Task<ValidateCandidateResult> process(BusHandle, std::shared_ptr<SpeculativeValidationRequest> request) {
    auto slot = request->candidate->id.slot;
    ++active->speculative_calls[slot];
    active->speculative_requests.push_back(request);
    auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
    EXPECT(!active->speculative_waiters.contains(slot));
    active->speculative_waiters.emplace(slot, std::move(promise));
    co_return co_await std::move(task);
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<BroadcastVote> request) {
    if (auto vote = std::get_if<NotarizeVote>(&request->vote.vote)) {
      active->notar_votes.push_back(vote->id);
    } else if (auto vote = std::get_if<FinalizeVote>(&request->vote.vote)) {
      active->final_votes.push_back(vote->id);
    } else {
      active->skip_votes.insert(std::get<SkipVote>(request->vote.vote).slot);
    }
    co_return {};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }
};

template <typename Scenario>
void run_scenario(Scenario scenario, std::chrono::milliseconds min_interval = std::chrono::milliseconds{0}) {
  td::actor::TestScheduler scheduler;
  Harness h;
  active = &h;
  Consensus::register_in(h.runtime);
  h.runtime.register_actor<ControlledDependencies>("ControlledDependencies");
  scheduler.run([&]() -> td::actor::Task<> {
    auto bus = std::make_shared<Bus>();
    PeerValidator local{};
    local.idx = PeerValidatorId{0};
    auto remote = local;
    remote.idx = PeerValidatorId{1};
    bus->local_id = local;
    bus->validator_set = {local, remote};
    bus->local_adnl_id = remote.adnl_id;
    bus->shard = kShard;
    bus->config.slots_per_leader_window = 16;
    bus->config.noncritical_params.min_block_interval = min_interval;
    bus->config.noncritical_params.first_block_timeout = std::chrono::seconds{30};
    bus->collator_schedule = td::make_ref<RemoteCollator>();
    h.parent_time = td::Clocks::system();
    h.bus = h.runtime.start(std::move(bus), "speculative-validation");
    co_await scheduler.wait_sync_work();
    co_await h.bus.publish<LeaderWindowObserved>(0, ParentId{});
    co_await scenario(h, scheduler);
    h.stop();
    co_await scheduler.wait_sync_work();
    h.parent_waiters.clear();
    h.store_waiters.clear();
    h.speculative_waiters.clear();
    h.bus = {};
    co_await scheduler.wait_sync_work();
    co_return {};
  });
  active = nullptr;
}

td::actor::Task<> start_parent_and_child(Harness& h, td::actor::TestScheduler& scheduler) {
  // Child-first arrival also exercises wakeup when the parent's local validation later completes.
  h.bus.publish<CandidateReceived>(h.candidate(1));
  co_await scheduler.wait_sync_work();
  EXPECT_EQ(h.speculative_calls[1], 0u);
  h.bus.publish<CandidateReceived>(h.candidate(0));
  co_await scheduler.wait_sync_work();
  EXPECT_EQ(h.votes(0), 1u);
  EXPECT_EQ(h.speculative_calls[1], 1u);
  EXPECT_EQ(h.votes(1), 0u);
  const auto& request = *h.speculative_requests.back();
  EXPECT(request.parent_id == candidate_id(0));
  EXPECT(request.parent_block_id == h.candidates.at(0)->block_id());
  EXPECT(request.min_masterchain_block_id == h.min_mc);
  co_return {};
}

TEST(SpeculativeValidation, ResultBeforeCertificateStillWaitsForCertificateAndStorage) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    h.held_stores.insert(1);
    co_await start_parent_and_child(h, scheduler);
    h.accept_speculation(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    h.certify(0);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    h.store(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 1u);
    EXPECT_EQ(h.normal_calls[1], 0u);
    co_return {};
  });
}

TEST(SpeculativeValidation, CertificateBeforeResultReusesInFlightValidation) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    co_await start_parent_and_child(h, scheduler);
    h.certify(0);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    EXPECT_EQ(h.normal_calls[1], 0u);
    h.accept_speculation(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 1u);
    EXPECT_EQ(h.normal_calls[1], 0u);
    co_return {};
  });
}

TEST(SpeculativeValidation, VotesRespectParentIntervalAndCandidateTimestamp) {
  for (bool child_timestamp_later : {false, true}) {
    run_scenario(
        [child_timestamp_later](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
          co_await start_parent_and_child(h, scheduler);
          h.accept_speculation(1, h.parent_time + (child_timestamp_later ? 0.5 : 0.1));
          h.certify(0);
          co_await scheduler.wait_sync_work();
          scheduler.advance_time(std::chrono::milliseconds{200});
          co_await scheduler.wait_sync_work();
          EXPECT_EQ(h.votes(1), 0u);
          scheduler.advance_time(std::chrono::milliseconds{110});
          co_await scheduler.wait_sync_work();
          EXPECT_EQ(h.votes(1), child_timestamp_later ? 0u : 1u);
          scheduler.advance_time(std::chrono::milliseconds{200});
          co_await scheduler.wait_sync_work();
          EXPECT_EQ(h.votes(1), 1u);
          co_return {};
        },
        std::chrono::milliseconds{300});
  }
}

TEST(SpeculativeValidation, OneChildAheadAndDuplicateCandidatesDoNotMultiplyWork) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    co_await start_parent_and_child(h, scheduler);
    h.bus.publish<CandidateReceived>(h.candidate(1, true));
    h.bus.publish<CandidateReceived>(h.candidates.at(1));
    h.bus.publish<CandidateReceived>(h.candidate(2));
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.speculative_calls[1], 1u);
    EXPECT_EQ(h.store_calls[1], 1u);
    EXPECT_EQ(h.speculative_calls[2], 0u);
    h.accept_speculation(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.speculative_calls[2], 0u);
    h.certify(0);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 1u);
    EXPECT_EQ(h.speculative_calls[2], 1u);
    h.accept_speculation(2);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(2), 0u);
    co_return {};
  });
}

TEST(SpeculativeValidation, ErrorsAndRejectionsRetryOnlyAfterParentCertificate) {
  for (bool reject : {false, true}) {
    run_scenario([reject](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
      co_await start_parent_and_child(h, scheduler);
      auto& promise = h.speculative_waiters.at(1);
      if (reject) {
        promise.set_value(CandidateReject{.reason = "early rejection", .proof = {}});
      } else {
        promise.set_error(td::Status::Error("parent data not available yet"));
      }
      h.speculative_waiters.erase(1);
      co_await scheduler.wait_sync_work();
      EXPECT_EQ(h.normal_calls[1], 0u);
      EXPECT_EQ(h.votes(1), 0u);
      h.certify(0);
      co_await scheduler.wait_sync_work();
      EXPECT_EQ(h.normal_calls[1], 1u);
      EXPECT_EQ(h.votes(1), 1u);
      co_return {};
    });
  }
}

TEST(SpeculativeValidation, ChangedParentOrMasterchainContextRequiresOrdinaryValidation) {
  for (bool change_parent : {false, true}) {
    run_scenario([change_parent](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
      co_await start_parent_and_child(h, scheduler);
      h.accept_speculation(1);
      if (change_parent) {
        h.conflicting_state = true;
      } else {
        h.min_mc.id.seqno += 1;
      }
      h.certify(0);
      co_await scheduler.wait_sync_work();
      EXPECT_EQ(h.normal_calls[1], 1u);
      EXPECT_EQ(h.votes(1), 1u);
      co_return {};
    });
  }
}

TEST(SpeculativeValidation, ParentConflictDuringStoragePreventsVote) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    h.held_stores.insert(1);
    co_await start_parent_and_child(h, scheduler);
    h.accept_speculation(1);
    h.certify(0);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    h.parent_conflict = true;
    h.store(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    co_return {};
  });
}

TEST(SpeculativeValidation, ConflictingCandidateCertificateDuringStoragePreventsVote) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    h.held_stores.insert(1);
    co_await start_parent_and_child(h, scheduler);
    h.accept_speculation(1);
    h.certify(0);
    co_await scheduler.wait_sync_work();
    auto id = candidate_id(1, true);
    h.bus.publish<NotarizationObserved>(
        id, td::make_ref<NotarCert>(NotarizeVote{id}, std::vector<NotarCert::VoteSignature>{}));
    co_await scheduler.wait_sync_work();
    h.store(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    co_return {};
  });
}

TEST(SpeculativeValidation, ObsoleteRunningValidationKeepsSingleSpeculationReservation) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    co_await start_parent_and_child(h, scheduler);
    auto id = candidate_id(1);
    h.bus.publish<FinalizationObserved>(
        id, td::make_ref<FinalCert>(FinalizeVote{id}, std::vector<FinalCert::VoteSignature>{}));
    h.allow_parent(2);
    h.bus.publish<CandidateReceived>(h.candidate(2));
    h.bus.publish<CandidateReceived>(h.candidate(3));
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(2), 1u);
    EXPECT_EQ(h.speculative_calls[3], 0u);
    h.accept_speculation(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    h.certify(2);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(3), 1u);
    h.bus.publish<CandidateReceived>(h.candidate(4));
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.speculative_calls[4], 1u);
    h.accept_speculation(4);
    co_return {};
  });
}

TEST(SpeculativeValidation, SkipAllowsLateNotarizationButPreventsFinalVote) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    co_await start_parent_and_child(h, scheduler);
    scheduler.advance_time(std::chrono::seconds{35});
    co_await scheduler.wait_sync_work();
    EXPECT(h.skip_votes.contains(1));
    h.accept_speculation(1);
    h.certify(0);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 1u);
    h.certify(1);
    co_await scheduler.wait_sync_work();
    EXPECT(std::none_of(h.final_votes.begin(), h.final_votes.end(), [](auto id) { return id.slot == 1; }));
    co_return {};
  });
}

TEST(SpeculativeValidation, FinalizationAndSessionStopPreventLateVotes) {
  for (bool stop : {false, true}) {
    run_scenario([stop](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
      co_await start_parent_and_child(h, scheduler);
      if (stop) {
        h.stop();
      } else {
        auto id = candidate_id(1, true);
        h.bus.publish<FinalizationObserved>(
            id, td::make_ref<FinalCert>(FinalizeVote{id}, std::vector<FinalCert::VoteSignature>{}));
      }
      co_await scheduler.wait_sync_work();
      h.accept_speculation(1);
      h.certify(0);
      co_await scheduler.wait_sync_work();
      EXPECT_EQ(h.votes(1), 0u);
      co_return {};
    });
  }
}

TEST(SpeculativeValidation, RejectedOrIneligibleParentCannotStartChildEarly) {
  for (bool reject : {false, true}) {
    run_scenario([reject](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
      h.reject_parent = reject;
      h.parent_can_validate_child = false;
      h.bus.publish<CandidateReceived>(h.candidate(0));
      h.bus.publish<CandidateReceived>(h.candidate(1));
      co_await scheduler.wait_sync_work();
      EXPECT_EQ(h.speculative_calls[1], 0u);
      EXPECT_EQ(h.votes(1), 0u);
      co_return {};
    });
  }
}

TEST(SpeculativeValidation, UnusedCompletedSpeculationReleasesCandidateAfterStop) {
  run_scenario([](Harness& h, td::actor::TestScheduler& scheduler) -> td::actor::Task<> {
    co_await start_parent_and_child(h, scheduler);
    auto child = h.candidates.at(1);
    h.accept_speculation(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(h.votes(1), 0u);
    h.stop();
    co_await scheduler.wait_sync_work();
    h.parent_waiters.clear();
    h.speculative_requests.clear();
    h.candidates.erase(1);
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(child->get_refcnt(), 1);
    co_return {};
  });
}

}  // namespace
}  // namespace ton::validator::consensus::simplex::test
