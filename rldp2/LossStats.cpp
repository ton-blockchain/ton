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

#include "LossStats.h"
#include "td/utils/misc.h"

#include <cmath>

namespace ton {
namespace rldp2 {
void LossStats::on_update(td::uint32 ack, td::uint32 lost) {
  ack_ += ack;
  lost_ += lost;

  if (ack_ + lost_ > 1000) {
    auto new_loss = td::clamp((double)lost_ / (ack_ + lost_), 0.001, 0.2);
    if (fabs(new_loss - loss) > 5e-3) {
      prob = LossSender(new_loss, 1e-9);
    }
    loss = new_loss;
    ack_ = 0;
    lost_ = 0;
  }
}
}  // namespace rldp2
}  // namespace ton
