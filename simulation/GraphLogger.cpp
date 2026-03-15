/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "GraphLogger.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
static void secure_random_bytes(uint8_t* buf, size_t len) {
  BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf),
                  static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}
#else
#  include <fstream>
static void secure_random_bytes(uint8_t* buf, size_t len) {
  std::ifstream urandom("/dev/urandom", std::ios::binary);
  urandom.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(len));
}
#endif

namespace ton::simulation {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string bytes_to_hex(const uint8_t* data, size_t len) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += hex[(data[i] >> 4) & 0xf];
    out += hex[data[i] & 0xf];
  }
  return out;
}

std::string bits256_to_hex(const std::array<uint8_t, 32>& b) {
  return bytes_to_hex(b.data(), b.size());
}

std::string make_node_id() {
  uint8_t buf[16];
  secure_random_bytes(buf, sizeof(buf));
  // version 4, variant bits
  buf[6] = static_cast<uint8_t>((buf[6] & 0x0f) | 0x40);
  buf[8] = static_cast<uint8_t>((buf[8] & 0x3f) | 0x80);
  return bytes_to_hex(buf, sizeof(buf));
}

namespace {

// Minimal JSON-string escaper: replaces " and \ and control chars.
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

// Appends   ,"key":"value"   to buf.
void append_str(std::string& buf, const char* key, const std::string& val) {
  buf += ",\"";
  buf += key;
  buf += "\":\"";
  buf += json_escape(val);
  buf += '"';
}

// Appends   ,"key":number   to buf.
void append_int(std::string& buf, const char* key, int64_t val) {
  buf += ",\"";
  buf += key;
  buf += "\":";
  buf += std::to_string(val);
}

std::string serialise(const GraphEntry& e) {
  // Start with required fields; all optional fields appended only when present.
  std::string buf;
  buf.reserve(256);

  buf += "{\"nodeId\":\"";
  buf += json_escape(e.node_id);
  buf += '"';

  append_str(buf, "sessionId", e.session_id);
  append_str(buf, "type", e.type);
  append_int(buf, "tsMs", e.ts_ms);
  append_int(buf, "slot", static_cast<int64_t>(e.slot));

  if (e.candidate_id) {
    append_str(buf, "candidateId", *e.candidate_id);
  }
  if (e.validator_idx) {
    append_int(buf, "validatorIdx", *e.validator_idx);
  }
  if (e.vote_type) {
    append_str(buf, "voteType", *e.vote_type);
  }
  if (e.outcome) {
    append_str(buf, "outcome", *e.outcome);
  }
  if (e.parent_node_id) {
    append_str(buf, "parentNodeId", *e.parent_node_id);
  }
  if (e.edge_type) {
    append_str(buf, "edgeType", *e.edge_type);
    append_int(buf, "edgeSlot", static_cast<int64_t>(e.edge_slot));
    if (e.edge_weight) {
      append_int(buf, "edgeWeight", *e.edge_weight);
    }
  }

  buf += '}';
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// GraphLogger
// ---------------------------------------------------------------------------

GraphLogger& GraphLogger::instance() {
  static GraphLogger inst;
  return inst;
}

void GraphLogger::init(std::string session_id, std::string output_path) {
  const char* env_enabled = std::getenv("GRAPH_LOGGING_ENABLED");
  if (env_enabled && std::string(env_enabled) == "0") {
    enabled_ = false;
    return;
  }

  if (output_path.empty()) {
    const char* env_path = std::getenv("GRAPH_LOG_FILE");
    output_path = (env_path && *env_path) ? env_path : "simulation/trace.ndjson";
  }

  std::lock_guard<std::mutex> lock(mu_);
  default_session_id_ = std::move(session_id);
  file_.open(output_path, std::ios::app);
  initialised_ = file_.is_open();
}

void GraphLogger::emit(const GraphEntry& entry) {
  if (!enabled_ || !initialised_) {
    return;
  }

  // Fill in session_id from default if the caller left it blank.
  const std::string& sid = entry.session_id.empty() ? default_session_id_ : entry.session_id;

  GraphEntry resolved = entry;
  if (resolved.session_id.empty()) {
    resolved.session_id = sid;
  }

  std::string line = serialise(resolved);
  line += '\n';

  std::lock_guard<std::mutex> lock(mu_);
  file_.write(line.data(), static_cast<std::streamsize>(line.size()));
  file_.flush();
}

}  // namespace ton::simulation
