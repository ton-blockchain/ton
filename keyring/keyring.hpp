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

#include "keyring.h"
#include "keys/encryptor.h"

#include <map>

namespace ton {

namespace keyring {

class KeyringImpl : public Keyring {
 private:
  struct PrivateKeyDescr {
    td::actor::ActorOwn<DecryptorAsync> decryptor;
    PublicKey public_key;
    bool is_temp;
    PrivateKeyDescr(td::actor::ActorOwn<DecryptorAsync> decryptor, PublicKey public_key, bool is_temp)
        : decryptor(std::move(decryptor)), public_key(public_key), is_temp(is_temp) {
    }
  };

 public:
  void start_up() override;

  td::Result<PrivateKeyDescr*> load_key(PublicKeyHash key_hash);

  void add_key(PrivateKey key, bool is_temp, td::Promise<td::Unit> promise) override;
  void check_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) override;
  void add_key_short(PublicKeyHash key_hash, td::Promise<PublicKey> promise) override;
  void del_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) override;

  void get_public_key(PublicKeyHash key_hash, td::Promise<PublicKey> promise) override;
  void sign_message(PublicKeyHash key_hash, td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;
  void sign_add_get_public_key(PublicKeyHash key_hash, td::BufferSlice data,
                               td::Promise<std::pair<td::BufferSlice, PublicKey>> promise) override;
  void sign_messages(PublicKeyHash key_hash, std::vector<td::BufferSlice> data,
                     td::Promise<std::vector<td::Result<td::BufferSlice>>> promise) override;

  void decrypt_message(PublicKeyHash key_hash, td::BufferSlice data, td::Promise<td::BufferSlice> promise) override;

  KeyringImpl(std::string db_root) : db_root_(db_root) {
  }

 private:
  std::map<PublicKeyHash, std::unique_ptr<PrivateKeyDescr>> map_;
  std::unique_ptr<Decryptor> decryptor_;
  std::unique_ptr<Encryptor> encryptor_;

  std::string db_root_;
};

}  // namespace keyring

}  // namespace ton

