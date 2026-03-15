/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include "ton/ton-types.h"  // td::Bits256 = td::BitArray<256>

namespace ton::simulation {

// A single graph node + edge to its parent.
// All optional fields are omitted from the NDJSON output when absent.
struct GraphEntry {
  // --- identity ---
  std::string node_id;        // UUID v4 (hex, 32 chars without dashes)
  std::string session_id;     // hex(ValidatorSessionId/Bits256)
  std::string type;           // "Candidate" | "Validator" | "Cert" | "Block" | "SkipEvent"
  int64_t ts_ms{0};           // Clocks::system() * 1000

  // --- protocol fields ---
  uint32_t slot{0};
  std::optional<std::string> candidate_id;   // hex(CandidateId.hash), 64 chars
  std::optional<int64_t> validator_idx;      // PeerValidatorId::value()
  std::optional<std::string> vote_type;      // "notarize" | "finalize" | "skip"
  std::optional<std::string> outcome;        // "reject" | "misbehavior" — only on failure

  // --- edge to parent node ---
  std::optional<std::string> parent_node_id;
  std::optional<std::string> edge_type;      // see CYPHER_QUERIES.md#edge-types
  uint32_t edge_slot{0};                     // slot carried on the edge
  std::optional<int64_t> edge_weight;        // for :cert / :accepted
};

// Formats a Bits256 value as a 64-character lowercase hex string.
std::string bits256_to_hex(const td::Bits256& b);

// Generates a random UUID v4 formatted as 32 lowercase hex chars (no dashes).
std::string make_node_id();

// Thread-safe, fire-and-forget NDJSON emitter.
// Each line written to the output file is a complete, self-contained JSON object.
// relay.mjs tails the file and forwards entries to Neo4j Aura via AuraGraphReporter.
class GraphLogger {
 public:
  // Returns the process-wide singleton.
  static GraphLogger& instance();

  // Opens the output file. Must be called once before the first emit().
  // session_id is stored and stamped into each entry if the entry's own session_id is empty.
  // Controlled by env var GRAPH_LOGGING_ENABLED (default: enabled).
  // Output path overridable via env var GRAPH_LOG_FILE (default: simulation/trace.ndjson).
  void init(std::string session_id, std::string output_path = "");

  // Serialises entry to a single NDJSON line and appends it to the file.
  // No-op when is_enabled() == false or init() has not been called.
  void emit(const GraphEntry& entry);

  bool is_enabled() const {
    return enabled_;
  }

 private:
  GraphLogger() = default;

  std::mutex mu_;
  std::ofstream file_;
  std::string default_session_id_;
  bool enabled_{true};
  bool initialised_{false};
};

}  // namespace ton::simulation
