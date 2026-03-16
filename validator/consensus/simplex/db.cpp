/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "bus.h"

namespace ton::validator::consensus::simplex {

namespace tl {

using db_key_vote = ton_api::consensus_simplex_db_key_vote;
using db_key_voteRef = tl_object_ptr<db_key_vote>;

using db_ourVote = ton_api::consensus_simplex_db_ourVote;
using db_ourVoteRef = tl_object_ptr<db_ourVote>;

using db_cert = ton_api::consensus_simplex_db_cert;
using db_certRef = tl_object_ptr<db_cert>;

using db_Vote = ton_api::consensus_simplex_db_Vote;
using db_VoteRef = tl_object_ptr<db_Vote>;

using db_key_poolState = ton_api::consensus_simplex_db_key_poolState;
using db_key_poolStateRef = tl_object_ptr<db_key_poolState>;

using db_poolState = ton_api::consensus_simplex_db_poolState;
using db_poolStateRef = tl_object_ptr<db_poolState>;

using db_key_candidateResolver_notarCert = ton_api::consensus_simplex_db_key_candidateResolver_notarCert;
using db_key_candidateResolver_notarCertRef = tl_object_ptr<db_key_candidateResolver_notarCert>;

using db_candidateResolver_notarCert = ton_api::consensus_simplex_db_candidateResolver_notarCert;
using db_candidateResolver_notarCertRef = tl_object_ptr<db_candidateResolver_notarCert>;

}  // namespace tl

namespace {

class DbImpl : public td::actor::SpawnsWith<Bus>, public td::actor::ConnectsTo<Bus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  DbImpl(Bus& bus) {
    init_pool_state(bus);
    init_votes(bus);
  }

  template <>
  void handle(BusHandle, std::shared_ptr<const StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<BroadcastVote> event) {
    auto vote = event->vote.to_tl();
    auto hash = sha256_bits256(serialize_tl_object(vote, true));

    if (saved_votes.contains(hash)) {
      co_return td::Status::Error(cancelled, "Vote was already casted");
    }
    saved_votes.insert(hash);

    auto key = create_serialize_tl_object<tl::db_key_vote>(hash);
    auto value = create_serialize_tl_object<tl::db_ourVote>(std::move(vote), next_seqno_++);

    auto result = co_await owning_bus()->db->set(std::move(key), std::move(value)).wrap();
    // We explicitly do not handle write failures here. Handling them will require an already very
    // complicated code in Pool to become even more complicated. If `set` returns `cancelled`, this
    // means that the whole consensus bus is shutting down because the group was rotated and thus
    // write persistence doesn't matter.
    CHECK(result.is_ok() || result.error().code() == cancelled);
    co_return result;
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<SaveCertificate> event) {
    auto cert = event->cert->to_tl();
    auto hash = sha256_bits256(serialize_tl_object(cert, true));

    if (saved_votes.contains(hash)) {
      co_return td::Status::Error(cancelled, "Certificate was already saved");
    }
    saved_votes.insert(hash);

    auto key = create_serialize_tl_object<tl::db_key_vote>(hash);
    auto value = create_serialize_tl_object<tl::db_cert>(std::move(cert));
    auto result = co_await owning_bus()->db->set(std::move(key), std::move(value)).wrap();
    CHECK(result.is_ok() || result.error().code() == cancelled);  // See above.
    co_return result;
  }

  template <>
  td::actor::Task<> process(BusHandle, std::shared_ptr<LeaderWindowObserved> event) {
    auto window = event->start_slot / owning_bus()->simplex_config.slots_per_leader_window;
    CHECK(first_nonannounced_window_ <= window);
    first_nonannounced_window_ = window + 1;

    auto value = create_serialize_tl_object<tl::db_poolState>(first_nonannounced_window_);
    auto result = co_await owning_bus()->db->set(pool_state_key.clone(), std::move(value)).wrap();
    CHECK(result.is_ok() || result.error().code() == cancelled);  // See above.
    co_return result;
  }

 private:
  void init_pool_state(Bus& bus) {
    auto pool_state_str = bus.db->get(pool_state_key);
    if (pool_state_str.has_value()) {
      auto pool_state = fetch_tl_object<tl::db_poolState>(*pool_state_str, true).move_as_ok();
      first_nonannounced_window_ = pool_state->first_nonannounced_window_;
      bus.first_nonannounced_window = first_nonannounced_window_;
    }
  }

  void init_votes(Bus& bus) {
    struct OurVote {
      td::int64 seqno;
      Vote vote;

      std::strong_ordering operator<=>(const OurVote& other) const {
        return seqno <=> other.seqno;
      }
    };

    std::vector<OurVote> our_votes;
    std::vector<CertificateRef<Vote>> certs;

    auto votes = bus.db->get_by_prefix(tl::db_key_vote::ID);

    for (auto& [key_str, value_str] : votes) {
      auto key = fetch_tl_object<tl::db_key_vote>(key_str, true).move_as_ok();
      saved_votes.insert(key->vote_hash_);

      auto value = fetch_tl_object<tl::db_Vote>(value_str, true).move_as_ok();

      auto our_vote_fn = [&](tl::db_ourVote& vote) {
        our_votes.push_back(OurVote{vote.seqno_, Vote::from_tl(*vote.vote_)});
      };
      auto cert_fn = [&](tl::db_cert& vote) {
        certs.push_back(Certificate<Vote>::from_tl(std::move(*vote.cert_), bus).move_as_ok());
      };

      ton_api::downcast_call(*value, td::overloaded(our_vote_fn, cert_fn));
    }
    std::sort(our_votes.begin(), our_votes.end());
    if (!our_votes.empty()) {
      next_seqno_ = our_votes.back().seqno + 1;
    }

    bus.bootstrap_certificates = std::move(certs);
    bus.bootstrap_votes = td::transform(our_votes, [](const OurVote& v) { return v.vote; });
  }

  const td::BufferSlice pool_state_key = create_serialize_tl_object<tl::db_key_poolState>();
  std::set<Bits256> saved_votes;
  td::uint32 first_nonannounced_window_ = 0;
  td::int64 next_seqno_ = 0;
};

}  // namespace

void Db::register_in(td::actor::Runtime& runtime) {
  runtime.register_actor<DbImpl>("SimplexDb");
}

}  // namespace ton::validator::consensus::simplex
