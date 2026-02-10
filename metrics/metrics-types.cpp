#include "td/utils/logging.h"


#include "metrics-types.h"

namespace ton::metrics {

std::string concat_names(std::string name1, std::string name2) {
  if (!name1.empty() && !name2.empty())
    return std::move(name1) + '_' + std::move(name2);
  return std::move(name1) + std::move(name2);
}

std::string Label::render() && {
  auto k = std::move(key), v = std::move(val);
  return PSTRING() << k << '=' << '"' << v << '"';
}

LabelSet LabelSet::join(LabelSet other) && {
  auto all_labels = std::move(labels);
  all_labels.reserve(all_labels.size() + other.labels.size());
  for (auto &l : other.labels)
    all_labels.push_back(std::move(l));
  other.labels.resize(0);
  return {.labels = std::move(all_labels)};
}

std::string LabelSet::render() && {
  if (labels.empty())
    return "";
  std::string result = "{";
  for (auto &l : labels) {
    if (result.size() != 1)
      result += ',';
    result += std::move(l).render();
  }
  labels = {};
  result += '}';
  return result;
}

std::string Sample::render(const std::string &metric_name, LabelSet metric_label_set) && {
  return PSTRING() << metric_name << std::move(metric_label_set).join(std::move(label_set)).render() << ' ' << value << '\n';
}

std::string Metric::render(std::string family_name) && {
  auto whole_name = concat_names(std::move(family_name), std::move(suffix));
  std::string result;
  for (auto &s : samples)
    result += std::move(s).render(whole_name, label_set);
  label_set = {};
  samples = {};
  return result;
}

Metric Metric::label(LabelSet extension) &&{
  auto new_label_set = std::move(label_set);
  for (auto &l : extension.labels)
    new_label_set.labels.push_back(std::move(l));
  return {.suffix = std::move(suffix), .label_set = std::move(new_label_set), .samples = std::move(samples)};
}

std::string MetricFamily::render(std::string prefix) && {
  auto whole_name = concat_names(std::move(prefix), std::move(name));
  std::string result;
  if (help.has_value())
    result += PSTRING() << "# HELP " << whole_name << ' ' << *help << '\n';
  if (type.has_value())
    result += PSTRING() << "# TYPE " << whole_name << ' ' << *type << '\n';
  for (auto &m : metrics)
    result += std::move(m).render(whole_name);
  metrics = {};
  return result;
}

MetricFamily MetricFamily::wrap(std::string prefix) && {
  return {.name = concat_names(std::move(prefix), std::move(name)), .type = std::move(type), .help = std::move(help), .metrics = std::move(metrics)};
}

MetricFamily MetricFamily::label(const LabelSet &extension) && {
  auto new_metrics = std::move(metrics);
  for (auto &m : new_metrics)
    m = std::move(m).label(extension);
  return {.name = std::move(name), .type = std::move(type), .help = std::move(help), .metrics = std::move(new_metrics)};
}

MetricFamily MetricFamily::make_scalar(std::string name, std::string type, double value, std::optional<std::string> help) {
  return MetricFamily{
    .name = name,
    .type = type,
    .help = help,
    .metrics = {Metric{
      .suffix = "",
      .label_set = {},
      .samples = {Sample{
        .label_set = {},
        .value = value
      }}
    }}
  };
}

MetricSet MetricSet::join(MetricSet other) && {
  auto all_families = std::move(families);
  all_families.reserve(all_families.size() + other.families.size());
  for (auto &f : other.families)
    all_families.push_back(std::move(f));
  other.families.resize(0);
  return {.families = std::move(all_families)};
}

std::string MetricSet::render(const std::string &prefix) && {
  std::string result;
  for (auto &f : families)
    result += std::move(f).render(prefix);
  families = {};
  return result;
}

MetricSet MetricSet::wrap(const std::string &prefix) && {
  auto new_families = std::move(families);
  for (auto &f : new_families)
    f = std::move(f).wrap(prefix);
  return {.families = std::move(new_families)};
}

MetricSet MetricSet::label(const LabelSet &extension) && {
  auto new_families = std::move(families);
  for (auto &f : new_families)
    f = std::move(f).label(extension);
  return {.families = std::move(new_families)};
}

std::string Exposition::render() && {
  std::string result = std::move(whole_set).render(prefix);
  result += "# EOF\n";
  return result;
}

}