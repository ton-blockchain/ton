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

#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "common/bitstring.h"

namespace ton::validator {

class LiteServerCache : public td::actor::Actor {
 public:
  ~LiteServerCache() override = default;

  virtual void lookup(td::Bits256 key, td::Promise<td::BufferSlice> promise) = 0;
  virtual void update(td::Bits256 key, td::BufferSlice value) = 0;

  virtual void process_send_message(td::Bits256 key, td::Promise<td::Unit> promise) = 0;
};

} // namespace ton::validator