/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/actor/coro_utils.h"

#include "bus.h"
#include "checksum.h"
#include "misbehavior.h"
#include "state.h"
#include "stats.h"

namespace ton::validator::consensus::simplex {

namespace {

template <typename T>
void log_certificate(const CertificateRef<T> &certificate, const Bus &bus) {
  std::string votes(bus.validator_set.size(), '.');
  for (const auto &signature : certificate->signatures) {
    votes[signature.validator.value()] = 'V';
  }
  LOG(WARNING) << "Obtained certificate for " << certificate->vote << ": " << votes;
}

template <typename T>
struct Proven {
  Proven(Signed<T> vote) : vote(vote.vote), proof(std::move(vote)) {
  }

  Proven(CertificateRef<T> cert) : vote(cert->vote), proof(std::move(cert)) {
  }

  td::BufferSlice serialize_as_proof() const {
    auto vote_fn = [](const Signed<T> &vote) { return vote.serialize(); };
    auto cert_fn = [](const CertificateRef<T> &cert) { return cert->serialize(); };
    return std::visit(td::overloaded(vote_fn, cert_fn), proof);
  }

  const Signed<T> &as_signed_vote() const & {
    CHECK(std::holds_alternative<Signed<T>>(proof));
    return std::get<Signed<T>>(proof);
  }

  std::optional<typename Certificate<T>::VoteSignature> to_signature(const T &vote) const {
    if (vote != this->vote) {
      return std::nullopt;
    }
    auto &signed_vote = as_signed_vote();
    return typename Certificate<T>::VoteSignature{
        .validator = signed_vote.validator,
        .signature = signed_vote.signature.clone(),
    };
  }

  void serialize_to(std::vector<ProtocolMessage> &messages) const {
    messages.push_back(as_signed_vote().serialize());
  }

  T vote;
  std::variant<Signed<T>, CertificateRef<T>> proof;
};

struct CertificateBundle {
  bool needs(const Vote &vote) const {
    return std::visit(
        [&]<typename T>(const T &v) {
          auto tuple = std::tie(notarize_, skip_, finalize_);
          const auto &stored_cert = std::get<const std::optional<CertificateRef<T>> &>(tuple);
          return !stored_cert.has_value();
        },
        vote.vote);
  }

  template <typename T>
  bool store(CertificateRef<T> cert) {
    auto tuple = std::tie(notarize_, skip_, finalize_);
    auto &stored_cert = std::get<std::optional<CertificateRef<T>> &>(tuple);
    if (stored_cert.has_value()) {
      return false;
    }
    stored_cert = std::move(cert);
    return true;
  }

  void serialize_to(std::vector<ProtocolMessage> &messages) const {
    if (notarize_.has_value()) {
      messages.push_back((*notarize_)->serialize());
    }
    if (skip_.has_value()) {
      messages.push_back((*skip_)->serialize());
    }
    if (finalize_.has_value()) {
      messages.push_back((*finalize_)->serialize());
    }
  }

  std::optional<NotarCertRef> notarize_;
  std::optional<SkipCertRef> skip_;
  std::optional<FinalCertRef> finalize_;
};

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

  AddVoteResult add_vote(Proven<NotarizeVote> vote) {
    if (notarize_.has_value()) {
      if (notarize_->vote != vote.vote) {
        return ConflictingVotes::create(vote.serialize_as_proof(), notarize_->serialize_as_proof());
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

  AddVoteResult add_vote(Proven<SkipVote> vote) {
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

  AddVoteResult add_vote(Proven<FinalizeVote> vote) {
    if (finalize_.has_value()) {
      if (finalize_->vote != vote.vote) {
        return ConflictingVotes::create(vote.serialize_as_proof(), finalize_->serialize_as_proof());
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

  bool is_notarized() const {
    return notarize_.has_value();
  }

  bool is_skipped() const {
    return skip_.has_value();
  }

  bool is_finalized() const {
    return finalize_.has_value();
  }

  template <typename T>
  std::optional<typename Certificate<T>::VoteSignature> to_signature(const T &vote) const {
    auto tuple = std::tuple{&notarize_, &skip_, &finalize_};
    const auto &stored_vote = *std::get<const std::optional<Proven<T>> *>(tuple);

    if (stored_vote.has_value()) {
      return stored_vote->to_signature(vote);
    }
    return std::nullopt;
  }

  void serialize_to(std::vector<ProtocolMessage> &messages, const CertificateBundle &bundle) const {
    if (notarize_.has_value() && !bundle.notarize_.has_value()) {
      notarize_->serialize_to(messages);
    }
    if (skip_.has_value() && !bundle.skip_.has_value()) {
      skip_->serialize_to(messages);
    }
    if (finalize_.has_value() && !bundle.finalize_.has_value()) {
      finalize_->serialize_to(messages);
    }
  }

 private:
  std::optional<MisbehaviorRef> check_invariants() const {
    if (notarize_.has_value() && finalize_.has_value() && notarize_->vote.id != finalize_->vote.id) {
      return ConflictingVotes::create(notarize_->serialize_as_proof(), finalize_->serialize_as_proof());
    }
    if (finalize_.has_value() && skip_.has_value()) {
      return ConflictingVotes::create(finalize_->serialize_as_proof(), skip_->serialize_as_proof());
    }
    return std::nullopt;
  }

  std::optional<Proven<NotarizeVote>> notarize_;
  std::optional<Proven<SkipVote>> skip_;
  std::optional<Proven<FinalizeVote>> finalize_;
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

  bool is_notarized() const {
    return certs.notarize_.has_value();
  }

  std::optional<CandidateId> notarized_block() const {
    if (certs.notarize_.has_value()) {
      return (*certs.notarize_)->vote.id;
    }
    if (certs.finalize_.has_value()) {
      return (*certs.finalize_)->vote.id;
    }
    return std::nullopt;
  }

  bool is_skipped() const {
    return certs.skip_.has_value();
  }

  bool is_finalized() const {
    return certs.finalize_.has_value();
  }

  void add_available_base(ParentId parent) {
    // If we have multiple bases, choose one coming from the highest slot to maximize the chance of
    // forward-progress.
    if (!available_base.has_value() || parent >= *available_base) {
      available_base = parent;
    }
  }

  std::vector<Tsentrizbirkom> votes;
  CertificateBundle certs;

  ValidatorWeight skip_weight = 0;
  std::map<CandidateId, ValidatorWeight> notarize_weight;
  std::map<CandidateId, ValidatorWeight> finalize_weight;

  std::optional<ParentId> available_base;
};

class PoolImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
  using State = ConsensusState<td::Unit, SlotState, td::Unit, const Bus &>;

  struct Request {
    CandidateId id;
    ParentId parent;
    td::Promise<std::optional<MisbehaviorRef>> promise;
    CandidateRef candidate_for_proof;
  };

 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    auto &bus = *owning_bus();

    slots_per_leader_window_ = bus.simplex_config.slots_per_leader_window;

    weight_threshold_ = (bus.total_weight * 2) / 3 + 1;

    state_.emplace(State(bus.simplex_config.slots_per_leader_window, {}, bus));
    state_->slot_at(0)->state->available_base = ParentId{};

    LOG(INFO) << "Validator group started. We are " << bus.local_id << " with weight " << bus.local_id.weight
              << " out of " << bus.total_weight;

    first_nonannounced_window_ = bus.first_nonannounced_window;
    for (const auto &vote : bus.bootstrap_votes) {
      handle_vote(vote.validator.get_using(bus), vote.clone());
    }

    if (first_nonannounced_window_ == 0) {
      maybe_publish_new_leader_window().start().detach();
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const Start>) {
    auto &bus = *owning_bus();
    owning_bus().publish<TraceEvent>(consensus::stats::Id::create(
        bus.shard, bus.cc_seqno, bus.local_id.idx.value(), bus.validator_set.size(), bus.local_id.weight,
        bus.total_weight, bus.simplex_config.slots_per_leader_window));

    reschedule_standstill_resolution();
    is_started_ = true;
    if (leader_window_observation_) {
      owning_bus().publish(std::move(leader_window_observation_));
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const IncomingProtocolMessage> message) {
    auto &bus = *owning_bus();

    auto maybe_tl_vote = fetch_tl_object<tl::vote>(message->message.data, true);
    if (maybe_tl_vote.is_ok()) {
      auto tl_vote = maybe_tl_vote.move_as_ok();
      auto maybe_vote = Signed<Vote>::from_tl(std::move(*tl_vote), message->source, bus);
      if (maybe_vote.is_error()) {
        return;
      }

      if (handle_vote(message->source.get_using(bus), maybe_vote.move_as_ok())) {
        store_vote_to_db(message->message.data.clone(), message->source).detach();
      }
    }

    auto maybe_tl_certificate = fetch_tl_object<tl::certificate>(message->message.data, true);
    if (maybe_tl_certificate.is_ok()) {
      auto tl_certificate = maybe_tl_certificate.move_as_ok();
      auto raw_vote = Vote::from_tl(*tl_certificate->vote_);

      auto slot = state_->slot_at(raw_vote.referenced_slot());
      if (!slot.has_value()) {
        return;
      }
      if (!slot->state->certs.needs(raw_vote)) {
        return;
      }

      auto maybe_certificate = Certificate<Vote>::from_tl(std::move(*tl_certificate), bus);
      if (maybe_certificate.is_error()) {
        LOG(WARNING) << "Dropping bad certificate from " << message->source << " : "
                     << maybe_certificate.move_as_error();
        return;
      }

      handle_foreign_certificate(*slot, std::move(maybe_certificate.move_as_ok().unique_write()));
    }
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
    auto &bus = *owning_bus();
    auto [begin, end] = state_->tracked_slots_interval();

    td::StringBuilder sb;

    std::vector<ProtocolMessage> messages;
    if (last_final_cert_.has_value()) {
      sb << "Last final cert is for " << (*last_final_cert_)->vote.id << "\n";
      messages.push_back((*last_final_cert_)->serialize());
    }

    for (td::uint32 i = begin; i < end; ++i) {
      auto slot = state_->slot_at(i);
      auto &certs = slot->state->certs;

      sb << i << ": ";
      for (size_t j = 0; j < bus.validator_set.size(); ++j) {
        auto &voting_state = slot->state->votes[j];
        if (voting_state.is_finalized()) {
          sb << 'F';
        } else if (voting_state.is_notarized() && voting_state.is_skipped()) {
          sb << 'I';
        } else if (voting_state.is_notarized()) {
          sb << 'N';
        } else if (voting_state.is_skipped()) {
          sb << 'S';
        } else {
          sb << '.';
        }
      }
      if (certs.notarize_.has_value()) {
        sb << " notar";
      }
      if (certs.skip_.has_value()) {
        sb << " skip";
      }
      if (certs.finalize_.has_value()) {
        sb << " final";
      }
      sb << "\n";

      certs.serialize_to(messages);
      slot->state->votes[bus.local_id.idx.value()].serialize_to(messages, slot->state->certs);
    }

    LOG(WARNING) << "Standstill detected. Current pool state: " << sb.as_cslice();

    for (auto &vote : messages) {
      owning_bus().publish<OutgoingProtocolMessage>(std::nullopt, std::move(vote));
    }

    reschedule_standstill_resolution();
  }

 private:
  void reschedule_standstill_resolution() {
    alarm_timestamp() = td::Timestamp::in(owning_bus()->standstill_timeout_s);
  }

  void publish_misbehavior(PeerValidatorId idx, MisbehaviorRef misbehavior) {
    owning_bus().publish<MisbehaviorReport>(idx, misbehavior);
  }

  bool handle_vote(const PeerValidator &validator, Signed<Vote> vote) {
    auto vote_fn = [&]<typename VoteT>(Signed<VoteT> vote) -> bool {
      auto slot = state_->slot_at(vote.vote.referenced_slot());
      if (!slot) {
        if constexpr (std::same_as<VoteT, FinalizeVote> || std::same_as<VoteT, NotarizeVote>) {
          if (vote.vote.id == last_finalized_block_) {
            return false;
          }
        }

        LOG(WARNING) << "Dropping " << vote.vote << " from " << validator << " which references a finalized slot";
        return false;
      }

      auto add_result = slot->state->votes[validator.idx.value()].add_vote(std::move(vote));

      if (auto misbehavior = add_result.misbehavior) {
        LOG_CHECK(validator != owning_bus()->local_id)
            << "We produced conflicting votes! Conflict occured for " << vote.vote;
        // The following line cannot be simply
        // `owning_bus().publish<MisbehaviorReport>(validator.idx, *misbehavior);` as this would
        // _sometimes_ result in link errors (at least with clang-21 and libstdc++-15) because of a
        // missing complete-object destructor. I think this is because we would somehow trigger
        // https://github.com/llvm/llvm-project/issues/46044 .
        publish_misbehavior(validator.idx, *misbehavior);
        return false;
      }

      if (add_result.is_applied) {
        handle_vote(validator, std::move(vote), *slot);
        return true;
      }
      return false;
    };
    return std::move(vote).consume_and_downcast(vote_fn);
  }

  void handle_vote(const PeerValidator &validator, Signed<NotarizeVote> vote, State::SlotRef &slot) {
    auto new_weight = (slot.state->notarize_weight[vote.vote.id] += validator.weight);
    if (!slot.state->is_notarized() && new_weight >= weight_threshold_) {
      handle_certificate(slot, slot.state->create_cert(vote.vote));
    }
  }

  void handle_vote(const PeerValidator &validator, Signed<SkipVote> vote, State::SlotRef &slot) {
    auto new_weight = (slot.state->skip_weight += validator.weight);
    if (!slot.state->is_skipped() && new_weight >= weight_threshold_) {
      handle_certificate(slot, slot.state->create_cert(vote.vote));
    }
  }

  void handle_vote(const PeerValidator &validator, Signed<FinalizeVote> vote, State::SlotRef &slot) {
    auto new_weight = (slot.state->finalize_weight[vote.vote.id] += validator.weight);
    if (!slot.state->is_finalized() && new_weight >= weight_threshold_) {
      handle_certificate(slot, slot.state->create_cert(vote.vote));
    }
  }

  td::actor::Task<> handle_our_vote(Vote vote) {
    auto &bus = *owning_bus();

    owning_bus().publish<TraceEvent>(stats::Voted::create(vote));

    auto vote_to_sign = serialize_tl_object(vote.to_tl(), true);
    auto data_to_sign = create_serialize_tl_object<consensus::tl::dataToSign>(bus.session_id, std::move(vote_to_sign));
    auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                             std::move(data_to_sign));

    Signed<Vote> signed_vote{bus.local_id.idx, vote, std::move(signature)};
    td::BufferSlice serialized = signed_vote.serialize();

    if (handle_vote(bus.local_id, std::move(signed_vote))) {
      co_await store_vote_to_db(serialized.clone(), bus.local_id.idx);
      owning_bus().publish(std::make_shared<OutgoingProtocolMessage>(std::nullopt, std::move(serialized)));
    }

    co_return {};
  }

  void advance_present() {
    while (true) {
      auto slot = state_->slot_at(now_);
      if (slot->state->is_notarized() || slot->state->is_skipped()) {
        ++now_;
      } else {
        break;
      }
    }
    maybe_publish_new_leader_window().start().detach();
  }

  td::actor::Task<> maybe_publish_new_leader_window() {
    td::uint32 now_save = now_;
    td::uint32 new_window = now_ / slots_per_leader_window_;
    if (new_window < first_nonannounced_window_) {
      co_return {};
    }
    first_nonannounced_window_ = new_window + 1;
    co_await store_pool_state_to_db();

    if (now_save != now_) {
      co_return {};
    }

    ParentId base = {};
    if (now_ != 0) {
      auto maybe_base = state_->slot_at(now_)->state->available_base;
      CHECK(maybe_base.has_value());
      base = maybe_base.value();
    }
    leader_window_observation_ = std::make_shared<LeaderWindowObserved>(now_, base);
    if (is_started_) {
      owning_bus().publish(std::move(leader_window_observation_));
    }
    co_return {};
  }

  State::SlotRef next_nonskipped_slot_after(int slot) {
    auto next_slot = state_->slot_at(slot + 1);
    if (next_slot->state->is_skipped()) {
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

    if (auto notarized_block = slot->state->notarized_block()) {
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
      if (parent_slot->state->is_notarized()) {
        if (parent_slot->state->notarized_block() != parent) {
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
    if (!next_slot->state->is_skipped()) {
      // Too early, don't have enough skip certificates.
      return false;
    }

    if (*skip_intervals_.lower_bound(next_slot_after_parent) >= id.slot) {
      return resolve_with(std::nullopt);
    }

    return false;
  }

  void maybe_resolve_requests() {
    for (size_t i = 0; i < requests_.size();) {
      if (maybe_resolve_request(requests_[i])) {
        if (i + 1 != requests_.size()) {
          std::swap(requests_[i], requests_.back());
        }
        requests_.pop_back();
      } else {
        ++i;
      }
    }
  }

  void handle_foreign_certificate(State::SlotRef &slot, Certificate<Vote> &&cert) {
    std::move(cert).consume_and_downcast([&](auto cert) {
      bool rc = slot.state->certs.store(cert);
      CHECK(rc);

      for (const auto &[idx, _] : cert->signatures) {
        auto add_result = slot.state->votes[idx.value()].add_vote(cert);
        if (auto misbehavior = add_result.misbehavior) {
          owning_bus().publish<MisbehaviorReport>(idx, *misbehavior);
        }
      }
      handle_certificate(slot, cert);
    });
  }

  void handle_certificate(State::SlotRef &slot, NotarCertRef cert) {
    slot.state->certs.notarize_ = cert;
    auto id = cert->vote.id;

    log_certificate(cert, *owning_bus());
    owning_bus().publish<OutgoingProtocolMessage>(std::nullopt, cert->serialize());
    owning_bus().publish<TraceEvent>(stats::CertObserved::create(cert->vote));
    owning_bus().publish<NotarizationObserved>(id, cert);

    next_nonskipped_slot_after(id.slot).state->add_available_base(id);

    advance_present();
    maybe_resolve_requests();
  }

  void handle_certificate(State::SlotRef &slot, SkipCertRef cert) {
    slot.state->certs.skip_ = cert;
    auto i = slot.i;

    log_certificate(cert, *owning_bus());
    owning_bus().publish<OutgoingProtocolMessage>(std::nullopt, cert->serialize());
    owning_bus().publish<TraceEvent>(stats::CertObserved::create(cert->vote));
    auto next_slot = next_nonskipped_slot_after(i);

    skip_intervals_.erase(i);
    if (next_slot.i == i + 1) {
      skip_intervals_.insert(i + 1);
    }

    if (auto base = slot.state->available_base) {
      next_slot.state->add_available_base(*base);
    }

    advance_present();
    maybe_resolve_requests();
  }

  void handle_certificate(State::SlotRef &slot, FinalCertRef cert) {
    slot.state->certs.finalize_ = cert;
    auto id = cert->vote.id;

    log_certificate(cert, *owning_bus());
    owning_bus().publish<TraceEvent>(stats::CertObserved::create(cert->vote));
    CHECK(!slot.state->is_skipped());
    CHECK(slot.state->notarized_block().value_or(id) == id);
    if (!slot.state->is_notarized()) {
      next_nonskipped_slot_after(id.slot).state->add_available_base(id);
    }

    last_finalized_block_ = id;
    last_final_cert_ = cert;
    first_nonfinalized_slot_ = id.slot + 1;
    owning_bus().publish<FinalizationObserved>(id, cert);

    if (now_ <= id.slot) {
      now_ = id.slot + 1;
      advance_present();
    }

    while (!skip_intervals_.empty() && *skip_intervals_.begin() <= id.slot) {
      skip_intervals_.erase(skip_intervals_.begin());
    }

    state_->notify_finalized(id.slot);

    maybe_resolve_requests();
    reschedule_standstill_resolution();
  }

  td::actor::Task<> store_vote_to_db(td::BufferSlice serialized, PeerValidatorId validator_id) {
    td::Bits256 hash = td::sha256_bits256(serialized);
    co_return co_await owning_bus()->db->set(create_serialize_tl_object<ton_api::consensus_simplex_db_key_vote>(hash),
                                             create_serialize_tl_object<ton_api::consensus_simplex_db_vote>(
                                                 std::move(serialized), (int)validator_id.value()));
  }

  td::actor::Task<> store_pool_state_to_db() {
    co_return co_await owning_bus()->db->set(
        create_serialize_tl_object<ton_api::consensus_simplex_db_key_poolState>(),
        create_serialize_tl_object<ton_api::consensus_simplex_db_poolState>(first_nonannounced_window_));
  }

  td::uint32 slots_per_leader_window_;
  ValidatorWeight weight_threshold_ = 0;
  std::optional<State> state_;

  bool is_started_ = false;
  std::shared_ptr<LeaderWindowObserved> leader_window_observation_;
  td::uint32 now_ = 0;

  std::set<td::uint32> skip_intervals_;

  td::uint32 first_nonannounced_window_ = 0;

  ParentId last_finalized_block_;
  std::optional<FinalCertRef> last_final_cert_;
  td::uint32 first_nonfinalized_slot_ = 0;

  std::vector<Request> requests_;
};

}  // namespace

void Pool::register_in(runtime::Runtime &runtime) {
  runtime.register_actor<PoolImpl>("SimplexPool");
}

}  // namespace ton::validator::consensus::simplex
