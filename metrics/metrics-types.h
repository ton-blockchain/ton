#pragma once
#include <atomic>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "td/utils/int_types.h"

namespace ton::metrics {

// Paired (bytes, messages) counter used by subsystems that aggregate across actor threads.
// One record() call does two relaxed atomic increments and is cheap in hot paths.
struct AtomicKindCounter {
  std::atomic<td::uint64> bytes{0};
  std::atomic<td::uint64> msgs{0};

  void record(td::uint64 size) {
    bytes.fetch_add(size, std::memory_order_relaxed);
    msgs.fetch_add(1, std::memory_order_relaxed);
  }
  td::uint64 bytes_load() const {
    return bytes.load(std::memory_order_relaxed);
  }
  td::uint64 msgs_load() const {
    return msgs.load(std::memory_order_relaxed);
  }
};
struct Label {
  std::string key, val;

  [[nodiscard]] std::string render() &&;
};

struct LabelSet {
  std::vector<Label> labels;

  [[nodiscard]] LabelSet join(LabelSet other) &&;
  [[nodiscard]] std::string render() &&;
};

struct Sample {
  LabelSet label_set;
  double value = 0;

  [[nodiscard]] std::string render(const std::string &metric_name, LabelSet metric_label_set) &&;
};

struct Metric {
  std::string suffix;
  LabelSet label_set;
  std::vector<Sample> samples;

  [[nodiscard]] std::string render(std::string family_name) &&;
  [[nodiscard]] Metric label(LabelSet extension) &&;
};

struct MetricFamily {
  std::string name;
  std::optional<std::string> type, help;
  std::vector<Metric> metrics;

  [[nodiscard]] std::string render() &&;
  [[nodiscard]] MetricFamily wrap(std::string prefix) &&;
  [[nodiscard]] MetricFamily label(const LabelSet &extension) &&;

  static MetricFamily make_scalar(std::string name, std::string type, double value,
                                  std::optional<std::string> help = std::nullopt);

  // Counter/gauge with one label dimension. Each entry becomes a Metric with a single Sample,
  // labeled {label_key = entry.first}. Replaces a lot of identical hand-rolled lambdas in
  // collector implementations.
  static MetricFamily make_labeled_scalar(std::string name, std::string type, std::string label_key,
                                          std::vector<std::pair<std::string, td::uint64>> entries,
                                          std::optional<std::string> help = std::nullopt);
};

struct MetricSet {
  std::vector<MetricFamily> families;

  [[nodiscard]] MetricSet join(MetricSet other) &&;
  [[nodiscard]] std::string render() &&;
  [[nodiscard]] MetricSet wrap(const std::string &prefix) &&;
  [[nodiscard]] MetricSet label(const LabelSet &extension) &&;

  // Sugar so that a collector can do `set.push_scalar(name, type, v, help)` instead of
  // `set.families.push_back(MetricFamily::make_scalar(name, type, v, help))`.  Any numeric
  // value type flows through — counters are typically uint64/int64/size_t, RTT gauges double.
  template <typename V, typename = std::enable_if_t<std::is_arithmetic_v<V>>>
  void push_scalar(std::string name, std::string type, V value, std::optional<std::string> help = std::nullopt) {
    families.push_back(
        MetricFamily::make_scalar(std::move(name), std::move(type), static_cast<double>(value), std::move(help)));
  }
  void push_labeled_scalar(std::string name, std::string type, std::string label_key,
                           std::vector<std::pair<std::string, td::uint64>> entries,
                           std::optional<std::string> help = std::nullopt);
};

struct Exposition {
  MetricSet main_set;

  [[nodiscard]] std::string render() &&;
};
}  // namespace ton::metrics
