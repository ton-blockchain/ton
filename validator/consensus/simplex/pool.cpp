/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"
#include "misbehavior.h"
#include "state.h"

namespace ton::validator::consensus::simplex {

namespace {

template <typename T>
void log_certificate(const CertificateRef<T> &certificate, const Bus &bus) {
  std::string votes(bus.validator_set.size(), '.');
  for (const auto &signature : certificate->signatures) {
    votes[signature.validator.value()] = 'V';
  }
  LOG(INFO) << "Obtained certificate for " << certificate->vote << ": " << votes;
}

class Tsentrizbirkom {
 public:
  struct AddVoteResult {
    AddVoteResult(MisbehaviorRef misbehavior) : is_applied(false), misbehavior(misbehavior) {
    }

    AddVoteResult(bool is_applied) : is_applied(is_applied) {
    }

    bool is_applied;
    std::optional<MisbehaviorRef> misbehavior;
  };

  AddVoteResult add_vote(Signed<NotarizeVote> vote) {
    if (notarize_.has_value()) {
      if (notarize_->vote != vote.vote) {
        return ConflictingVotes::create(vote.serialize(), notarize_->serialize());
      }
      return false;
    }

    notarize_ = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      notarize_ = std::nullopt;
      return *misbehavior;
    }
    return true;
  }

  AddVoteResult add_vote(Signed<SkipVote> vote) {
    if (skip_.has_value()) {
      return false;
    }

    skip_ = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      skip_ = std::nullopt;
      return *misbehavior;
    }
    return true;
  }

  AddVoteResult add_vote(Signed<FinalizeVote> vote) {
    if (finalize_.has_value()) {
      if (finalize_->vote != vote.vote) {
        return ConflictingVotes::create(vote.serialize(), finalize_->serialize());
      }
      return false;
    }

    finalize_ = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      finalize_ = std::nullopt;
      return *misbehavior;
    }
    return true;
  }

  template <typename T>
  std::optional<typename Certificate<T>::VoteSignature> to_signature(const T &vote) const {
    auto tuple = std::tuple{&notarize_, &skip_, &finalize_};
    const auto &stored_vote = *std::get<const std::optional<Signed<T>> *>(tuple);

    if (!stored_vote.has_value() || stored_vote->vote != vote) {
      return std::nullopt;
    }
    return typename Certificate<T>::VoteSignature{
        .validator = stored_vote->validator,
        .signature = stored_vote->signature.clone(),
    };
  }

  std::vector<ProtocolMessage> serialize() const {
    std::vector<ProtocolMessage> result;
    if (notarize_.has_value()) {
      result.push_back(notarize_->serialize());
    }
    if (skip_.has_value()) {
      result.push_back(skip_->serialize());
    }
    if (finalize_.has_value()) {
      result.push_back(finalize_->serialize());
    }
    return result;
  }

 private:
  std::optional<MisbehaviorRef> check_invariants() const {
    if (notarize_.has_value() && finalize_.has_value() && notarize_->vote.id != finalize_->vote.id) {
      return ConflictingVotes::create(notarize_->serialize(), finalize_->serialize());
    }
    if (finalize_.has_value() && skip_.has_value()) {
      return ConflictingVotes::create(finalize_->serialize(), skip_->serialize());
    }
    return std::nullopt;
  }

  std::optional<Signed<NotarizeVote>> notarize_;
  std::optional<Signed<SkipVote>> skip_;
  std::optional<Signed<FinalizeVote>> finalize_;
};

struct SlotState {
  SlotState(const Bus &bus) : votes(bus.validator_set.size()) {
  }

  template <typename T>
  CertificateRef<T> create_cert(const T &vote) const {
    std::vector<typename Certificate<T>::VoteSignature> signatures;
    for (const auto &voting_state : votes) {
      if (auto notar_sig = voting_state.to_signature(vote)) {
        signatures.emplace_back(std::move(*notar_sig));
      }
    }
    return td::make_ref<Certificate<T>>(vote, std::move(signatures));
  }

  std::vector<Tsentrizbirkom> votes;

  ValidatorWeight skip_weight = 0;
  std::map<RawCandidateId, ValidatorWeight> notarize_weight;
  std::map<RawCandidateId, ValidatorWeight> finalize_weight;

  std::optional<RawCandidateId> notarized;
  bool skipped = false;
  std::optional<RawParentId> available_base;
  bool finalized = false;
};

class PoolImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
  using State = ConsensusState<td::Unit, SlotState, td::Unit, const Bus &>;

  struct Request {
    RawCandidateId id;
    RawParentId parent;
    td::Promise<std::optional<MisbehaviorRef>> promise;
    RawCandidateRef candidate_for_proof;
  };

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto &bus = *owning_bus();

    slots_per_leader_window_ = bus.simplex_config.slots_per_leader_window;

    weight_threshold_ = (bus.total_weight * 2) / 3 + 1;

    state_.emplace(State(bus.simplex_config.slots_per_leader_window, {}, bus));
    state_->slot_at(0)->state->available_base = RawParentId{};

    LOG(INFO) << "Validator group started. We are " << bus.local_id << " with weight " << bus.local_id.weight
              << " out of " << bus.total_weight;

    owning_bus().publish<LeaderWindowObserved>(0, RawParentId{});

    reschedule_standstill_resolution();
    // FIXME: Load our existing votes from disk
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const IncomingProtocolMessage> message) {
    auto &bus = *owning_bus();

    auto maybe_vote = Signed<Vote>::deserialize(message->message.data, message->source, bus);
    if (maybe_vote.is_error()) {
      LOG(WARNING) << "MISBEHAVIOR: Dropping invalid vote from " << message->source;
      return;
    }

    handle_vote(message->source.get_using(bus), maybe_vote.move_as_ok());
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const BroadcastVote> event) {
    handle_our_vote(event->vote).start().detach();
  }

  template <>
  td::actor::Task<std::optional<MisbehaviorRef>> process(BusHandle, std::shared_ptr<WaitForParent> request) {
    const auto &candidate = request->candidate;
    CHECK(!candidate->parent_id.has_value() || candidate->parent_id->slot < candidate->id.slot);

    auto [bridge, promise] = td::actor::StartedTask<std::optional<MisbehaviorRef>>::make_bridge();
    requests_.push_back(Request{
        .id = candidate->id,
        .parent = candidate->parent_id,
        .promise = std::move(promise),
        .candidate_for_proof = request->candidate,
    });
    if (maybe_resolve_request(requests_.back())) {
      requests_.pop_back();
    }
    co_return co_await std::move(bridge);
  }

  void alarm() override {
    LOG(WARNING) << "Standstill detected, re-broadcasting votes";
    auto &bus = *owning_bus();
    auto [begin, end] = state_->tracked_slots_interval();

    for (td::uint32 i = begin; i < end; ++i) {
      auto slot = state_->slot_at(i);
      auto votes = slot->state->votes[bus.local_id.idx.value()].serialize();
      for (auto &vote : votes) {
        owning_bus().publish<OutgoingProtocolMessage>(std::nullopt, std::move(vote));
      }
    }

    reschedule_standstill_resolution();
  }

 private:
  void reschedule_standstill_resolution() {
    alarm_timestamp() = td::Timestamp::in(owning_bus()->standstill_timeout_s);
  }

  void handle_vote(const PeerValidator &validator, Signed<Vote> vote) {
    auto vote_fn = [&](auto vote) {
      auto slot = state_->slot_at(vote.vote.referenced_slot());
      if (!slot) {
        LOG(WARNING) << "Dropping " << vote.vote << " from " << validator << " which references a finalized slot";
        return;
      }

      auto add_result = slot->state->votes[validator.idx.value()].add_vote(std::move(vote));

      if (auto misbehavior = add_result.misbehavior) {
        owning_bus().publish<MisbehaviorReport>(validator.idx, *misbehavior);
        return;
      }

      if (add_result.is_applied) {
        handle_vote(validator, std::move(vote), *slot);
      }
    };
    std::move(vote).consume_and_downcast(vote_fn);
  }

  void handle_vote(const PeerValidator &validator, Signed<NotarizeVote> vote, State::SlotRef &slot) {
    auto new_weight = (slot.state->notarize_weight[vote.vote.id] += validator.weight);
    if (!slot.state->notarized && new_weight >= weight_threshold_) {
      on_notarization(slot, vote.vote.id, slot.state->create_cert(vote.vote));
    }
  }

  void handle_vote(const PeerValidator &validator, Signed<SkipVote> vote, State::SlotRef &slot) {
    auto new_weight = (slot.state->skip_weight += validator.weight);
    if (!slot.state->skipped && new_weight >= weight_threshold_) {
      on_skip(slot, vote.vote.slot, slot.state->create_cert(vote.vote));
    }
  }

  void handle_vote(const PeerValidator &validator, Signed<FinalizeVote> vote, State::SlotRef &slot) {
    auto new_weight = (slot.state->finalize_weight[vote.vote.id] += validator.weight);
    if (!slot.state->finalized && new_weight >= weight_threshold_) {
      on_finalization(slot, vote.vote.id, slot.state->create_cert(vote.vote));
    }
  }

  td::actor::Task<> handle_our_vote(Vote vote) {
    auto &bus = *owning_bus();

    auto vote_to_sign = serialize_tl_object(vote.to_tl(), true);
    auto data_to_sign = create_serialize_tl_object<consensus::tl::dataToSign>(bus.session_id, std::move(vote_to_sign));
    auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                             std::move(data_to_sign));

    Signed<Vote> signed_vote{bus.local_id.idx, vote, std::move(signature)};

    // FIXME: Store on disk

    owning_bus().publish(std::make_shared<OutgoingProtocolMessage>(std::nullopt, signed_vote.serialize()));
    handle_vote(bus.local_id, std::move(signed_vote));

    co_return {};
  }

  void maybe_publish_new_leader_windows() {
    while (true) {
      auto slot = state_->slot_at(now_);
      if (slot->state->notarized.has_value() || slot->state->skipped) {
        ++now_;
      } else {
        break;
      }
    }

    td::uint32 new_window = now_ / slots_per_leader_window_;
    if (new_window >= first_nonannounced_window_) {
      const auto &base = state_->slot_at(now_)->state->available_base;
      CHECK(base.has_value());
      owning_bus().publish<LeaderWindowObserved>(now_, *base);
      first_nonannounced_window_ = new_window + 1;
    }
  }

  State::SlotRef next_nonskipped_slot_after(int slot) {
    auto next_slot = state_->slot_at(slot + 1);
    if (next_slot->state->skipped) {
      next_slot = state_->slot_at(*skip_intervals_.lower_bound(slot + 1));
    }
    return *next_slot;
  }

  bool maybe_resolve_request(Request &request_) {
    auto resolve_with = [&](td::Result<std::optional<MisbehaviorRef>> result) {
      request_.promise.set_result(std::move(result));
      return true;
    };

    auto id = request_.id;
    auto parent = request_.parent;
    td::uint32 next_slot_after_parent = parent.has_value() ? parent->slot + 1 : 0;

    if (id.slot < first_nonfinalized_slot_) {
      return resolve_with(td::Status::Error("Candidate's slot is already finalized"));
    }
    if (next_slot_after_parent < first_nonfinalized_slot_) {
      return resolve_with(ConflictingCandidateAndCertificate::create(
          /* candidate, last_finalization_cert */));
    }

    auto slot = state_->slot_at(id.slot);

    if (auto notarized_block = slot->state->notarized) {
      if (notarized_block == id) {
        return resolve_with(td::Status::Error("Notarization cert for the candidate already exists"));
      } else {
        return resolve_with(ConflictingCandidateAndCertificate::create(/* candidate, notarization_cert(slot) */));
      }
    }

    if (next_slot_after_parent == first_nonfinalized_slot_) {
      if (last_finalized_block_ != parent) {
        // Here, if `first_nonfinalized_slot_ == 0`, `!parent.has_value()`. But
        // `first_nonfinalized_slot_ == 0` <=> `!last_finalized_block_.has_value()`, so
        CHECK(first_nonfinalized_slot_ != 0);

        return resolve_with(ConflictingCandidateAndCertificate::create(
            /* candidate, notarization_cert(first_nonfinalized_slot_ - 1) */));
      }
    } else {
      // Here, `next_slot_after_parent > first_nonfinalized_slot_ >= 0`, so
      CHECK(parent.has_value());

      auto parent_slot = state_->slot_at(parent->slot);
      if (parent_slot->state->notarized.has_value()) {
        if (parent_slot->state->notarized != parent) {
          return resolve_with(ConflictingCandidateAndCertificate::create(
              /* candidate, notarization_cert(slot) */));
        }
      } else {
        // Parent is not yet notarized, will try our luck later.
        return false;
      }
    }

    if (next_slot_after_parent == id.slot) {
      return resolve_with(std::nullopt);
    }

    auto next_slot = state_->slot_at(next_slot_after_parent);
    if (!next_slot->state->skipped) {
      // Too early, don't have enough skip certificates.
      return false;
    }

    if (*skip_intervals_.lower_bound(next_slot_after_parent) >= id.slot) {
      return resolve_with(std::nullopt);
    }

    return false;
  }

  void maybe_resolve_requests() {
    for (size_t i = 0; i < requests_.size(); ++i) {
      if (maybe_resolve_request(requests_[i])) {
        if (i + 1 != requests_.size()) {
          std::swap(requests_[i], requests_.back());
          // FIXME: Catch bug with i == 0 with stress.
          --i;
        }
        requests_.pop_back();
      }
    }
  }

  void on_notarization(State::SlotRef &slot, RawCandidateId id, NotarCertRef cert) {
    log_certificate(cert, *owning_bus());
    owning_bus().publish<NotarizationObserved>(id, cert);
    owning_bus().publish<StatsTargetReached>(StatsTargetReached::NotarObserved, id.slot);
    slot.state->notarized = id;

    next_nonskipped_slot_after(id.slot).state->available_base = id;

    maybe_publish_new_leader_windows();
    maybe_resolve_requests();
  }

  void on_skip(State::SlotRef &slot, td::uint32 i, SkipCertRef cert) {
    log_certificate(cert, *owning_bus());
    auto next_slot = next_nonskipped_slot_after(i);

    slot.state->skipped = true;
    skip_intervals_.erase(i);
    if (next_slot.i == i + 1) {
      skip_intervals_.insert(i + 1);
    }

    if (!next_slot.state->available_base.has_value()) {
      next_slot.state->available_base = slot.state->available_base;
    }

    maybe_publish_new_leader_windows();
    maybe_resolve_requests();
  }

  void on_finalization(State::SlotRef &slot, RawCandidateId id, FinalCertRef cert) {
    log_certificate(cert, *owning_bus());
    slot.state->finalized = true;
    CHECK(!slot.state->skipped);
    CHECK(slot.state->notarized.value_or(id) == id);
    if (!slot.state->notarized) {
      slot.state->notarized = id;
      next_nonskipped_slot_after(id.slot).state->available_base = id;
    }

    last_finalized_block_ = id;
    first_nonfinalized_slot_ = id.slot + 1;
    owning_bus().publish<StatsTargetReached>(StatsTargetReached::FinalObserved, id.slot);
    owning_bus().publish<FinalizationObserved>(id, cert);

    if (now_ <= id.slot) {
      now_ = id.slot + 1;
      maybe_publish_new_leader_windows();
    }

    while (!skip_intervals_.empty() && *skip_intervals_.begin() <= id.slot) {
      skip_intervals_.erase(skip_intervals_.begin());
    }

    state_->notify_finalized(id.slot);

    maybe_resolve_requests();
    reschedule_standstill_resolution();
  }

  td::uint32 slots_per_leader_window_;
  ValidatorWeight weight_threshold_ = 0;
  std::optional<State> state_;

  td::uint32 now_ = 0;

  std::set<td::uint32> skip_intervals_;
  RawParentId last_finalized_block_;
  td::uint32 first_nonfinalized_slot_ = 0;
  td::uint32 first_nonannounced_window_ = 1;

  std::vector<Request> requests_;
};

}  // namespace

void Pool::register_in(runtime::Runtime &runtime) {
  runtime.register_actor<PoolImpl>("SimplexPool");
}

}  // namespace ton::validator::consensus::simplex
