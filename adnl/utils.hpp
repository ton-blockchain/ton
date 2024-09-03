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

#include "td/utils/buffer.h"
#include "td/utils/misc.h"
#include "td/utils/crypto.h"
#include "td/utils/format.h"
#include "td/utils/base64.h"
#include "tl-utils/tl-utils.hpp"

#include "common/errorcode.h"
#include "common/checksum.h"
#include "adnl-node-id.hpp"
#include "common/status.h"
#include "adnl-node.h"
#include "adnl-address-list.hpp"

namespace ton {

namespace adnl {

inline bool adnl_node_is_older(AdnlNode &a, AdnlNode &b) {
  return a.addr_list().version() < b.addr_list().version();
}

class RateLimiter {
public:
  explicit RateLimiter(td::uint32 capacity, double period) : capacity_(capacity), period_(period), remaining_(capacity) {
  }

  bool take() {
    while (remaining_ < capacity_ && increment_at_.is_in_past()) {
      ++remaining_;
      increment_at_ += period_;
    }
    if (remaining_) {
      --remaining_;
      if (increment_at_.is_in_past()) {
        increment_at_ = td::Timestamp::in(period_);
      }
      return true;
    }
    return false;
  }

  td::Timestamp ready_at() const {
    if (remaining_) {
      return td::Timestamp::now();
    }
    return increment_at_;
  }

private:
  td::uint32 capacity_;
  double period_;
  td::uint32 remaining_;
  td::Timestamp increment_at_ = td::Timestamp::never();
};

}  // namespace adnl

}  // namespace ton
