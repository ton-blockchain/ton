#pragma once

#include "td/utils/Time.h"
#include "td/utils/Variant.h"

#include "FecHelper.h"
#include "SenderPackets.h"

namespace ton {
namespace rldp2 {
struct Ack;
struct BdwStats;
struct RttStats;
struct LossStats;

inline td::uint32 sub_or_zero(td::uint32 a, td::uint32 b) {
  if (a < b) {
    return 0;
  }
  return a - b;
}

class RldpSender {
 public:
  struct Config {
    static constexpr double DEFAULT_MAX_ACK_DELAY = 0.01;
    static constexpr td::uint32 DEFAULT_PACKET_TRESHOLD = 3;
    static constexpr double DEFAULT_INITIAL_RTT = 0.5;

    double max_ack_delay{DEFAULT_MAX_ACK_DELAY};
    double ack_delay{DEFAULT_MAX_ACK_DELAY};
    td::uint32 packet_treshold{DEFAULT_PACKET_TRESHOLD};
    double initial_rtt{DEFAULT_INITIAL_RTT};
  };

  RldpSender() = default;
  RldpSender(Config config, td::uint32 symbols_count) : config_(config) {
    fec_helper_.symbols_count = symbols_count;
    extra_symbols_ = fec_helper_.get_left_fec_symbols_count();
  }

  struct ActionWait {
    td::Timestamp wait_till;
  };

  struct ActionSend {
    td::uint32 seqno;
    bool is_probe;
  };

  td::Variant<ActionWait, ActionSend> next_action(td::Timestamp now, bool only_probe = false);
  td::Variant<ActionWait, ActionSend> next_probe(td::Timestamp now);

  td::uint32 get_inflight_symbols_count() const {
    return packets_.in_flight_count();
  }

  SenderPackets::Update on_ack(const Ack &ack, double ack_delay, td::Timestamp now, RttStats &rtt_stats,
                               BdwStats &bdw_stats, LossStats &loss_stats);

  void on_send(td::uint32 seqno, td::Timestamp now, bool is_probe, const RttStats &rtt_stats,
               const BdwStats &bdw_state);

 private:
  Config config_;
  SenderPackets packets_;
  FecHelper fec_helper_;
  td::Timestamp probe_timeout_;
  td::uint32 probe_k_{1};
  td::uint32 extra_symbols_{0};

  double get_loss_delay(const RttStats &rtt_stats);

  double get_probe_delay(const RttStats &rtt_stats);

  td::uint32 get_loss_seqno_delay() {
    return config_.packet_treshold;
  }
};
}  // namespace rldp2
}  // namespace ton
