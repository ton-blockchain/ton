#pragma once

#include "td/utils/Time.h"
#include "td/utils/TimedStat.h"

namespace ton {
namespace rldp2 {
struct BdwStats {
  struct State {};

  struct PacketInfo {
    td::Timestamp first_sent_at;

    td::Timestamp delivered_now;
    td::uint64 delivered_count{0};
    bool is_paused{false};
  };

  PacketInfo on_packet_send(td::Timestamp first_sent_at) const;
  void on_packet_ack(const PacketInfo &info, td::Timestamp sent_at, td::Timestamp now);

  void on_update(td::Timestamp now, td::uint64 delivered_count_diff);

  void on_pause(td::Timestamp now);
  double windowed_max_bdw{0};

 private:
  td::Timestamp delivered_now;
  td::uint64 delivered_count{0};
  td::TimedStat<td::MaxStat<double>> windowed_max_bdw_stat{5, 0};
  td::Timestamp paused_at_;

  void on_rate_sample(double rate, td::Timestamp now, bool is_paused);
};
}  // namespace rldp2
}  // namespace ton
