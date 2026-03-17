#include "GraphLogger.h"

#include <chrono>
#include <cstdlib>
#include <sstream>

namespace simulation {

// ── Singleton ──────────────────────────────────────────────────────────────

GraphLogger& GraphLogger::instance() {
  static GraphLogger inst;
  return inst;
}

// ── Init ───────────────────────────────────────────────────────────────────

void GraphLogger::init() {
  const char* enabled_env = std::getenv("GRAPH_LOGGING_ENABLED");
  if (!enabled_env || std::string(enabled_env) == "0" || std::string(enabled_env) == "false") {
    enabled_ = false;
    return;
  }

  const char* path_env = std::getenv("GRAPH_LOG_FILE");
  path_ = path_env ? path_env : "simulation/trace.ndjson";

  file_ = std::fopen(path_.c_str(), "a");
  if (!file_) {
    fprintf(stderr, "[GraphLogger] Cannot open log file: %s\n", path_.c_str());
    enabled_ = false;
    return;
  }

  enabled_ = true;
  fprintf(stderr, "[GraphLogger] logging → %s\n", path_.c_str());
}

// ── Emit ───────────────────────────────────────────────────────────────────

void GraphLogger::emit(const std::string& event_type, const Props& props) {
  if (!enabled_) return;

  int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

  std::ostringstream ss;
  ss << "{\"event\":\"" << event_type << "\"";
  for (const auto& [k, v] : props) {
    ss << ",\"" << k << "\":" << to_json_value(v);
  }
  ss << ",\"tsMs\":" << ts_ms << "}\n";

  std::string line = ss.str();
  std::lock_guard<std::mutex> lk(mu_);
  std::fwrite(line.c_str(), 1, line.size(), file_);
  std::fflush(file_);
}

// ── JSON helpers ───────────────────────────────────────────────────────────

std::string GraphLogger::to_json_value(const PropVal& v) {
  return std::visit(
      [](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) {
          // Minimal escaping: backslash and double-quote
          std::string out = "\"";
          for (char c : x) {
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else out += c;
          }
          out += '"';
          return out;
        } else if constexpr (std::is_same_v<T, bool>) {
          return x ? "true" : "false";
        } else {
          return std::to_string(x);
        }
      },
      v);
}

}  // namespace simulation
