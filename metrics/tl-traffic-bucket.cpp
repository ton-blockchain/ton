#include "auto/tl/ton_api_id_names.h"
#include "td/utils/as.h"

#include "tl-traffic-bucket.h"

namespace ton::metrics {

void TlTrafficBucket::account(td::Slice data) {
  if (data.size() < sizeof(td::int32)) {
    unknown_bytes += data.size();
    unknown_messages++;
    return;
  }
  account_with_magic(td::as<td::int32>(data.data()), data.size());
}

void TlTrafficBucket::account_with_magic(td::int32 magic, td::uint64 size) {
  if (ton_api_id_name(magic) == nullptr) {
    unknown_bytes += size;
    unknown_messages++;
    return;
  }
  bytes[magic] += size;
  messages[magic]++;
}

void render_tl_bucket(MetricSet &set, const std::string &base, const std::string &bucket_label_value,
                      const TlTrafficBucket &bucket, std::optional<std::string> bytes_help,
                      std::optional<std::string> messages_help, std::string bucket_label_key) {
  auto build = [&](std::string name_suffix, std::optional<std::string> help,
                   const std::map<td::int32, td::uint64> &counts, td::uint64 unknown_value) {
    MetricFamily fam{.name = base + name_suffix, .type = "counter", .help = std::move(help), .metrics = {}};
    auto push = [&](std::string tl_name, td::uint64 value) {
      fam.metrics.push_back(Metric{
          .suffix = "",
          .label_set = LabelSet{.labels = {{bucket_label_key, bucket_label_value}, {"tl", std::move(tl_name)}}},
          .samples = {Sample{.label_set = {}, .value = static_cast<double>(value)}},
      });
    };
    for (auto &[magic, value] : counts) {
      const char *name = ton_api_id_name(magic);
      // Should never be null because `account` filters unknowns into `unknown_*`.
      push(name == nullptr ? "unknown" : name, value);
    }
    push("unknown", unknown_value);
    set.families.push_back(std::move(fam));
  };
  build("_bytes_by_tl_total", std::move(bytes_help), bucket.bytes, bucket.unknown_bytes);
  build("_messages_by_tl_total", std::move(messages_help), bucket.messages, bucket.unknown_messages);
}

}  // namespace ton::metrics
