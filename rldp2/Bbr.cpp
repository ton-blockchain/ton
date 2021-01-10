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

#include "Bbr.h"

#include "BdwStats.h"
#include "RttStats.h"

#include "td/utils/Random.h"

namespace ton {
namespace rldp2 {
void Bbr::step(const RttStats &rtt_stats, const BdwStats &bdw_stats, td::uint64 in_flight, td::Timestamp now) {
  rtt_min_ = rtt_stats.windowed_min_rtt;
  bdw_max_ = bdw_stats.windowed_max_bdw;
  if (bdw_max_ > bdw_peak_ * 1.25) {
    bdw_peak_ = bdw_max_;
    bdw_peak_at_round = rtt_stats.rtt_round;
    //LOG(ERROR) << "NEW PEAK " << bdw_peak_ * 768;
  }

  if (state_ == State::Start && bdw_peak_at_round + 3 < rtt_stats.rtt_round) {
    //LOG(ERROR) << "START -> DRAIN";
    state_ = State::Drain;
  }

  if (state_ == State::Drain && (double)in_flight < bdw_max_ * rtt_min_) {
    //LOG(ERROR) << "DRAIN -> BPROBE BDW";
    state_ = State::ProbeBdw;
    probe_bdw_cycle_ = td::Random::fast(1, 5);
    probe_bdw_cycle_at_ = now;
  }

  if (state_ == State::ProbeBdw && td::Timestamp::in(rtt_stats.windowed_min_rtt, probe_bdw_cycle_at_).is_in_past(now)) {
    probe_bdw_cycle_at_ = now;
    probe_bdw_cycle_ = (probe_bdw_cycle_ + 1) % 6;
    //LOG(ERROR) << "NEW PROBE BDW CYCLE";
  }

  //TODO: ProbeRtt state. Don't want to implenent now without proper tests
}

double Bbr::get_rate() const {
  if (state_ == State::Start) {
    return bdw_max_ * 2.8;
  }
  if (state_ == State::Drain) {
    return bdw_max_ / 2.8;
  }
  if (state_ == State::ProbeBdw) {
    constexpr double probe_bdw_gain[6] = {0.75, 1, 1, 1, 1, 1.25};
    return probe_bdw_gain[probe_bdw_cycle_] * bdw_max_;
  }
  UNREACHABLE();
}

td::uint32 Bbr::get_window_size() const {
  if (state_ == State::Start || state_ == State::Drain) {
    return td::max(td::uint32(bdw_max_ * rtt_min_ * 2.8 + 1), 10u);
  }
  return td::max(td::uint32(bdw_max_ * rtt_min_ * 2 + 1), 10u);
}

}  // namespace rldp2
}  // namespace ton
