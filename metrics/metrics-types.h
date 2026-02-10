#pragma once
#include <optional>
#include <string>
#include <vector>

namespace ton::metrics {
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

  static MetricFamily make_scalar(std::string name, std::string type, double value, std::optional<std::string> help = std::nullopt);
};

struct MetricSet {
  std::vector<MetricFamily> families;

  [[nodiscard]] MetricSet join(MetricSet other) &&;
  [[nodiscard]] std::string render() &&;
  [[nodiscard]] MetricSet wrap(const std::string &prefix) &&;
  [[nodiscard]] MetricSet label(const LabelSet &extension) &&;
};

struct Exposition {
  MetricSet main_set;

  [[nodiscard]] std::string render() &&;
};

std::string concat_names(std::string name1, std::string name2);
}