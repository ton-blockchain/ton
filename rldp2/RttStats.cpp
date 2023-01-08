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

#include "RttStats.h"
#include "rldp.hpp"
#include <cmath>

namespace ton {
namespace rldp2 {
void RttStats::on_rtt_sample(double rtt_sample, double ack_delay, td::Timestamp now) {
  if (rtt_sample < 0.001 || rtt_sample > 10) {
    VLOG(RLDP_INFO) << "Suspicious rtt sample " << rtt_sample;
    return;
  }
  if (ack_delay < -1e-9 || ack_delay > 10) {
    VLOG(RLDP_INFO) << "Suspicious ack_delay " << ack_delay;
    return;
  }
  rtt_sample = td::max(0.01, rtt_sample);

  last_rtt = rtt_sample;

  windowed_min_rtt_stat.add_event(rtt_sample, now.at());
  auto windowed_min_rtt_sample = windowed_min_rtt_stat.get_stat(now.at()).get_stat();
  if (windowed_min_rtt_sample) {
    windowed_min_rtt = windowed_min_rtt_sample.value();
  }

  if (smoothed_rtt < 0) {
    // ignore ack_delay just because
    min_rtt = last_rtt;
    smoothed_rtt = last_rtt;
    rtt_var = last_rtt / 2;
  } else {
    if (rtt_sample < min_rtt) {
      min_rtt = rtt_sample;
    }

    double adjusted_rtt = rtt_sample;
    if (adjusted_rtt - ack_delay > min_rtt) {
      adjusted_rtt -= ack_delay;
    }

    smoothed_rtt += (adjusted_rtt - smoothed_rtt) / 8;
    double var = fabs(smoothed_rtt - adjusted_rtt);
    rtt_var += (var - rtt_var) / 4;
  }

  if (td::Timestamp::in(smoothed_rtt, rtt_round_at).is_in_past(now)) {
    rtt_round_at = now;
    rtt_round++;
  }
}
}  // namespace rldp2
}  // namespace ton
