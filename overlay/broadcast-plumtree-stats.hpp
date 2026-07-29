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
#pragma once

#include <array>
#include <map>
#include <tuple>
#include <vector>

#include "auto/tl/ton_api.h"
#include "crypto/common/bitstring.h"
#include "td/utils/Time.h"
#include "td/utils/common.h"

namespace ton {

namespace overlay {

constexpr std::size_t PLUMTREE_STATS_STORE_LIMIT = 100;
constexpr td::uint32 PLUMTREE_STATS_USEFUL_AVG_MS_LIMIT = 10000;
constexpr td::uint32 PLUMTREE_STATS_USEFUL_MAX_MS_LIMIT = 30000;
constexpr td::uint32 PLUMTREE_STATS_USEFUL_P99_MS_LIMIT = 3000;
constexpr td::uint32 PLUMTREE_STATS_REPAIR_PERCENT_SCALE = 255;
constexpr double PLUMTREE_STATS_SNAPSHOT_DELAY = 1.0 / 12;  // 5 minutes
constexpr double PLUMTREE_STATS_SEND_JITTER = 1.0 / 12;     // 5 minutes

struct PlumtreeLatencyStats {
  void note(double timestamp);
  td::uint32 avg_ms(td::uint32 limit) const;
  td::uint32 max_ms() const;
  td::uint32 p99_ms() const;
  void reset();

 private:
  td::uint64 count = 0;
  td::uint64 sum_ms = 0;
  td::uint32 max_ms_value = 0;
  std::array<td::uint64, PLUMTREE_STATS_USEFUL_P99_MS_LIMIT> buckets{};
};

struct PlumtreeRepairStats {
  void note(bool via_repair);
  td::uint32 percent_scaled() const;
  void reset();

 private:
  td::uint64 total = 0;
  td::uint64 repair = 0;
};

struct PlumtreeStats {
  void note_delivered_broadcast();
  void note_fec_parts_collected(td::uint32 parts, td::uint32 parts_limit);
  void note_useful_delivery(double timestamp, bool is_slot0);
  void note_part_received(bool is_slot0, bool via_repair);
  std::tuple<td::int32, td::int32, td::int32, td::int32, td::int32> snapshot_and_reset(td::uint32 parts_limit);

 private:
  td::uint64 delivered_bcsts = 0;
  std::vector<td::uint64> fec_parts_buckets;
  std::array<PlumtreeLatencyStats, 2> useful_latency_;
  std::array<PlumtreeRepairStats, 2> repair_stats_;

  td::int32 parts_in_p99_bcsts() const;
  void reset_fec_parts(td::uint32 parts_limit);
  td::int32 useful_avg_ms() const;
  td::int32 useful_max_ms() const;
  td::int32 useful_p99_ms() const;
  void reset_useful();
};

struct PlumtreeStatsEpochState {
  td::int64 epoch = -1;
  td::uint32 rotating_slot = 0;
  td::Timestamp snapshot_at = td::Timestamp::never();
  td::Timestamp send_at = td::Timestamp::never();
  td::Timestamp next_epoch_at = td::Timestamp::never();
  bool snapshot_done = false;
  bool send_done = false;
  bool skipped = false;
  // Keyed by reporting node id.
  std::map<td::Bits256, tl_object_ptr<ton_api::overlay_plumtreeStatsRecord>> store;
};

}  // namespace overlay

}  // namespace ton
