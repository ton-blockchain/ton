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

#include "common/checksum.h"
#include "common/errorcode.h"
#include "common/status.h"
#include "td/utils/base64.h"
#include "td/utils/buffer.h"
#include "td/utils/crypto.h"
#include "td/utils/format.h"
#include "td/utils/misc.h"
#include "tl-utils/tl-utils.hpp"

#include "adnl-address-list.hpp"
#include "adnl-node-id.hpp"
#include "adnl-node.h"

namespace ton {

namespace adnl {

inline bool adnl_node_is_older(AdnlNode &a, AdnlNode &b) {
  return a.addr_list().version() < b.addr_list().version();
}

class RateLimiter {
 public:
  explicit RateLimiter(td::uint32 capacity, double period)
      : period_(period), emission_interval_(make_emission_interval(capacity, period)), ready_at_{-emission_interval_} {
  }

  bool take() {
    const auto now = td::Timestamp::now();
    auto min_ready_at = now.at() - emission_interval_;
    if (ready_at_ < min_ready_at) {
      ready_at_ = min_ready_at;
    }
    if (ready_at_ > now.at()) {
      return false;
    }
    ready_at_ += period_;
    return true;
  }

  td::Timestamp ready_at() const {
    const auto now = td::Timestamp::now();
    if (ready_at_ > now.at()) {
      return td::Timestamp::at(ready_at_);
    }
    return now;
  }

  bool is_full() const {
    return ready_at_ < td::Timestamp::now().at() - emission_interval_;
  }

  double period() const {
    return period_;
  }

 private:
  static double make_emission_interval(td::uint32 capacity, double period) {
    CHECK(capacity >= 1);
    CHECK(period > 0.0);
    return static_cast<double>(capacity - 1) * period;
  }

  double period_;
  double emission_interval_;
  double ready_at_;
};

}  // namespace adnl

}  // namespace ton
