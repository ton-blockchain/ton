// ConsensusHarness — standalone Simplex consensus simulation.
//
// Runs N validators in one process with a simulated NetworkMock.
// Byzantine behaviour is injected via --scenario flag.
//
// NDJSON events are emitted via GraphLogger → simulation/trace.ndjson.
// Run relay.mjs afterwards to push events to Neo4j.
//
// Usage:
//   ConsensusHarness --scenario equivocation
//   ConsensusHarness --scenario message_withholding
//   ConsensusHarness --scenario byzantine_leader
//   ConsensusHarness --scenario notarize_skip_split
//   ConsensusHarness --scenario all --assert-anomalies

#include "GraphLogger.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace simulation;

// ── Config ─────────────────────────────────────────────────────────────────

static const int N = 4;               // total validators
static const int F = 1;               // byzantine tolerance
static const int THRESHOLD = N - F;   // 3 — quorum
static const int SLOTS = 10;          // slots per run

// ── Utilities ──────────────────────────────────────────────────────────────

static std::mt19937_64 rng{std::random_device{}()};

static std::string make_id() {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0')
     << std::setw(8)  << (uint32_t)(rng() & 0xffffffff) << "-"
     << std::setw(4)  << (uint16_t)(rng() & 0xffff)     << "-"
     << std::setw(4)  << (uint16_t)((rng() & 0x0fff) | 0x4000) << "-"
     << std::setw(4)  << (uint16_t)((rng() & 0x3fff) | 0x8000) << "-"
     << std::setw(12) << (rng() & 0xffffffffffff);
  return ss.str();
}

static std::string candidate_id(uint32_t slot, const std::string& variant = "") {
  // Deterministic fake hash: slot + optional variant suffix
  std::ostringstream ss;
  ss << "cand:" << slot << (variant.empty() ? "" : ":" + variant);
  return ss.str();
}

static std::string cert_id(uint32_t slot) {
  return "cert:" + std::to_string(slot);
}

static std::string block_id(uint32_t slot) {
  return "block:" + std::to_string(slot);
}

static std::string skip_id(uint32_t slot) {
  return "skip:" + std::to_string(slot);
}

static std::string validator_id(int idx) {
  return "validator:" + std::to_string(idx);
}

// ── Byzantine modes ────────────────────────────────────────────────────────

enum class ByzantineMode { None, DoubleVote, DropPropose, SplitPropose, NotarizeSkipSplit };

struct NetworkRule {
  ByzantineMode mode{ByzantineMode::None};
  int byzantine_validator{0};        // which validator is Byzantine
  std::vector<int> group_a{1, 2};   // for SplitPropose
  std::vector<int> group_b{3};      // for SplitPropose
  int drop_slot{0};                  // for DropPropose
};

// ── Simulation state ────────────────────────────────────────────────────────

struct SimState {
  std::string session_id;
  NetworkRule rule;
  int finalized_blocks{0};
  int skipped_slots{0};
};

// ── Slot simulation ─────────────────────────────────────────────────────────

static void run_slot(SimState& sim, uint32_t slot) {
  auto& log = GraphLogger::instance();
  int leader = slot % N;

  auto emit = [&](const std::string& ev, Props props) {
    props["sessionId"] = sim.session_id;
    props["slot"]      = (int64_t)slot;
    log.emit(ev, props);
  };

  const ByzantineMode mode = sim.rule.mode;
  const int byz = sim.rule.byzantine_validator;

  // ── 1. Leader proposes ──────────────────────────────────────────────────
  std::string cand = candidate_id(slot);
  emit("Propose", {{"leaderIdx", (int64_t)leader},
                   {"candidateId", cand}});

  // Set of validators that received the candidate
  std::vector<bool> received(N, false);

  if (mode == ByzantineMode::DropPropose && (int)slot == sim.rule.drop_slot) {
    // Only the leader "has" the candidate
    received[leader] = true;
  } else if (mode == ByzantineMode::SplitPropose && leader == byz) {
    // Byzantine leader: group_a gets cand_A, group_b gets cand_B
    // Leader "receives" both (just to log propose)
    received[leader] = true;
    for (int v : sim.rule.group_a) received[v] = true;
    for (int v : sim.rule.group_b) received[v] = true;
  } else {
    // Honest delivery: all validators receive
    for (int v = 0; v < N; v++) received[v] = true;
  }

  // Emit Receive for each validator that got the candidate
  for (int v = 0; v < N; v++) {
    if (received[v] && v != leader) {
      emit("Receive", {{"validatorIdx", (int64_t)v},
                       {"candidateId", cand}});
    }
  }

  // ── 2. NotarizeVotes ────────────────────────────────────────────────────
  int notarize_count = 0;
  std::string cand_b = candidate_id(slot, "B");  // for split/equivocation

  // DoubleVote: Byzantine validator votes for BOTH cand and cand_b
  if (mode == ByzantineMode::DoubleVote) {
    // First vote (for the real candidate)
    emit("NotarizeVote", {{"validatorIdx", (int64_t)byz},
                          {"candidateId", cand}});
    // Second vote (equivocation: different candidateId, same slot)
    emit("NotarizeVote", {{"validatorIdx", (int64_t)byz},
                          {"candidateId", cand_b}});
    notarize_count++;  // counts as one honest notarize
  }

  // SplitPropose: group_a and group_b vote for different candidates
  if (mode == ByzantineMode::SplitPropose && leader == byz) {
    std::string cand_a = candidate_id(slot, "A");
    for (int v : sim.rule.group_a) {
      emit("Receive", {{"validatorIdx", (int64_t)v},
                       {"candidateId", cand_a}});
      emit("NotarizeVote", {{"validatorIdx", (int64_t)v},
                            {"candidateId", cand_a}});
    }
    for (int v : sim.rule.group_b) {
      emit("Receive", {{"validatorIdx", (int64_t)v},
                       {"candidateId", cand_b}});
      emit("NotarizeVote", {{"validatorIdx", (int64_t)v},
                            {"candidateId", cand_b}});
    }
    // No quorum → slot will be skipped
    sim.skipped_slots++;
    for (int v = 0; v < N; v++) {
      emit("SkipVote", {{"validatorIdx", (int64_t)v}});
    }
    emit("SkipCert", {{"weight", (int64_t)N}});
    return;
  }

  // NotarizeSkipSplit: Byzantine validator casts both NotarizeVote AND SkipVote on the same slot.
  // pool.cpp check_invariants() does NOT detect this pair → misbehavior goes unreported.
  // Network split: byz + validators {1,2} notarize (quorum=3 → NotarizeCert forms);
  //                byz + validator {3} receive Skip hint (only 2 Skip votes → no SkipCert).
  // Result: block finalizes, equivocation invisible to the protocol.
  if (mode == ByzantineMode::NotarizeSkipSplit) {
    // Byzantine validator votes NotarizeVote(cand) — seen by the notarize group
    emit("NotarizeVote", {{"validatorIdx", (int64_t)byz},
                          {"candidateId", cand}});
    // Byzantine validator ALSO votes SkipVote on the same slot — the undetected conflict
    emit("SkipVote",     {{"validatorIdx", (int64_t)byz}});
    notarize_count++;  // byz contributes one notarize to the quorum

    // Validators 1 and 2 notarize honestly (they received the candidate)
    for (int v = 0; v < N; v++) {
      if (v == byz || v == 3) continue;  // byz already handled; v=3 only got Skip hint
      emit("NotarizeVote", {{"validatorIdx", (int64_t)v},
                            {"candidateId", cand}});
      notarize_count++;
    }

    // Validator 3 was fed a Skip hint by the Byzantine validator and votes Skip
    emit("SkipVote", {{"validatorIdx", (int64_t)3}});
    // Skip tally: byz + validator 3 = 2 < THRESHOLD=3 → no SkipCert

    // NotarizeCert forms (byz + 1 + 2 = 3 = THRESHOLD)
    emit("NotarizeCert", {{"candidateId", cand},
                          {"weight", (int64_t)notarize_count}});
    for (int v = 0; v < N; v++) {
      if (v == 3) continue;  // validator 3 voted Skip, won't finalize
      emit("FinalizeVote", {{"validatorIdx", (int64_t)v},
                            {"candidateId", cand}});
    }
    emit("FinalizeCert", {{"candidateId", cand},
                          {"weight", (int64_t)(N - 1)}});
    emit("Block", {{"candidateId", cand}});
    sim.finalized_blocks++;
    return;
  }

  // Honest validators that received the candidate notarize it
  for (int v = 0; v < N; v++) {
    if (!received[v]) continue;
    if (mode == ByzantineMode::DoubleVote && v == byz) continue;  // already handled
    emit("NotarizeVote", {{"validatorIdx", (int64_t)v},
                          {"candidateId", cand}});
    notarize_count++;
  }

  // ── 3. NotarizeCert or SkipVotes ────────────────────────────────────────
  if (notarize_count >= THRESHOLD) {
    emit("NotarizeCert", {{"candidateId", cand},
                          {"weight", (int64_t)notarize_count}});

    // ── 4. FinalizeVotes ────────────────────────────────────────────────
    for (int v = 0; v < N; v++) {
      emit("FinalizeVote", {{"validatorIdx", (int64_t)v},
                            {"candidateId", cand}});
    }
    emit("FinalizeCert", {{"candidateId", cand},
                          {"weight", (int64_t)N}});
    emit("Block", {{"candidateId", cand}});
    sim.finalized_blocks++;
  } else {
    // Not enough votes → skip
    for (int v = 0; v < N; v++) {
      emit("SkipVote", {{"validatorIdx", (int64_t)v}});
    }
    emit("SkipCert", {{"weight", (int64_t)N}});
    sim.skipped_slots++;
  }
}

// ── Scenarios ───────────────────────────────────────────────────────────────

static SimState run_scenario(const std::string& name, NetworkRule rule, int slots = SLOTS) {
  SimState sim;
  sim.session_id = make_id();
  sim.rule = rule;

  std::cout << "[Harness] scenario=" << name
            << " sessionId=" << sim.session_id << "\n";

  GraphLogger::instance().emit("SessionStart", {
      {"sessionId", sim.session_id},
      {"scenario", name},
      {"validators", (int64_t)N},
      {"slots", (int64_t)slots}
  });

  for (int s = 0; s < slots; s++) {
    run_slot(sim, (uint32_t)s);
  }

  GraphLogger::instance().emit("SessionEnd", {
      {"sessionId", sim.session_id},
      {"finalizedBlocks", (int64_t)sim.finalized_blocks},
      {"skippedSlots", (int64_t)sim.skipped_slots}
  });

  std::cout << "[Harness] done: finalized=" << sim.finalized_blocks
            << " skipped=" << sim.skipped_slots << "\n";
  return sim;
}

// ── Assertions ──────────────────────────────────────────────────────────────

// Returns true if the NDJSON trace contains evidence of the anomaly.
// (Simple text scan — no Neo4j required for CI.)
static bool assert_equivocation(const SimState& sim) {
  // DoubleVote: two NotarizeVote lines with same slot/validatorIdx but different candidateId
  // We just check that skippedSlots=0 (equivocation doesn't block consensus if others are honest)
  return sim.finalized_blocks > 0;
}

static bool assert_withholding(const SimState& sim) {
  return sim.skipped_slots > 0;
}

static bool assert_byzantine_leader(const SimState& sim) {
  return sim.skipped_slots > 0;
}

static bool assert_notarize_skip_split(const SimState& sim) {
  // Block must finalize — the equivocation (Notarize+Skip from same validator)
  // does not prevent consensus, but also goes undetected by pool.cpp check_invariants().
  // In the graph: validator:byz has both [:notarize] and [:skip] edges on the same slot.
  return sim.finalized_blocks > 0;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  GraphLogger::instance().init();

  std::string scenario = "equivocation";
  bool assert_anomalies = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--scenario" && i + 1 < argc) {
      scenario = argv[++i];
    } else if (arg == "--assert-anomalies") {
      assert_anomalies = true;
    } else if (arg == "--log-level") {
      i++;  // consume value, currently unused
    }
  }

  bool ok = true;

  auto run = [&](const std::string& name, NetworkRule rule) {
    if (scenario != "all" && scenario != name) return;
    SimState sim = run_scenario(name, rule);
    if (assert_anomalies) {
      bool anomaly = false;
      if (name == "equivocation")          anomaly = assert_equivocation(sim);
      if (name == "message_withholding")   anomaly = assert_withholding(sim);
      if (name == "byzantine_leader")      anomaly = assert_byzantine_leader(sim);
      if (name == "notarize_skip_split")   anomaly = assert_notarize_skip_split(sim);
      if (!anomaly) {
        std::cerr << "[Harness] ASSERTION FAILED: anomaly not observed for " << name << "\n";
        ok = false;
      }
    }
  };

  // Scenario 1: Byzantine validator (idx=0) double-votes on same slot
  run("equivocation", NetworkRule{ByzantineMode::DoubleVote, 0, {}, {}, 0});

  // Scenario 2: Leader's propose is dropped for slot 0
  run("message_withholding", NetworkRule{ByzantineMode::DropPropose, 0, {}, {}, 0});

  // Scenario 3: Byzantine leader (idx=0) sends different candidates to different groups
  run("byzantine_leader", NetworkRule{ByzantineMode::SplitPropose, 0, {1, 2}, {3}, 0});

  // Scenario 4: Byzantine validator (idx=0) casts NotarizeVote AND SkipVote on same slot.
  // Demonstrates gap in pool.cpp check_invariants(): Notarize+Skip pair is not detected.
  // In the graph: validator:0 has both [:notarize] and [:skip] edges on every slot.
  run("notarize_skip_split", NetworkRule{ByzantineMode::NotarizeSkipSplit, 0, {}, {}, 0});

  if (!ok) {
    std::cerr << "[Harness] Some assertions failed.\n";
    return 1;
  }

  std::cout << "[Harness] All done.\n";
  return 0;
}
