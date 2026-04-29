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

#include <map>
#include <memory>

#include "adnl/adnl-query.h"
#include "metrics/metrics-collectors.h"
#include "tl-utils/tl-utils.hpp"

#include "rldp-peer.h"
#include "rldp.h"

namespace ton {

namespace rldp {

constexpr int VERBOSITY_NAME(RLDP_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(RLDP_NOTICE) = verbosity_INFO;
constexpr int VERBOSITY_NAME(RLDP_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(RLDP_DEBUG) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(RLDP_EXTRA_DEBUG) = verbosity_DEBUG + 1;

using TransferId = td::Bits256;

struct RldpMetrics {
  struct AtomicKindCounter {
    std::shared_ptr<metrics::AtomicCounter<td::uint64>> bytes;
    std::shared_ptr<metrics::AtomicCounter<td::uint64>> msgs;

    void record(td::uint64 size) const {
      bytes->add(size);
      msgs->add(1);
    }
  };
  using CounterPtr = std::shared_ptr<metrics::AtomicCounter<td::uint64>>;
  AtomicKindCounter sent_to_adnl_part, sent_to_adnl_confirm, sent_to_adnl_complete;
  AtomicKindCounter received_part, received_confirm, received_complete;
  CounterPtr transfers_started;
  CounterPtr transfers_completed_out;
  CounterPtr transfers_completed_in;
  CounterPtr transfers_failed_in;
  CounterPtr parse_errors_part;
  CounterPtr parse_errors_message;
};

class RldpImpl : public Rldp {
 public:
  virtual void transfer_completed(TransferId transfer_id) = 0;
  //virtual void in_transfer_completed(TransferId transfer_id) = 0;
};

}  // namespace rldp

}  // namespace ton
