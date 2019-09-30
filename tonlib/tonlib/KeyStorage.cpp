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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "KeyStorage.h"

#include "tonlib/keys/Mnemonic.h"
#include "tonlib/keys/DecryptedKey.h"
#include "tonlib/keys/EncryptedKey.h"

#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"
#include "td/utils/crypto.h"

namespace tonlib {
namespace {
std::string to_file_name_old(const KeyStorage::Key &key) {
  return td::buffer_to_hex(key.public_key);
}

std::string to_file_name(const KeyStorage::Key &key) {
  return td::buffer_to_hex(td::sha512(key.secret.as_slice()).substr(0, 32));
}
}  // namespace

void KeyStorage::set_key_value(std::shared_ptr<KeyValue> kv) {
  kv_ = std::move(kv);
}

td::Result<KeyStorage::Key> KeyStorage::save_key(const DecryptedKey &decrypted_key, td::Slice local_password) {
  auto encrypted_key = decrypted_key.encrypt(local_password);

  Key res;
  res.public_key = encrypted_key.public_key.as_octet_string();
  res.secret = std::move(encrypted_key.secret);
  TRY_STATUS(kv_->set(to_file_name(res), encrypted_key.encrypted_data));
  return std::move(res);
}

td::Result<KeyStorage::Key> KeyStorage::create_new_key(td::Slice local_password, td::Slice mnemonic_password,
                                                       td::Slice entropy) {
  Mnemonic::Options create_options;
  create_options.password = td::SecureString(mnemonic_password);
  create_options.entropy = td::SecureString(entropy);
  TRY_RESULT(mnemonic, Mnemonic::create_new(std::move(create_options)));

  return save_key(DecryptedKey(std::move(mnemonic)), local_password);
}

td::Result<DecryptedKey> KeyStorage::export_decrypted_key(InputKey input_key) {
  auto r_encrypted_data = kv_->get(to_file_name(input_key.key));
  if (r_encrypted_data.is_error()) {
    r_encrypted_data = kv_->get(to_file_name_old(input_key.key));
    if (r_encrypted_data.is_ok()) {
      LOG(WARNING) << "Restore private from deprecated location " << to_file_name_old(input_key.key) << " --> "
                   << to_file_name(input_key.key);
      TRY_STATUS(kv_->set(to_file_name(input_key.key), r_encrypted_data.ok()));
      kv_->erase(to_file_name_old(input_key.key)).ignore();
    }
  }
  TRY_RESULT(encrypted_data, std::move(r_encrypted_data));
  EncryptedKey encrypted_key{std::move(encrypted_data), td::Ed25519::PublicKey(std::move(input_key.key.public_key)),
                             std::move(input_key.key.secret)};
  return encrypted_key.decrypt(std::move(input_key.local_password));
}

td::Result<KeyStorage::ExportedKey> KeyStorage::export_key(InputKey input_key) {
  TRY_RESULT(decrypted_key, export_decrypted_key(std::move(input_key)));
  ExportedKey exported_key;
  exported_key.mnemonic_words = std::move(decrypted_key.mnemonic_words);
  return std::move(exported_key);
}

td::Result<KeyStorage::PrivateKey> KeyStorage::load_private_key(InputKey input_key) {
  TRY_RESULT(decrypted_key, export_decrypted_key(std::move(input_key)));
  PrivateKey private_key;
  private_key.private_key = decrypted_key.private_key.as_octet_string();
  return std::move(private_key);
}

td::Status KeyStorage::delete_key(const Key &key) {
  return kv_->erase(to_file_name(key));
}

td::Result<KeyStorage::Key> KeyStorage::import_key(td::Slice local_password, td::Slice mnemonic_password,
                                                   ExportedKey exported_key) {
  TRY_RESULT(mnemonic, Mnemonic::create(std::move(exported_key.mnemonic_words), td::SecureString(mnemonic_password)));
  if (!mnemonic.is_basic_seed()) {
    if (mnemonic_password.empty() && mnemonic.is_password_seed()) {
      return td::Status::Error("Mnemonic password is expected");
    }
    return td::Status::Error("Invalid mnemonic words or password (invalid checksum)");
  }
  return save_key(DecryptedKey(std::move(mnemonic)), local_password);
}

td::Result<KeyStorage::ExportedPemKey> KeyStorage::export_pem_key(InputKey input_key, td::Slice key_password) {
  TRY_RESULT(decrypted_key, export_decrypted_key(std::move(input_key)));
  TRY_RESULT(pem, decrypted_key.private_key.as_pem(key_password));
  return ExportedPemKey{std::move(pem)};
}

td::Result<KeyStorage::Key> KeyStorage::change_local_password(InputKey input_key, td::Slice new_local_password) {
  auto new_secret =
      DecryptedKey::change_local_password(input_key.key.secret, input_key.local_password, new_local_password);
  Key res;
  res.public_key = std::move(input_key.key.public_key);
  res.secret = std::move(new_secret);
  TRY_RESULT(value, kv_->get(to_file_name(input_key.key)));
  TRY_STATUS(kv_->add(to_file_name(res), value));
  return std::move(res);
}

td::Result<KeyStorage::Key> KeyStorage::import_pem_key(td::Slice local_password, td::Slice key_password,
                                                       ExportedPemKey exported_key) {
  TRY_RESULT(key, td::Ed25519::PrivateKey::from_pem(exported_key.pem, key_password));
  return save_key(DecryptedKey({}, std::move(key)), local_password);
}

static std::string dummy_secret = "dummy secret of 32 bytes length!";
td::Result<KeyStorage::ExportedEncryptedKey> KeyStorage::export_encrypted_key(InputKey input_key,
                                                                              td::Slice key_password) {
  TRY_RESULT(decrypted_key, export_decrypted_key(std::move(input_key)));
  auto res = decrypted_key.encrypt(key_password, dummy_secret);
  return ExportedEncryptedKey{std::move(res.encrypted_data)};
}

td::Result<KeyStorage::Key> KeyStorage::import_encrypted_key(td::Slice local_password, td::Slice key_password,
                                                             ExportedEncryptedKey exported_key) {
  EncryptedKey encrypted_key{std::move(exported_key.data), td::Ed25519::PublicKey(td::SecureString()),
                             td::SecureString(dummy_secret)};
  TRY_RESULT(decrypted_key, encrypted_key.decrypt(key_password, false));
  return save_key(std::move(decrypted_key), local_password);
}

}  // namespace tonlib
