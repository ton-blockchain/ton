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

#include "rldp.h"
#include "rldp-peer.h"

#include "tl-utils/tl-utils.hpp"
#include "adnl/adnl-query.h"

#include <map>

namespace ton {

namespace rldp {

constexpr int VERBOSITY_NAME(RLDP_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(RLDP_NOTICE) = verbosity_INFO;
constexpr int VERBOSITY_NAME(RLDP_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(RLDP_DEBUG) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(RLDP_EXTRA_DEBUG) = verbosity_DEBUG + 1;

using TransferId = td::Bits256;

class RldpImpl : public Rldp {
 public:
  virtual void transfer_completed(TransferId transfer_id) = 0;
  //virtual void in_transfer_completed(TransferId transfer_id) = 0;
};

}  // namespace rldp

}  // namespace ton
