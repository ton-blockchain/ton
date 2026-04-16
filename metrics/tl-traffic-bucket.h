#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "td/utils/Slice.h"
#include "td/utils/int_types.h"

#include "metrics-types.h"

namespace ton::metrics {

// Per-TL-constructor traffic counter. Use one instance per (layer, kind) pair (e.g. rldp send/query/...).
//
// Magics that don't match a known TL constructor — including wire garbage and short payloads —
// are funnelled into a single `unknown` bucket so cardinality stays bounded by the schema size.
struct TlTrafficBucket {
  // Indexed by TL constructor id. Only valid (schema-known) ids are inserted as keys here.
  std::map<td::int32, td::uint64> bytes;
  std::map<td::int32, td::uint64> messages;

  // Single "unknown" bucket for wire payloads whose first 4 bytes don't match a known constructor.
  td::uint64 unknown_bytes = 0;
  td::uint64 unknown_messages = 0;

  // Increment counters for `data` (a serialized TL object). `data` may be empty / shorter than 4.
  void account(td::Slice data);

  // Same accounting as account() but with the magic already extracted.  Use this when the
  // accounting happens in a different actor from the one that owns the BufferSlice — pass
  // (magic, size) through send_closure rather than copying the whole payload.  Pass magic == 0
  // (or any other unknown value) to force the unknown bucket.
  void account_with_magic(td::int32 magic, td::uint64 size);
};

// Render a TlTrafficBucket as two MetricFamily entries (`<base>_bytes_by_tl_total`,
// `<base>_messages_by_tl_total`) with labels {<bucket_label_key>=<bucket_label_value>,
// tl=<schema_name|"unknown">}.
//
// `base` is something like "app_send"; the "_bytes_by_tl_total" / "_messages_by_tl_total"
// suffixes are appended. Use `bucket_label_key` = "kind" for per-layer kinds, or "type" for
// overlay-type partitioning, etc.
[[nodiscard]] std::vector<MetricFamily> render_tl_bucket(const std::string &base, const std::string &bucket_label_value,
                                                         const TlTrafficBucket &bucket,
                                                         std::optional<std::string> bytes_help = std::nullopt,
                                                         std::optional<std::string> messages_help = std::nullopt,
                                                         std::string bucket_label_key = "kind");

}  // namespace ton::metrics
