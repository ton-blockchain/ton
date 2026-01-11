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

    auto id = RawCandidateId::from_tl(request.id_);
    CandidateAndCert result;

    if (!entry.candidate_.empty()) {
      TRY_RESULT(candidate, RawCandidate::deserialize(entry.candidate_, bus));
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
    auto id = RawCandidateId::from_tl(request.id_);

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

  void merge(const CandidateAndCert &other) {
    if (!candidate.has_value() && other.candidate.has_value()) {
      candidate = other.candidate;
    }
    if (!notar_cert.has_value() && other.notar_cert.has_value()) {
      notar_cert = other.notar_cert;
    }
  }

  std::optional<RawCandidateRef> candidate = std::nullopt;
  std::optional<NotarCertRef> notar_cert = std::nullopt;
};

struct BlockchainState;
using BlockchainStateRef = td::Ref<BlockchainState>;

class CandidateResolverImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    auto &state = resolve_states_[event->candidate->id];
    state.data.candidate.emplace(event->candidate);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    auto &state = resolve_states_[event->id];
    state.data.notar_cert.emplace(event->certificate);
  }

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle, std::shared_ptr<IncomingOverlayRequest> event) {
    auto request = co_await fetch_tl_object<tl::requestCandidate>(event->request.data, true);
    auto id = RawCandidateId::from_tl(request->id_);

    auto it = resolve_states_.find(id);
    if (it == resolve_states_.end()) {
      co_return ProtocolMessage{CandidateAndCert{}.to_tl(*request)};
    }
    co_return ProtocolMessage{it->second.data.to_tl(*request)};
  }

  template <>
  td::actor::Task<ResolveCandidate::Result> process(BusHandle bus, std::shared_ptr<ResolveCandidate> request) {
    ResolveState &state = resolve_states_[request->id];

    if (state.result.has_value()) {
      co_return *state.result;
    }

    auto [task, promise] = td::actor::StartedTask<ResolveCandidate::Result>::make_bridge();
    state.promises.push_back(std::move(promise));

    if (state.started) {
      co_return co_await std::move(task);
    }

    state.started = true;
    auto result = co_await resolve_candidate_inner(bus, request->id);

    for (auto &p : state.promises) {
      auto result_copy = result;
      p.set_value(std::move(result_copy));
    }
    state.promises.clear();
    state.result = result;

    co_return result;
  }

 private:
  td::actor::Task<ResolveCandidate::Result> resolve_candidate_inner(BusHandle bus, RawCandidateId id) {
    ResolveState &state = resolve_states_[id];
    double timeout_s = bus->candidate_resolve_initial_timeout_s;

    while (true) {
      if (state.is_complete()) {
        co_return ResolveCandidate::Result{*state.data.candidate, *state.data.notar_cert};
      }

      auto request_tl = state.make_request(id);
      ProtocolMessage request{serialize_tl_object(request_tl, true)};

      size_t peer_idx = td::Random::fast(0, static_cast<int>(bus->validator_set.size()) - 1);
      PeerValidatorId peer{peer_idx};

      if (peer == bus->local_id.idx) {
        peer_idx = (peer_idx + 1) % bus->validator_set.size();
        peer = PeerValidatorId{peer_idx};
      }

      auto timeout_at = td::Timestamp::in(timeout_s);
      auto maybe_response = co_await bus.publish<OutgoingOverlayRequest>(peer, timeout_at, std::move(request)).wrap();

      if (maybe_response.is_ok()) {
        auto response = maybe_response.move_as_ok();
        auto response_tl_r = fetch_tl_object<tl::candidateAndCert>(response.data, true);
        if (response_tl_r.is_ok()) {
          auto data_r = CandidateAndCert::from_tl(std::move(*response_tl_r.move_as_ok()), *request_tl, *bus);
          if (data_r.is_ok()) {
            state.data.merge(data_r.move_as_ok());

            if (state.is_complete()) {
              co_return ResolveCandidate::Result{*state.data.candidate, *state.data.notar_cert};
            }
          }
        }
      }

      timeout_s = std::min(timeout_s * bus->candidate_resolve_timeout_multiplier, bus->candidate_resolve_max_timeout_s);
    }
  }

  struct ResolveState {
    CandidateAndCert data;
    bool started = false;
    std::optional<ResolveCandidate::Result> result;
    std::vector<td::Promise<ResolveCandidate::Result>> promises;

    bool is_complete() const {
      return data.candidate.has_value() && data.notar_cert.has_value();
    }

    tl::RequestCandidateRef make_request(const RawCandidateId &id) const {
      return create_tl_object<tl::requestCandidate>(id.to_tl(), !data.candidate.has_value(),
                                                    !data.notar_cert.has_value());
    }
  };

  std::map<RawCandidateId, ResolveState> resolve_states_;
};

}  // namespace

void CandidateResolver::register_in(runtime::Runtime &runtime) {
  runtime.register_actor<CandidateResolverImpl>("CandidateResolver");
}

}  // namespace ton::validator::consensus::simplex
