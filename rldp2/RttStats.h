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

#include "td/utils/Time.h"
#include "td/utils/TimedStat.h"

namespace ton {
namespace rldp2 {
struct RttStats {
  void on_rtt_sample(double rtt_sample, double ack_delay, td::Timestamp now);

  double min_rtt = -1;
  double windowed_min_rtt = -1;
  double last_rtt = -1;
  double smoothed_rtt = -1;
  double rtt_var = -1;
  td::uint32 rtt_round{0};

 private:
  td::Timestamp rtt_round_at;
  td::TimedStat<td::MinStat<double>> windowed_min_rtt_stat{5, 0};
};
}  // namespace rldp2
}  // namespace ton
