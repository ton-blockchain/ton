#include "RttStats.h"
#include <cmath>

namespace ton {
namespace rldp2 {
void RttStats::on_rtt_sample(double rtt_sample, double ack_delay, td::Timestamp now) {
  if (rtt_sample < 0.001 || rtt_sample > 10) {
    LOG(WARNING) << "Suspicious rtt sample " << rtt_sample;
    return;
  }
  if (ack_delay < -1e-9 || ack_delay > 10) {
    LOG(WARNING) << "Suspicious ack_delay " << ack_delay;
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
