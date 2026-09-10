/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "consensus/simplex/bus.h"
#include "td/actor/BusRuntime.h"
#include "td/actor/TestScheduler.h"
#include "td/utils/tests.h"

namespace ton::validator::consensus::simplex::test {
namespace {

constexpr td::uint32 kCandidateSlot = 7;

size_t observed_outcomes = 0;

CandidateId candidate_id(td::uint32 slot) {
  return CandidateId{slot, td::Bits256::zero()};
}

CandidateRef empty_candidate() {
  std::variant<BlockIdExt, BlockCandidate> block{BlockIdExt{}};
  return td::make_ref<Candidate>(candidate_id(kCandidateSlot), candidate_id(kCandidateSlot - 1), PeerValidatorId{0},
                                 std::move(block), td::BufferSlice{});
}

class DiscardingDb final : public consensus::Db {
 public:
  std::optional<td::BufferSlice> get(td::Slice) const override {
    return std::nullopt;
  }

  std::vector<std::pair<td::BufferSlice, td::BufferSlice>> get_by_prefix(td::uint32) const override {
    return {};
  }

  td::actor::Task<> set(td::BufferSlice, td::BufferSlice) override {
    co_return {};
  }

  td::actor::Task<> close() override {
    co_return {};
  }
};

class OutcomeObserver final : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateOutcomeObserved> event) {
    ++observed_outcomes;
    EXPECT_EQ(event->id.slot, kCandidateSlot);
    EXPECT(event->is_empty);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }
};

TEST(CandidateResolverMetrics, PublishesOneOutcomeAfterSuccessfulResolution) {
  observed_outcomes = 0;

  td::actor::TestScheduler scheduler;
  td::actor::Runtime runtime;
  CandidateResolver::register_in(runtime);
  runtime.register_actor<OutcomeObserver>("OutcomeObserver");

  scheduler.run([&]() -> td::actor::Task<> {
    auto candidate = empty_candidate();
    auto bus = std::make_shared<Bus>();
    PeerValidator local{};
    local.idx = PeerValidatorId{0};
    bus->local_id = local;
    bus->config.noncritical_params.candidate_resolve_cooldown = std::chrono::milliseconds{2};
    bus->db = std::make_unique<DiscardingDb>();
    auto handle = runtime.start(std::move(bus), "candidate-resolver-metrics");
    co_await scheduler.wait_sync_work();

    co_await handle.publish<StoreCandidate>(candidate);

    // Exercise the asynchronous resolver path: knowing the candidate alone must not publish an
    // outcome. Resolution completes only after the notarization certificate arrives.
    auto resolution = handle.publish<ResolveCandidate>(candidate->id).start();
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(observed_outcomes, 0u);

    auto notar = td::make_ref<NotarCert>(NotarizeVote{candidate->id}, std::vector<NotarCert::VoteSignature>{});
    handle.publish<NotarizationObserved>(candidate->id, std::move(notar));

    auto first = co_await std::move(resolution);
    EXPECT_EQ(first.candidate->id.slot, kCandidateSlot);
    EXPECT(first.candidate->is_empty());
    co_await scheduler.wait_sync_work();
    EXPECT_EQ(observed_outcomes, 1u);

    // A second successful request takes the immediate path. The per-candidate publication guard
    // must suppress both this attempt and the still-finishing asynchronous resolver task.
    auto second = co_await handle.publish<ResolveCandidate>(candidate->id);
    EXPECT_EQ(second.candidate->id.slot, kCandidateSlot);
    co_await scheduler.wait_sync_work();

    if (auto timeout = scheduler.next_timeout()) {
      scheduler.advance_time_to(*timeout);
      co_await scheduler.wait_sync_work();
    }
    EXPECT_EQ(observed_outcomes, 1u);

    handle.publish<StopRequested>();
    handle = {};
    co_await scheduler.wait_sync_work();
    co_return {};
  });
}

}  // namespace
}  // namespace ton::validator::consensus::simplex::test
