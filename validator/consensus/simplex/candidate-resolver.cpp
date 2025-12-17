/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace {

class CandidateResolverImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    cache_[event->candidate->id].candidate = event->candidate;
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    cache_[event->id].notar_cert.emplace(event->certificate);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const FinalizationObserved> event) {
    cache_[event->id].final_cert.emplace(event->certificate);
    last_staging_finalized_block_ = event->id;
    next_non_staging_finalized_slot_ = event->id.slot + 1;
    maybe_true_finalize().start().detach();
  }

  template <>
  td::actor::Task<CandidateId> process(BusHandle, std::shared_ptr<ResolveCandidate> request) {
    if (request->id.slot + 1 < next_non_staging_finalized_slot_) {
      co_return td::Status::Error("Slot is already finalized");
    }
    if (request->id.slot + 1 == next_non_true_finalized_slot_) {
      auto candidate = last_true_finalized_block_.value();
      CHECK(candidate == request->id);
      co_return candidate;
    }
    co_return (co_await get(request->id)).candidate->id;
  }

 private:
  using NotarCertRef = CertificateRef<NotarizeVote>;
  using FinalCertRef = CertificateRef<FinalizeVote>;

  struct Entry {
    std::optional<RawCandidateRef> candidate = std::nullopt;
    std::optional<NotarCertRef> notar_cert = std::nullopt;
    std::optional<FinalCertRef> final_cert = std::nullopt;
  };

  struct CandidateAndNotarCert {
    RawCandidateRef candidate;
    NotarCertRef notar_cert;
  };

  td::actor::Task<CandidateAndNotarCert> get(RawCandidateId candidate) {
    if (auto it = cache_.find(candidate); it != cache_.end()) {
      if (it->second.candidate.has_value() && it->second.notar_cert.has_value()) {
        co_return {
            .candidate = it->second.candidate.value(),
            .notar_cert = it->second.notar_cert.value(),
        };
      }
    }

    co_return td::Status::Error("TODO: should request candidate from other node");
  }

  td::actor::Task<> maybe_true_finalize() {
    if (is_true_finalize_running_) {
      co_return {};
    }
    is_true_finalize_running_ = true;
    auto result = co_await maybe_true_finalize_inner().wrap();
    // NOTE: This is not SCOPE_EXIT as capturing this in a destructor is potentially use-after-free.
    is_true_finalize_running_ = false;
    co_return result;
  }

  td::actor::Task<> maybe_true_finalize_inner() {
    while (last_staging_finalized_block_ != last_true_finalized_block_) {
      CHECK(next_non_true_finalized_slot_ < next_non_staging_finalized_slot_);
      co_await true_finalize_up_to(last_staging_finalized_block_.value());
    }
    co_return {};
  }

  td::actor::Task<> true_finalize_up_to(RawCandidateId block_to_finalize) {
    std::vector<CandidateAndNotarCert> sequence;
    RawParentId next_block = block_to_finalize;
    while (next_block != last_true_finalized_block_) {
      CHECK(next_block.has_value() && next_block->slot >= next_non_true_finalized_slot_);
      sequence.push_back(co_await get(next_block.value()));
      next_block = sequence.back().candidate->parent_id;
    }

    auto last_candidate = sequence.front().candidate;
    auto last_final_cert = *cache_[block_to_finalize].final_cert;
    auto last_signature_set = last_final_cert->to_signature_set(last_candidate, *owning_bus());

    ParentId parent = last_true_finalized_block_;
    for (size_t i = sequence.size(); i--;) {
      auto& [candidate, notar_cert] = sequence[i];
      if (std::holds_alternative<BlockCandidate>(candidate->block)) {
        td::Ref<block::BlockSignatureSet> cert;
        if (last_candidate->id.block == candidate->id.block) {
          cert = last_signature_set;
        } else {
          cert = notar_cert->to_signature_set(candidate, *owning_bus());
        }
        auto resolved_candidate = td::make_ref<Candidate>(parent, candidate);
        co_await owning_bus().publish<BlockFinalized>(resolved_candidate, cert);
      }

      last_true_finalized_block_ = candidate->id;
      next_non_true_finalized_slot_ = candidate->id.slot + 1;
      while (!cache_.empty() && cache_.begin()->first.slot < next_non_true_finalized_slot_) {
        cache_.erase(cache_.begin());
      }

      parent = candidate->id;
    }

    CHECK(block_to_finalize == last_true_finalized_block_);
    co_return {};
  }

  std::map<RawCandidateId, Entry> cache_;

  RawParentId last_staging_finalized_block_;
  td::uint32 next_non_staging_finalized_slot_ = 0;

  ParentId last_true_finalized_block_;
  td::uint32 next_non_true_finalized_slot_ = 0;

  bool is_true_finalize_running_ = false;
};

}  // namespace

void CandidateResolver::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<CandidateResolverImpl>("CandidateResolver");
}

}  // namespace ton::validator::consensus::simplex
