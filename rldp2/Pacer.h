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
#include "td/utils/optional.h"
#include "td/utils/Time.h"

namespace ton {
namespace rldp2 {
// NB: Should be careful with max_capacity < time_granularity * speed
// We may send packet of any size.
// After that we will be put to sleep till wakeup_at().
// When we are awake we send packet of any size again.
// Logic is - we don't have to wait to send a packet - it is poinless.
// But we have to wait for some time after packet is sent
class Pacer {
 public:
  struct Options {
    Options() {
    }
    double initial_capacity{20};
    double initial_speed{10};
    double max_capacity{40};
    double time_granularity{0.001};
  };
  Pacer(Options options = {});
  td::Timestamp wakeup_at() const;
  void set_speed(double speed);
  td::optional<td::Timestamp> send(double size, td::Timestamp now);

 private:
  double speed_;
  double capacity_;
  double max_capacity_;
  double time_granularity_;
  td::Timestamp capacity_at_;
  td::Timestamp wakeup_at_;

  void update_capacity(td::Timestamp now);
};
}  // namespace rldp2
}  // namespace ton
