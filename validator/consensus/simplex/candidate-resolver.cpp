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

struct BlockchainState;
using BlockchainStateRef = td::Ref<BlockchainState>;

class CandidateResolverImpl : public runtime::SpawnsWith<Bus>, public runtime::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    load_from_db();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const CandidateReceived> event) {
    auto &state = resolve_states_[event->candidate->id];
    if (!state.data.candidate.has_value()) {
      state.data.candidate.emplace(event->candidate);
      store_to_db(event->candidate->id, state).start().detach();
    }
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const NotarizationObserved> event) {
    auto &state = resolve_states_[event->id];
    if (!state.data.notar_cert.has_value()) {
      state.data.notar_cert.emplace(event->certificate);
      store_to_db(event->id, state).start().detach();
    }
  }

  template <>
  td::actor::Task<ProtocolMessage> process(BusHandle, std::shared_ptr<IncomingOverlayRequest> event) {
    auto request = co_await fetch_tl_object<tl::requestCandidate>(event->request.data, true);
    auto id = CandidateId::from_tl(request->id_);

    auto it = resolve_states_.find(id);
    if (it == resolve_states_.end()) {
      co_return ProtocolMessage{CandidateAndCert{}.to_tl(*request)};
    }
    if (request->want_candidate_) {
      co_await try_load_candidate_data_from_db(id);
    }
    co_return ProtocolMessage{it->second.data.to_tl(*request)};
  }

  template <>
  td::actor::Task<ResolveCandidate::Result> process(BusHandle bus, std::shared_ptr<ResolveCandidate> request) {
    ResolveState &state = resolve_states_[request->id];

    if (state.result.has_value() && state.stored_data_to_db && state.stored_info_to_db &&
        state.stored_notar_cert_to_db) {
      co_return *state.result;
    }

    auto [task, promise] = td::actor::StartedTask<ResolveCandidate::Result>::make_bridge();
    state.promises.push_back(std::move(promise));

    if (state.started) {
      co_return co_await std::move(task);
    }

    state.started = true;
    auto result = co_await resolve_candidate_inner(bus, request->id);
    co_await store_to_db(request->id, state);

    for (auto &p : state.promises) {
      auto result_copy = result;
      p.set_value(std::move(result_copy));
    }
    state.promises.clear();
    state.result = result;

    co_return result;
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<WaitCandidateInfoStored> request) {
    ResolveState &state = resolve_states_[request->id];
    if (request->wait_candidate_info && !state.stored_info_to_db) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      state.stored_info_to_db_waiters.push_back(std::move(promise));
      co_await std::move(task);
    }
    if (request->wait_notar_cert && !state.stored_notar_cert_to_db) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      state.stored_notar_cert_to_db_waiters.push_back(std::move(promise));
      co_await std::move(task);
    }
    co_return {};
  }

 private:
  td::actor::Task<> try_load_candidate_data_from_db(CandidateId id) {
    ResolveState &state = resolve_states_[id];
    if (state.candidate_info_from_db.has_value() && !state.data.candidate.has_value()) {
      ResolveState::CandidateInfo &info = *state.candidate_info_from_db;
      auto r_candidate =
          co_await td::actor::ask(owning_bus()->manager, &ManagerFacade::load_block_candidate,
                                  info.leader_id.get_using(*owning_bus()).key, info.block_id, info.collated_file_hash)
              .wrap();
      if (r_candidate.is_error()) {
        LOG(WARNING) << "Failed to load block candidate data from db: " << r_candidate.move_as_error();
      } else {
        state.data.candidate = td::make_ref<Candidate>(id, info.parent, info.leader_id, r_candidate.move_as_ok(),
                                                       std::move(info.signature));
        state.stored_data_to_db = true;
      }
      state.candidate_info_from_db = std::nullopt;
    }
    co_return {};
  }

  td::actor::Task<ResolveCandidate::Result> resolve_candidate_inner(BusHandle bus, CandidateId id) {
    co_await try_load_candidate_data_from_db(id);
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

  void load_from_db() {
    auto &bus = *owning_bus();

    auto notar_certs = bus.db->get_by_prefix(ton_api::consensus_simplex_db_key_candidateResolver_notarCert::ID);
    for (auto &[key_str, value_str] : notar_certs) {
      auto key = fetch_tl_object<ton_api::consensus_simplex_db_key_candidateResolver_notarCert>(key_str, true)
                     .ensure()
                     .move_as_ok();
      auto value = fetch_tl_object<ton_api::consensus_simplex_db_candidateResolver_notarCert>(value_str, true)
                       .ensure()
                       .move_as_ok();
      CandidateId id = CandidateId::from_tl(key->candidateId_);
      auto notar_cert = NotarCert::from_tl(std::move(*value->notar_), NotarizeVote{id}, bus).ensure().move_as_ok();
      ResolveState &state = resolve_states_[id];
      state.stored_notar_cert_to_db = true;
      state.data.notar_cert = std::move(notar_cert);
    }
    LOG(INFO) << "Loaded " << notar_certs.size() << " notarize certificates from DB";

    auto candidates = bus.db->get_by_prefix(ton_api::consensus_simplex_db_key_candidateResolver_candidateInfo::ID);
    for (auto &[key_str, value_str] : candidates) {
      auto key = fetch_tl_object<ton_api::consensus_simplex_db_key_candidateResolver_candidateInfo>(key_str, true)
                     .ensure()
                     .move_as_ok();
      auto value = fetch_tl_object<ton_api::consensus_simplex_db_candidateResolver_candidateInfo>(value_str, true)
                       .ensure()
                       .move_as_ok();
      CandidateId id = CandidateId::from_tl(key->candidateId_);
      PeerValidatorId leader_id((size_t)value->leader_id_);
      auto hash_data = CandidateHashData::from_tl(std::move(*value->candidate_hash_data_));
      BlockIdExt block_id = hash_data.block();

      ResolveState &state = resolve_states_[id];
      state.stored_info_to_db = true;
      if (std::holds_alternative<CandidateHashData::EmptyCandidate>(hash_data.candidate)) {
        state.data.candidate =
            td::make_ref<Candidate>(id, hash_data.parent, leader_id, block_id, std::move(value->signature_));
        state.stored_data_to_db = true;
      } else {
        state.candidate_info_from_db = ResolveState::CandidateInfo{
            .leader_id = leader_id,
            .block_id = block_id,
            .collated_file_hash = std::get<CandidateHashData::FullCandidate>(hash_data.candidate).collated_file_hash,
            .parent = hash_data.parent,
            .signature = std::move(value->signature_)};
      }
    }
    LOG(INFO) << "Loaded " << candidates.size() << " candidates from DB";
  }

  struct ResolveState {
    CandidateAndCert data;
    bool started = false;
    std::optional<ResolveCandidate::Result> result;
    std::vector<td::Promise<ResolveCandidate::Result>> promises;

    struct CandidateInfo {
      PeerValidatorId leader_id;
      BlockIdExt block_id;
      FileHash collated_file_hash;
      ParentId parent;
      td::BufferSlice signature;
    };
    std::optional<CandidateInfo> candidate_info_from_db;
    bool stored_info_to_db = false;
    bool stored_notar_cert_to_db = false;
    bool stored_data_to_db = false;
    std::vector<td::Promise<td::Unit>> stored_info_to_db_waiters;
    std::vector<td::Promise<td::Unit>> stored_notar_cert_to_db_waiters;

    bool is_complete() const {
      return data.candidate.has_value() && data.notar_cert.has_value();
    }

    tl::RequestCandidateRef make_request(const CandidateId &id) const {
      return create_tl_object<tl::requestCandidate>(id.to_tl(), !data.candidate.has_value(),
                                                    !data.notar_cert.has_value());
    }
  };

  td::actor::Task<> store_to_db(CandidateId id, ResolveState &state) {
    std::vector<td::actor::StartedTask<>> tasks;
    if (state.data.candidate.has_value()) {
      auto &cand = *state.data.candidate;
      if (!state.stored_info_to_db) {
        auto key =
            create_serialize_tl_object<ton_api::consensus_simplex_db_key_candidateResolver_candidateInfo>(id.to_tl());
        auto value = create_serialize_tl_object<ton_api::consensus_simplex_db_candidateResolver_candidateInfo>(
            (int)cand->leader.value(), cand->hash_data().to_tl(), cand->signature.clone());
        tasks.push_back(owning_bus()->db->set(std::move(key), std::move(value)).start());
      }
      if (!state.stored_data_to_db && std::holds_alternative<BlockCandidate>(cand->block)) {
        tasks.push_back(td::actor::ask(owning_bus()->manager, &ManagerFacade::store_block_candidate,
                                       std::get<BlockCandidate>(cand->block).clone()));
      }
    }
    if (!state.stored_notar_cert_to_db && state.data.notar_cert.has_value()) {
      auto &notar = *state.data.notar_cert;
      auto key = create_serialize_tl_object<ton_api::consensus_simplex_db_key_candidateResolver_notarCert>(id.to_tl());
      auto value = create_serialize_tl_object<ton_api::consensus_simplex_db_candidateResolver_notarCert>(
          notar->to_tl_vote_signature_set());
      tasks.push_back(owning_bus()->db->set(std::move(key), std::move(value)).start());
    }

    co_await td::actor::all(std::move(tasks));

    if (state.data.candidate.has_value()) {
      state.stored_info_to_db = true;
      for (auto &p : state.stored_info_to_db_waiters) {
        p.set_value(td::Unit{});
      }
      state.stored_info_to_db_waiters.clear();
      state.stored_data_to_db = true;
    }
    if (state.data.notar_cert.has_value()) {
      state.stored_notar_cert_to_db = true;
      for (auto &p : state.stored_notar_cert_to_db_waiters) {
        p.set_value(td::Unit{});
      }
      state.stored_notar_cert_to_db_waiters.clear();
    }
    co_return {};
  }

  std::map<CandidateId, ResolveState> resolve_states_;
};

}  // namespace

void CandidateResolver::register_in(runtime::Runtime &runtime) {
  runtime.register_actor<CandidateResolverImpl>("CandidateResolver");
}

}  // namespace ton::validator::consensus::simplex
