/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "common/stats.h"
#include "consensus/stats.h"

#include "votes.h"

namespace ton::validator::consensus::simplex::stats {

namespace tl {

using voted = ton_api::consensus_simplex_stats_voted;
using certObserved = ton_api::consensus_simplex_stats_certObserved;

}  // namespace tl

class MetricCollector;

class Voted : public consensus::stats::CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<Voted> create(Vote vote);

  consensus::stats::tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  const Vote& vote() const {
    return vote_;
  }

 private:
  Voted(Vote vote);

  Vote vote_;
};

class CertObserved : public consensus::stats::CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CertObserved> create(Vote vote);

  consensus::stats::tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  const Vote& vote() const {
    return vote_;
  }

 private:
  CertObserved(Vote vote);

  Vote vote_;
};

struct Flow {
  std::optional<double> collate_started;
  std::optional<double> collate_finished;
  std::optional<double> candidate_received;
  std::optional<double> validation_started;
  std::optional<double> validation_finished;
  std::optional<double> notarize_voted;
  std::optional<double> notarize_cert_observed;
  std::optional<double> finalize_voted;
  std::optional<double> finalize_cert_observed;
  std::optional<double> manager_accepted;
  std::optional<BlockIdExt> block_id;
  bool is_collator = false;

  bool is_normal() const;
};

class MetricCollector final : public consensus::stats::MetricCollector {
 public:
  MetricCollector(ValidatorSessionId session_id, PublicKeyHash self_id, std::unique_ptr<ton::stats::Recorder> recorder);

  void collect_collate_started(const consensus::stats::CollateStarted& event) override;
  void collect_collate_finished(const consensus::stats::CollateFinished& event) override;
  void collect_candidate_received(const consensus::stats::CandidateReceived& event) override;
  void collect_validation_started(const consensus::stats::ValidationStarted& event) override;
  void collect_validation_finished(const consensus::stats::ValidationFinished& event) override;
  void collect_block_accepted(const consensus::stats::BlockAccepted& event) override;

  void collect_voted(const Voted& event);
  void collect_cert_observed(const CertObserved& event);

 private:
  void log_fake_catchain_stats(const Flow& flow);

  ValidatorSessionId session_id_;
  PublicKeyHash self_id_;

  std::map<CandidateId, Flow> flows_;
  std::map<td::uint32, double> collate_started_by_slot_;
  td::uint32 first_non_accepted_slot_ = 0;

  std::unique_ptr<ton::stats::Recorder> recorder_;
};

}  // namespace ton::validator::consensus::simplex::stats
