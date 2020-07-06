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
#include "td/utils/int_types.h"
#include "td/utils/Time.h"

namespace ton {
namespace rldp2 {
struct RttStats;
struct BdwStats;

struct Bbr {
 public:
  void step(const RttStats &rtt_stats, const BdwStats &bdw_stats, td::uint64 in_flight, td::Timestamp now);
  double get_rate() const;

  td::uint32 get_window_size() const;

 private:
  double bdw_peak_{-1};
  td::uint32 bdw_peak_at_round{0};
  td::uint32 probe_bdw_cycle_{0};
  td::Timestamp probe_bdw_cycle_at_;
  double rtt_min_{0};
  double bdw_max_{0};
  enum class State { Start, Drain, ProbeRtt, ProbeBdw } state_ = State::Start;
};

}  // namespace rldp2
}  // namespace ton

