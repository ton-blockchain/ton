#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <variant>

namespace simulation {

// Property value for NDJSON events
using PropVal = std::variant<std::string, int64_t, double, bool>;
using Props = std::map<std::string, PropVal>;

// Fire-and-forget NDJSON logger.
// One JSON object per line → simulation/trace.ndjson
// Controlled by GRAPH_LOGGING_ENABLED env var (default: off).
class GraphLogger {
 public:
  static GraphLogger& instance();

  // Call once before emitting. Reads GRAPH_LOGGING_ENABLED / GRAPH_LOG_FILE.
  void init();

  // Emit one NDJSON line: {"event":"<type>", ...props, "tsMs":<ms>}
  void emit(const std::string& event_type, const Props& props = {});

  bool is_enabled() const { return enabled_; }

 private:
  GraphLogger() = default;

  bool enabled_{false};
  std::string path_;
  FILE* file_{nullptr};
  std::mutex mu_;

  static std::string to_json_value(const PropVal& v);
};

}  // namespace simulation
