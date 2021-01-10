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
#include "keyring.hpp"
#include "common/errorcode.h"
#include "common/io.hpp"
#include "td/utils/port/path.h"
#include "td/utils/filesystem.h"
#include "td/utils/Random.h"

namespace ton {

namespace keyring {

void KeyringImpl::start_up() {
  if (db_root_.size() > 0) {
    td::mkdir(db_root_).ensure();
  }
}

td::Result<KeyringImpl::PrivateKeyDescr *> KeyringImpl::load_key(PublicKeyHash key_hash) {
  auto it = map_.find(key_hash);
  if (it != map_.end()) {
    return it->second.get();
  }

  if (db_root_.size() == 0) {
    return td::Status::Error(ErrorCode::notready, "key not in db");
  }

  auto name = db_root_ + "/" + key_hash.bits256_value().to_hex();

  auto R = td::read_file(td::CSlice{name});
  if (R.is_error()) {
    return R.move_as_error_prefix("key not in db: ");
  }
  auto data = R.move_as_ok();
  auto R2 = PrivateKey::import(td::SecureString(data));
  R2.ensure();

  auto key = R2.move_as_ok();
  auto pub = key.compute_public_key();
  auto short_id = pub.compute_short_id();
  CHECK(short_id == key_hash);

  auto D = key.create_decryptor_async();
  D.ensure();

  return map_.emplace(short_id, std::make_unique<PrivateKeyDescr>(D.move_as_ok(), pub, false)).first->second.get();
}

void KeyringImpl::add_key(PrivateKey key, bool is_temp, td::Promise<td::Unit> promise) {
  auto pub = key.compute_public_key();
  auto short_id = pub.compute_short_id();

  if (map_.count(short_id)) {
    LOG(WARNING) << "duplicate key " << short_id;
    promise.set_value(td::Unit());
    return;
  }
  if (db_root_.size() == 0) {
    CHECK(is_temp);
  }
  auto D = key.create_decryptor_async();
  D.ensure();

  map_.emplace(short_id, std::make_unique<PrivateKeyDescr>(D.move_as_ok(), pub, is_temp));

  if (!is_temp && key.exportable()) {
    auto S = key.export_as_slice();
    auto name = db_root_ + "/" + short_id.bits256_value().to_hex();

    td::write_file(td::CSlice(name), S.as_slice()).ensure();
  }
  promise.set_value(td::Unit());
}

void KeyringImpl::check_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
  } else {
    promise.set_value(td::Unit());
  }
}

void KeyringImpl::add_key_short(PublicKeyHash key_hash, td::Promise<PublicKey> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
  } else {
    promise.set_result(map_[key_hash]->public_key);
  }
}

void KeyringImpl::del_key(PublicKeyHash key_hash, td::Promise<td::Unit> promise) {
  map_.erase(key_hash);
  if (db_root_.size() == 0) {
    return promise.set_value(td::Unit());
  }
  auto name = db_root_ + "/" + key_hash.bits256_value().to_hex();
  td::BufferSlice d{256};
  td::Random::secure_bytes(d.as_slice());
  td::write_file(name, d.as_slice()).ensure();
  td::unlink(name).ensure();
  promise.set_value(td::Unit());
}

void KeyringImpl::get_public_key(PublicKeyHash key_hash, td::Promise<PublicKey> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
  } else {
    promise.set_result(map_[key_hash]->public_key);
  }
}

void KeyringImpl::sign_message(PublicKeyHash key_hash, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
  } else {
    td::actor::send_closure(S.move_as_ok()->decryptor, &DecryptorAsync::sign, std::move(data), std::move(promise));
  }
}

void KeyringImpl::sign_add_get_public_key(PublicKeyHash key_hash, td::BufferSlice data,
                                          td::Promise<std::pair<td::BufferSlice, PublicKey>> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
    return;
  }

  auto D = S.move_as_ok();
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), id = D->public_key](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
          return;
        }
        promise.set_value(std::pair<td::BufferSlice, PublicKey>{R.move_as_ok(), id});
      });
  td::actor::send_closure(D->decryptor, &DecryptorAsync::sign, std::move(data), std::move(P));
}

void KeyringImpl::sign_messages(PublicKeyHash key_hash, std::vector<td::BufferSlice> data,
                                td::Promise<std::vector<td::Result<td::BufferSlice>>> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
  } else {
    td::actor::send_closure(S.move_as_ok()->decryptor, &DecryptorAsync::sign_batch, std::move(data),
                            std::move(promise));
  }
}

void KeyringImpl::decrypt_message(PublicKeyHash key_hash, td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  auto S = load_key(key_hash);

  if (S.is_error()) {
    promise.set_error(S.move_as_error());
  } else {
    td::actor::send_closure(S.move_as_ok()->decryptor, &DecryptorAsync::decrypt, std::move(data), std::move(promise));
  }
}

td::actor::ActorOwn<Keyring> Keyring::create(std::string db_root) {
  return td::actor::create_actor<KeyringImpl>("keyring", db_root);
}

}  // namespace keyring

}  // namespace ton
