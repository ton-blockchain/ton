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

#include "ton/ton-types.h"
#include "auto/tl/ton_api.h"
#include "keys/keys.hpp"

namespace ton {

namespace validator {

class ValidatorFullId : public PublicKey {
 public:
  NodeIdShort short_id() const;
  operator Ed25519_PublicKey() const;

  ValidatorFullId(PublicKey id) : PublicKey{std::move(id)} {
  }
  ValidatorFullId(const Ed25519_PublicKey& key) : PublicKey{pubkeys::Ed25519{key.as_bits256()}} {
  }
};

}  // namespace validator

}  // namespace ton
