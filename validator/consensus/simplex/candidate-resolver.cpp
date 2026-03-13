/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/utils/Random.h"

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace tl {

using candidateAndCert = ton_api::consensus_simplex_candidateAndCert;
using CandidateAndCertRef = tl_object_ptr<candidateAndCert>;

using requestCandidate = ton_api::consensus_simplex_requestCandidate;
using RequestCandidateRef = tl_object_ptr<requestCandidate>;

using db_key_candidateResolver_candidateInfo = ton_api::consensus_simplex_db_key_candidateResolver_candidateInfo;
using db_key_candidateResolver_candidateInfoRef = tl_object_ptr<db_key_candidateResolver_candidateInfo>;

using db_candidateResolver_candidateInfo = ton_api::consensus_simplex_db_candidateResolver_candidateInfo;
using db_candidateResolver_candidateInfoRef = tl_object_ptr<db_candidateResolver_candidateInfo>;

using db_key_candidate = ton_api::consensus_simplex_db_key_candidate;
using db_key_candidateRef = tl_object_ptr<db_key_candidate>;

}  // namespace tl

namespace {
using BlockSignatureSetRef = td::Ref<block::BlockSignatureSet>;

struct CandidateAndCert {
  static td::Result<CandidateAndCert> from_tl(tl::candidateAndCert &&entry, const tl::requestCandidate &request,
                                              const Bus &bus) {
    if (!entry.candidate_.empty() && !request.want_candidate_) {
      return td::Status::Error("Candidate was not requested but was provided");
    }
    if (!entry.notar_.empty() && !request.want_notar_) {
      return td::Status::Error("Notar cert was not requested but was provided");
    }

    auto id = CandidateId::from_tl(request.id_);
    CandidateAndCert result;

    if (!entry.candidate_.empty()) {
      TRY_RESULT(candidate, Candidate::deserialize(entry.candidate_, bus));
      if (candidate->id != id) {
        return td::Status::Error("Candidate id mismatch");
      }
      result.candidate = candidate;
    }
    if (!entry.notar_.empty()) {
      auto vote = NotarizeVote{id};
      TRY_RESULT(signatures, fetch_tl_object<tl::voteSignatureSet>(entry.notar_, true));
      TRY_RESULT_ASSIGN(result.notar_cert, NotarCert::from_tl(std::move(*signatures), vote, bus));
    }
    return result;
  }

  tl::CandidateAndCertRef to_tl(const tl::requestCandidate &request) {
    auto id = CandidateId::from_tl(request.id_);

    td::BufferSlice serialized_candidate;
    if (request.want_candidate_ && candidate.has_value()) {
      CHECK((*candidate)->id == id);
      serialized_candidate = (*candidate)->serialize();
    }

    td::BufferSlice serialized_notar;
    if (request.want_notar_ && notar_cert.has_value()) {
      serialized_notar = serialize_tl_object((*notar_cert)->to_tl_vote_signature_set(), true);
    }

    return create_tl_object<tl::candidateAndCert>(std::move(serialized_candidate), std::move(serialized_notar));
  }

  bool is_complete() const {
    return candidate.has_value() && notar_cert.has_value();
  }

  ResolveCandidate::Result as_resolution_result() const {
    CHECK(is_complete());
    return {*candidate, *notar_cert};
  }

  tl::RequestCandidateRef make_request(CandidateId id) const {
    return create_tl_object<tl::requestCandidate>(id.to_tl(), !candidate.has_value(), !notar_cert.has_value());
  }

  void merge(const CandidateAndCert &other) {
    if (!candidate.has_value() && other.candidate.has_value()) {
      candidate = other.candidate;
    }
    if (!notar_cert.has_value() && other.notar_cert.has_value()) {
      notar_cert = other.notar_cert;
    }
  }

  std::optional<CandidateRef> candidate = std::nullopt;
  std::optional<NotarCertRef> notar_cert = std::nullopt;
};

class CandidateResolverImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    load_from_db();
  }

  void tear_down() override {
    for (auto &[_, s] : state_) {
      for (auto &p : s.resolve_awaiters) {
        p.set_error(td::Status::Error(ErrorCode::cancelled, "cancelled"));
      }
      for (auto &p : s.store_awaiters) {
        p.set_error(td::Status::Error(ErrorCode::cancelled, "cancelled"));
      }
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle, std::shared_ptr<IncomingOverlayRequest> event) {
    auto request = co_await fetch_tl_object<tl::requestCandidate>(event->request.data, true);
    auto id = CandidateId::from_tl(request->id_);

    auto it = state_.find(id);
    if (it == state_.end()) {
      co_return ProtocolMessage{CandidateAndCert{}.to_tl(*request)};
    }
    if (request->want_candidate_) {
      co_await try_load_candidate_data_from_db(id, it->second);
    }
    co_return ProtocolMessage{it->second.candidate_and_cert.to_tl(*request)};
  }

  template <>
  td::actor::Task<ResolveCandidate::Result> process(BusHandle bus, std::shared_ptr<ResolveCandidate> request) {
    CandidateState &state = state_[request->id];

    if (state.candidate_and_cert.is_complete()) {
      co_return state.candidate_and_cert.as_resolution_result();
    }

    auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    state.resolve_awaiters.push_back(std::move(promise));

    if (state.resolve_awaiters.size() == 1) {
      resolve_candidate_task(request->id, state).start().detach();
    }

    co_await std::move(task);
    co_return state.candidate_and_cert.as_resolution_result();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<StoreCandidate> request) {
    auto &state = state_[request->candidate->id];

    state.candidate_and_cert.candidate = request->candidate;
    maybe_resume_resolve_awaiters(state);

    if (state.candidate_stored) {
      co_return {};
    }

    auto [task, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
    state.store_awaiters.push_back(std::move(promise));

    if (state.store_awaiters.size() == 1) {
      auto result = co_await store_candidate(request->candidate->id, state).wrap();

      for (auto &p : state.store_awaiters) {
        p.set_result(result.clone());
      }
      state.store_awaiters.clear();
    }

    co_await std::move(task);
    co_return {};
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    auto &state = state_[event->id];
    state.candidate_and_cert.notar_cert = event->certificate;
    maybe_resume_resolve_awaiters(state);
  }

 private:
  struct CandidateState {
    bool candidate_in_db = false;
    bool candidate_stored = false;
    CandidateAndCert candidate_and_cert;

    std::vector<td::Promise<td::Unit>> resolve_awaiters;
    std::vector<td::Promise<td::Unit>> store_awaiters;
  };

  std::map<CandidateId, CandidateState> state_;

  void load_from_db() {
    auto &bus = *owning_bus();

    size_t notar_certs_count = 0;
    size_t candidate_count = 0;

    // Load all notarization certificates we have.
    for (auto cert : bus.bootstrap_certificates) {
      if (std::holds_alternative<NotarizeVote>(cert->vote.vote)) {
        std::move(cert.write()).consume_and_downcast([&]<typename T>(CertificateRef<T> cert) {
          if constexpr (std::same_as<T, NotarizeVote>) {
            state_[cert->vote.id].candidate_and_cert.notar_cert = cert;
          }
        });
        ++notar_certs_count;
      }
    }

    // Load all candidate metadata entries we have.
    auto candidates = bus.db->get_by_prefix(tl::db_key_candidateResolver_candidateInfo::ID);
    for (auto &[key_str, value_str] : candidates) {
      auto key = fetch_tl_object<tl::db_key_candidateResolver_candidateInfo>(key_str, true).move_as_ok();
      CandidateId id = CandidateId::from_tl(key->candidateId_);
      auto &state = state_[id];

      if (value_str.empty()) {
        ++candidate_count;
        state.candidate_in_db = true;
      }
    }

    LOG(INFO) << "Loaded " << notar_certs_count << " notarization certificates and " << candidate_count
              << " candidates metadata entries from db";
  }

  td::actor::Task<bool> try_load_candidate_data_from_db(CandidateId id, CandidateState &state) {
    auto &bus = *owning_bus();

    if (state.candidate_and_cert.candidate.has_value()) {
      co_return true;
    }

    if (state.candidate_in_db) {
      auto contents_key = create_serialize_tl_object<tl::db_key_candidate>(id.to_tl());
      auto data = bus.db->get(std::move(contents_key)).value();
      state.candidate_and_cert.candidate = Candidate::deserialize(data, bus).move_as_ok();
    }

    co_return false;
  }

  void maybe_resume_resolve_awaiters(CandidateState &state) {
    if (state.candidate_and_cert.is_complete()) {
      for (auto &p : state.resolve_awaiters) {
        p.set_value({});
      }
      state.resolve_awaiters.clear();
    }
  }

  td::actor::Task<> resolve_candidate_task(CandidateId id, CandidateState &state) {
    auto result = co_await resolve_candidate_inner(id, state).wrap();
    if (result.is_ok()) {
      CHECK(state.candidate_and_cert.is_complete());
      maybe_resume_resolve_awaiters(state);
    } else {
      for (auto &p : state.resolve_awaiters) {
        p.set_result(result.clone());
      }
      state.resolve_awaiters.clear();
    }
    co_return {};
  }

  td::actor::Task<> resolve_candidate_inner(CandidateId id, CandidateState &state) {
    auto &bus = *owning_bus();

    co_await try_load_candidate_data_from_db(id, state);

    if (bus.validator_set.size() == 1) {
      CHECK(state.candidate_and_cert.is_complete());
      co_return {};
    }

    double timeout_s = bus.candidate_resolve_initial_timeout_s;

    while (!state.candidate_and_cert.is_complete()) {
      auto request_tl = state.candidate_and_cert.make_request(id);
      ProtocolMessage request{serialize_tl_object(request_tl, true)};

      size_t peer_idx = td::Random::fast(0, static_cast<int>(bus.validator_set.size()) - 2);
      if (peer_idx >= bus.local_id.idx.value()) {
        peer_idx += 1;
      }
      PeerValidatorId peer{peer_idx};

      auto timeout = td::Timestamp::in(timeout_s);
      auto maybe_response =
          co_await owning_bus().publish<OutgoingOverlayRequest>(peer, timeout, std::move(request)).wrap();

      if (maybe_response.is_ok()) {
        auto response = maybe_response.move_as_ok();
        auto response_tl_r = fetch_tl_object<tl::candidateAndCert>(response.data, true);
        if (response_tl_r.is_ok()) {
          auto data_r = CandidateAndCert::from_tl(std::move(*response_tl_r.move_as_ok()), *request_tl, bus);
          if (data_r.is_ok()) {
            state.candidate_and_cert.merge(data_r.move_as_ok());
          }
        }
      }

      timeout_s = std::min(timeout_s * bus.candidate_resolve_timeout_multiplier, bus.candidate_resolve_max_timeout_s);
    }

    co_return {};
  }

  td::actor::Task<> store_candidate(CandidateId id, CandidateState &state) {
    auto &bus = *owning_bus();

    auto candidate = state.candidate_and_cert.candidate;
    CHECK(candidate.has_value());

    if (state.candidate_stored) {
      co_return {};
    }

    auto contents_key = create_serialize_tl_object<tl::db_key_candidate>(id.to_tl());
    co_await bus.db->set(std::move(contents_key), (*candidate)->serialize());

    auto index_key = create_serialize_tl_object<tl::db_key_candidateResolver_candidateInfo>(id.to_tl());
    co_await bus.db->set(std::move(index_key), td::BufferSlice());

    state.candidate_stored = true;
    co_return {};
  }
};

}  // namespace

void CandidateResolver::register_in(td::actor::Runtime &runtime) {
  runtime.register_actor<CandidateResolverImpl>("CandidateResolver");
}

}  // namespace ton::validator::consensus::simplex
