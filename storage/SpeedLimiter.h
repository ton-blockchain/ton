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
*/

#pragma once
#include "td/actor/actor.h"
#include <queue>

namespace ton {

class SpeedLimiter : public td::actor::Actor {
 public:
  explicit SpeedLimiter(double max_speed);

  void set_max_speed(double max_speed);  // Negative = unlimited
  void enqueue(double size, td::Timestamp timeout, td::Promise<td::Unit> promise);

  void alarm() override;

 private:
  double max_speed_ = -1.0;
  td::Timestamp unlock_at_ = td::Timestamp::never();

  struct Event {
    td::Timestamp execute_at_;
    double size_;
    td::Timestamp timeout_;
    td::Promise<td::Unit> promise_;
  };
  std::queue<Event> queue_;

  void process_queue();
};

struct SpeedLimiters {
  td::actor::ActorId<SpeedLimiter> download, upload;
};

}  // namespace ton
