/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/

#include <algorithm>
#include <cmath>
#include <limits>

#include "td/utils/port/Clocks.h"

#include "broadcast-plumtree-stats.hpp"

namespace ton {

namespace overlay {

namespace {

td::int32 stats_to_int32(td::uint64 value) {
  auto max = static_cast<td::uint64>(std::numeric_limits<td::int32>::max());
  return value > max ? std::numeric_limits<td::int32>::max() : static_cast<td::int32>(value);
}

td::int32 encode_stats_pair(td::uint32 slot0_value, td::uint32 other_slots_value, td::uint32 limit) {
  slot0_value = std::min(slot0_value, limit);
  other_slots_value = std::min(other_slots_value, limit);
  // Values are in [0, limit], so base is limit + 1 to encode the limit itself without ambiguity.
  auto base = static_cast<td::uint64>(limit) + 1;
  return stats_to_int32(static_cast<td::uint64>(slot0_value) * base + other_slots_value);
}

td::int32 encode_useful_max_ms(td::uint32 max_ms, td::uint32 slot0_repair_percent,
                               td::uint32 other_slots_repair_percent) {
  max_ms = std::min(max_ms, PLUMTREE_STATS_USEFUL_MAX_MS_LIMIT);
  slot0_repair_percent = std::min(slot0_repair_percent, PLUMTREE_STATS_REPAIR_PERCENT_SCALE);
  other_slots_repair_percent = std::min(other_slots_repair_percent, PLUMTREE_STATS_REPAIR_PERCENT_SCALE);
  return stats_to_int32((static_cast<td::uint64>(max_ms) << 16) | (slot0_repair_percent << 8) |
                        other_slots_repair_percent);
}

std::size_t stats_index(bool is_slot0) {
  return is_slot0 ? 0 : 1;
}

}  // namespace

void PlumtreeLatencyStats::note(double timestamp) {
  auto duration_ms = (td::Clocks::system() - timestamp) * 1000.0;
  if (!std::isfinite(duration_ms)) {
    return;
  }
  if (duration_ms < 0.0) {
    duration_ms = 0.0;
  }
  auto max = static_cast<double>(std::numeric_limits<td::uint32>::max());
  auto value = duration_ms > max ? std::numeric_limits<td::uint32>::max() : static_cast<td::uint32>(duration_ms + 0.5);
  ++count;
  sum_ms += value;
  max_ms_value = std::max(max_ms_value, value);
  auto bucket = std::min<std::size_t>(value, buckets.size() - 1);
  ++buckets[bucket];
}

td::uint32 PlumtreeLatencyStats::avg_ms(td::uint32 limit) const {
  if (count == 0) {
    return 0;
  }
  auto value = sum_ms / count;
  return value > limit ? limit : static_cast<td::uint32>(value);
}

td::uint32 PlumtreeLatencyStats::max_ms() const {
  return max_ms_value;
}

td::uint32 PlumtreeLatencyStats::p99_ms() const {
  if (count == 0) {
    return 0;
  }
  auto target = count - count / 100;
  td::uint64 seen = 0;
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    seen += buckets[i];
    if (seen >= target) {
      return static_cast<td::uint32>(i + 1);
    }
  }
  return PLUMTREE_STATS_USEFUL_P99_MS_LIMIT;
}

void PlumtreeLatencyStats::reset() {
  count = 0;
  sum_ms = 0;
  max_ms_value = 0;
  buckets.fill(0);
}

void PlumtreeRepairStats::note(bool via_repair) {
  ++total;
  if (via_repair) {
    ++repair;
  }
}

td::uint32 PlumtreeRepairStats::percent_scaled() const {
  if (total == 0) {
    return 0;
  }
  return static_cast<td::uint32>((repair * PLUMTREE_STATS_REPAIR_PERCENT_SCALE + total / 2) / total);
}

void PlumtreeRepairStats::reset() {
  total = 0;
  repair = 0;
}

void PlumtreeStats::note_delivered_broadcast() {
  ++delivered_bcsts;
}

void PlumtreeStats::note_fec_parts_collected(td::uint32 parts, td::uint32 parts_limit) {
  if (parts_limit == 0) {
    return;
  }
  auto bucket_count = static_cast<std::size_t>(parts_limit) + 1;
  if (fec_parts_buckets.size() != bucket_count) {
    fec_parts_buckets.assign(bucket_count, 0);
  }
  auto bucket = std::min<td::uint32>(parts, parts_limit);
  ++fec_parts_buckets[bucket];
}

void PlumtreeStats::note_useful_delivery(double timestamp, bool is_slot0) {
  useful_latency_[stats_index(is_slot0)].note(timestamp);
}

void PlumtreeStats::note_part_received(bool is_slot0, bool via_repair) {
  repair_stats_[stats_index(is_slot0)].note(via_repair);
}

std::tuple<td::int32, td::int32, td::int32, td::int32, td::int32> PlumtreeStats::snapshot_and_reset(
    td::uint32 parts_limit) {
  auto result = std::make_tuple(stats_to_int32(delivered_bcsts), parts_in_p99_bcsts(), useful_avg_ms(), useful_max_ms(),
                                useful_p99_ms());
  delivered_bcsts = 0;
  reset_fec_parts(parts_limit);
  reset_useful();
  return result;
}

td::int32 PlumtreeStats::parts_in_p99_bcsts() const {
  td::uint64 total = 0;
  for (auto count : fec_parts_buckets) {
    total += count;
  }
  if (total == 0) {
    return 0;
  }
  // Lower-tail cutoff: 99% of observed FEC broadcasts collected at least this many parts.
  auto target = total / 100 + 1;
  td::uint64 seen = 0;
  for (std::size_t i = 0; i < fec_parts_buckets.size(); ++i) {
    seen += fec_parts_buckets[i];
    if (seen >= target) {
      return stats_to_int32(i);
    }
  }
  return stats_to_int32(fec_parts_buckets.size() - 1);
}

void PlumtreeStats::reset_fec_parts(td::uint32 parts_limit) {
  if (parts_limit == 0) {
    fec_parts_buckets.clear();
    return;
  }
  auto bucket_count = static_cast<std::size_t>(parts_limit) + 1;
  if (fec_parts_buckets.size() != bucket_count) {
    fec_parts_buckets.assign(bucket_count, 0);
    return;
  }
  std::fill(fec_parts_buckets.begin(), fec_parts_buckets.end(), 0);
}

td::int32 PlumtreeStats::useful_avg_ms() const {
  return encode_stats_pair(useful_latency_[0].avg_ms(PLUMTREE_STATS_USEFUL_AVG_MS_LIMIT),
                           useful_latency_[1].avg_ms(PLUMTREE_STATS_USEFUL_AVG_MS_LIMIT),
                           PLUMTREE_STATS_USEFUL_AVG_MS_LIMIT);
}

td::int32 PlumtreeStats::useful_max_ms() const {
  return encode_useful_max_ms(std::max(useful_latency_[0].max_ms(), useful_latency_[1].max_ms()),
                              repair_stats_[0].percent_scaled(), repair_stats_[1].percent_scaled());
}

td::int32 PlumtreeStats::useful_p99_ms() const {
  return encode_stats_pair(useful_latency_[0].p99_ms(), useful_latency_[1].p99_ms(),
                           PLUMTREE_STATS_USEFUL_P99_MS_LIMIT);
}

void PlumtreeStats::reset_useful() {
  for (auto &latency : useful_latency_) {
    latency.reset();
  }
  for (auto &repair : repair_stats_) {
    repair.reset();
  }
}

}  // namespace overlay

}  // namespace ton
