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

#include "LoadSpeed.h"

#include "td/utils/format.h"

namespace ton {
void LoadSpeed::add(td::uint64 size, td::Timestamp now) {
  total_size_ += size;
  events_.push(Event{size, now});
  update(now);
}
double LoadSpeed::speed(td::Timestamp now) const {
  update(now);
  return (double)total_size_ / duration(now);
}

td::StringBuilder &operator<<(td::StringBuilder &sb, const LoadSpeed &speed) {
  return sb << td::format::as_size(static_cast<td::uint64>(speed.speed())) << "/s";
}

void LoadSpeed::update(td::Timestamp now) const {
  while (duration(now) > 30) {
    total_size_ -= events_.front().size;
    events_.pop();
  }
}
double LoadSpeed::duration(td::Timestamp now) const {
  double res = 5;
  if (!events_.empty()) {
    res = std::max(res, now.at() - events_.front().at.at());
  }
  return res;
}
}  // namespace ton
