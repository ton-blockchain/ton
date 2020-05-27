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
