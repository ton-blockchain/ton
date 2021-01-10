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
#include "keys/keys.hpp"

namespace ton {

namespace keyring {

class Keyring : public td::actor::Actor {
 public:
  virtual ~Keyring() = default;

  virtual void add_key(PrivateKey key, bool temp, td::Promise<td::Unit> promise) = 0;
  virtual void check_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) = 0;
  virtual void add_key_short(PublicKeyHash key_hash, td::Promise<PublicKey> promise) = 0;
  virtual void del_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) = 0;

  //virtual void export_private_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) = 0;
  virtual void get_public_key(PublicKeyHash key_hash, td::Promise<PublicKey> promise) = 0;
  virtual void sign_message(PublicKeyHash key_hash, td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;
  virtual void sign_add_get_public_key(PublicKeyHash key_hash, td::BufferSlice data,
                                       td::Promise<std::pair<td::BufferSlice, PublicKey>> promise) = 0;
  virtual void sign_messages(PublicKeyHash key_hash, std::vector<td::BufferSlice> data,
                             td::Promise<std::vector<td::Result<td::BufferSlice>>> promise) = 0;

  virtual void decrypt_message(PublicKeyHash key_hash, td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;

  static td::actor::ActorOwn<Keyring> create(std::string db_root);
};

}  // namespace keyring

}  // namespace ton
