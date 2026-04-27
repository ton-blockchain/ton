#include "auto/tl/ton_api_id_names.h"
#include "td/utils/as.h"

#include "tl-traffic-bucket.h"

namespace ton::metrics {

namespace {

// Overlay wrappers we see through to find the actual payload TL.
constexpr td::int32 kMagicOverlayQuery = -855800765;
constexpr td::int32 kMagicOverlayQueryWithExtra = -1795177495;
constexpr td::int32 kMagicOverlayMessage = 1965368352;
constexpr td::int32 kMagicOverlayMessageWithExtra = -1573772483;
constexpr td::int32 kMagicOverlayMessageExtra = 785967269;

constexpr size_t kMagicSize = 4;
constexpr size_t kOverlayIdSize = 32;

// For overlay.query / overlay.message / …WithExtra return the inner object's magic located
// right after the wrapper. Returns 0 if the wrapper is unrecognized or the data is truncated.
// For `WithExtra` variants we only peek past an overlay.messageExtra with no certificate
// (flags == 0); richer extras fall back to the wrapper magic.
td::int32 unwrap_overlay_inner_magic(td::int32 outer, td::Slice data) {
  size_t inner_off = 0;
  switch (outer) {
    case kMagicOverlayQuery:
    case kMagicOverlayMessage:
      inner_off = kMagicSize + kOverlayIdSize;
      break;
    case kMagicOverlayQueryWithExtra:
    case kMagicOverlayMessageWithExtra: {
      size_t extra_off = kMagicSize + kOverlayIdSize;
      if (data.size() < extra_off + 8) {
        return 0;
      }
      if (td::as<td::int32>(data.data() + extra_off) != kMagicOverlayMessageExtra) {
        return 0;
      }
      if (td::as<td::uint32>(data.data() + extra_off + kMagicSize) != 0) {
        return 0;  // has a certificate; parsing skipped
      }
      inner_off = extra_off + 8;
      break;
    }
    default:
      return 0;
  }
  if (data.size() < inner_off + kMagicSize) {
    return 0;
  }
  return td::as<td::int32>(data.data() + inner_off);
}

}  // namespace

void TlTrafficBucket::account(td::Slice data) {
  if (data.size() < sizeof(td::int32)) {
    unknown.bytes += data.size();
    unknown.msgs++;
    return;
  }
  td::int32 magic = td::as<td::int32>(data.data());
  if (td::int32 inner = unwrap_overlay_inner_magic(magic, data); inner != 0 && ton_api_id_name(inner)) {
    magic = inner;
  }
  account_with_magic(magic, data.size());
}

void TlTrafficBucket::account_with_magic(td::int32 magic, td::uint64 size) {
  if (ton_api_id_name(magic) == nullptr) {
    unknown.bytes += size;
    unknown.msgs++;
    return;
  }
  auto &c = by_magic[magic];
  c.bytes += size;
  c.msgs++;
}

void render_tl_bucket(MetricSet &set, const std::string &base, const std::string &bucket_label_value,
                      const TlTrafficBucket &bucket, std::optional<std::string> bytes_help,
                      std::optional<std::string> messages_help, std::string bucket_label_key) {
  auto build = [&](std::string name_suffix, std::optional<std::string> help, auto extract_value,
                   td::uint64 unknown_value) {
    MetricFamily fam{.name = base + name_suffix, .type = "counter", .help = std::move(help), .metrics = {}};
    auto push = [&](std::string tl_name, td::uint64 value) {
      fam.metrics.push_back(Metric{
          .suffix = "",
          .label_set = LabelSet{.labels = {{bucket_label_key, bucket_label_value}, {"tl", std::move(tl_name)}}},
          .samples = {Sample{.label_set = {}, .value = static_cast<double>(value)}},
      });
    };
    for (auto &[magic, counter] : bucket.by_magic) {
      push(ton_api_id_name(magic), extract_value(counter));  // non-null by construction
    }
    push("unknown", unknown_value);
    set.families.push_back(std::move(fam));
  };
  build(
      "_bytes_by_tl_total", std::move(bytes_help), [](const TlTrafficBucket::Counter &c) { return c.bytes; },
      bucket.unknown.bytes);
  build(
      "_messages_by_tl_total", std::move(messages_help), [](const TlTrafficBucket::Counter &c) { return c.msgs; },
      bucket.unknown.msgs);
}

}  // namespace ton::metrics
