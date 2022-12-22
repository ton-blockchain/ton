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

#include "td/utils/StringBuilder.h"
#include "td/utils/Time.h"
#include "td/utils/VectorQueue.h"

namespace ton {
class LoadSpeed {
 public:
  void add(td::uint64 size, td::Timestamp now = td::Timestamp::now());
  double speed(td::Timestamp now = td::Timestamp::now()) const;
  friend td::StringBuilder &operator<<(td::StringBuilder &sb, const LoadSpeed &speed);

 private:
  struct Event {
    td::uint64 size;
    td::Timestamp at;
  };
  mutable td::VectorQueue<Event> events_;
  mutable td::uint64 total_size_{0};

  double duration(td::Timestamp now) const;
  void update(td::Timestamp now) const;
};
}  // namespace ton
