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

#include "LossSender.h"

#include "td/utils/int_types.h"

namespace ton {
namespace rldp2 {
struct LossStats {
  void on_update(td::uint32 ack, td::uint32 lost);
  double loss = 0.1;
  LossSender prob{0.1, 1e-9};

 private:
  td::uint32 ack_{0};
  td::uint32 lost_{0};
};
}  // namespace rldp2
}  // namespace ton
