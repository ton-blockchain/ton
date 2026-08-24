/*
    Portable, versioned disk input for the collation/validation benchmark.

    Corpus loading and writing are setup operations.  Callers must keep them
    outside the measured collation and validation regions.
*/
#pragma once

#include <string>

#include "benchmark/collation/fixture.h"

namespace bench::collation {

inline constexpr td::Slice kCollationCorpusFormat = "ton-collation-validation-fixture";
inline constexpr td::uint32 kCollationCorpusVersion = 1;

// meta.json workload.name of a workload.  Selecting the payload is an additive
// v1 extension: a jetton corpus keeps its historical name and bytes.
td::Slice collation_corpus_workload_name(Workload workload);

struct LoadedCorpus {
  FixtureConfig config;
  Fixture fixture;
  ton::BlockCandidate full_candidate;
  ton::BlockCandidate preloaded_candidate;
  td::uint64 expected_transactions{0};
  td::uint64 expected_gas_used{0};
};

enum class CandidateSidecarKind { Full, Preloaded };

// Writes all artifacts first, meta.json second, and manifest.sha256 last.  Each
// file is replaced atomically.  The returned lowercase hex string is
// SHA256(exact meta.json bytes), which is also the corpus id.
td::Result<std::string> write_collation_corpus(td::CSlice directory, const FixtureConfig& config,
                                               const Fixture& fixture, const ton::BlockCandidate& full_candidate,
                                               const ton::BlockCandidate& preloaded_candidate, td::uint64 transactions,
                                               td::uint64 gas_used);

// Verifies the checksum manifest, every artifact byte hash/size/root occurrence,
// the fixed v1 workload contract, all block/state ids, and the golden successor
// state before constructing independent in-memory C++ objects.  V1 fixes the
// shard predecessor at seqno 0 and therefore rejects a shard-prev block; a
// future nonzero-predecessor profile must make that artifact mandatory.
td::Result<LoadedCorpus> load_collation_corpus(td::CSlice directory);

// Loads a foreign implementation's candidate from exactly block.boc and
// collated.boc.  The block id is derived from those bytes; all predecessor,
// masterchain, topology, global-id, sidecar-shape, and successor-state checks
// are bound to the already loaded corpus fixture; transaction count and gas
// are checked against its manifest values.  This is a read-only setup operation
// and must remain outside validation timers.
td::Result<ton::BlockCandidate> load_foreign_collation_candidate(td::CSlice directory, const Fixture& fixture,
                                                                 CandidateSidecarKind sidecar_kind,
                                                                 td::uint64 expected_transactions,
                                                                 td::uint64 expected_gas_used);

}  // namespace bench::collation
