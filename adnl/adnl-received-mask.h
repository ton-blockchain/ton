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

#include "td/utils/int_types.h"
#include "td/utils/logging.h"

namespace ton {

namespace adnl {

class AdnlReceivedMask {
 public:
  void reset() {
    seqno_ = 0;
    mask_ = 0;
  }
  bool packet_is_delivered(td::int64 seqno) const {
    if (seqno <= 0) {
      return false;
    }
    if (seqno + 64 <= seqno_) {
      return true;
    }
    if (seqno > seqno_) {
      return false;
    }
    return mask_ & (1ull << (seqno_ - seqno));
  }
  void deliver_packet(td::int64 seqno) {
    CHECK(!packet_is_delivered(seqno));

    CHECK(seqno > 0);
    if (seqno <= seqno_) {
      mask_ |= (1ull << (seqno_ - seqno));
    } else {
      auto old = seqno_;
      seqno_ = seqno;
      if (seqno_ - old >= 64) {
        mask_ = 1;
      } else {
        mask_ = mask_ << (seqno_ - old);
        mask_ |= 1;
      }
    }
  }

 private:
  td::int64 seqno_{0};
  td::uint64 mask_{0};
};

class AdnlReceivedMaskVersion {
 public:
  bool packet_is_delivered(td::int32 utime, td::uint64 seqno) {
    if (utime < utime_) {
      return true;
    } else if (utime == utime_) {
      return mask_.packet_is_delivered(seqno);
    } else {
      return false;
    }
  }
  void deliver_packet(td::int32 utime, td::uint64 seqno) {
    CHECK(utime >= utime_);
    if (utime == utime_) {
      mask_.deliver_packet(seqno);
    } else {
      utime_ = utime;
      mask_.reset();
      mask_.deliver_packet(seqno);
    }
  }
  void reset() {
    mask_.reset();
    utime_ = 0;
  }

 private:
  AdnlReceivedMask mask_;
  td::int32 utime_{0};
};

}  // namespace adnl

}  // namespace ton
