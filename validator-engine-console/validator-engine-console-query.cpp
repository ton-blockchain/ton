/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "validator-engine-console-query.h"
#include "auto/tl/ton_api.h"
#include "td/utils/StringBuilder.h"
#include "validator-engine-console.h"
#include "terminal/terminal.h"
#include "td/utils/filesystem.h"
#include "overlay/overlays.h"
#include "ton/ton-tl.hpp"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "keys/encryptor.h"
#include "td/utils/port/path.h"
#include "tl/tl_json.h"

#include <cctype>
#include <fstream>

Tokenizer::Tokenizer(td::BufferSlice data) : data_(std::move(data)) {
  remaining_ = data_.as_slice();
}

void Tokenizer::skipspc() {
  while (remaining_.size() > 0 && std::isspace(remaining_[0])) {
    remaining_.remove_prefix(1);
  }
}

bool Tokenizer::endl() {
  skipspc();
  return remaining_.size() == 0;
}

td::Result<td::Slice> Tokenizer::get_raw_token() {
  skipspc();
  if (remaining_.size() == 0) {
    return td::Status::Error("failed to parse token: EOL");
  }
  size_t idx = 0;
  while (idx < remaining_.size() && !std::isspace(remaining_[idx])) {
    idx++;
  }
  auto r = remaining_.copy().truncate(idx);
  remaining_.remove_prefix(idx);
  return r;
}

td::Result<td::Slice> Tokenizer::peek_raw_token() {
  skipspc();
  if (remaining_.size() == 0) {
    return td::Status::Error("failed to parse token: EOL");
  }
  size_t idx = 0;
  while (idx < remaining_.size() && !std::isspace(remaining_[idx])) {
    idx++;
  }
  auto r = remaining_.copy().truncate(idx);
  return r;
}

void Query::start_up() {
  auto R = [&]() -> td::Status {
    TRY_STATUS(run());
    TRY_STATUS(send());
    return td::Status::OK();
  }();
  if (R.is_error()) {
    handle_error(std::move(R));
  }
}

void Query::handle_error(td::Status error) {
  td::TerminalIO::out() << "Failed " << name() << " query: " << error << "\n";
  td::actor::send_closure(console_, &ValidatorEngineConsole::got_result, false);
  stop();
}

void Query::receive_wrap(td::BufferSlice R) {
  auto S = receive(std::move(R));
  if (S.is_error()) {
    handle_error(std::move(S));
  } else {
    td::actor::send_closure(console_, &ValidatorEngineConsole::got_result, true);
    stop();
  }
}

td::Status GetTimeQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetTimeQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getTime>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetTimeQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_time>(std::move(data), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "received validator time: time=" << f->time_ << "\n";
  return td::Status::OK();
}

td::Status GetHelpQuery::run() {
  if (tokenizer_.endl()) {
    return td::Status::OK();
  }
  TRY_RESULT_ASSIGN(command_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetHelpQuery::send() {
  td::actor::send_closure(console_, &ValidatorEngineConsole::show_help, command_, create_promise());
  return td::Status::OK();
}

td::Status GetHelpQuery::receive(td::BufferSlice R) {
  CHECK(R.size() == 0);
  return td::Status::OK();
}

td::Status GetLicenseQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetLicenseQuery::send() {
  td::actor::send_closure(console_, &ValidatorEngineConsole::show_license, create_promise());
  return td::Status::OK();
}

td::Status GetLicenseQuery::receive(td::BufferSlice R) {
  CHECK(R.size() == 0);
  return td::Status::OK();
}

td::Status NewKeyQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status NewKeyQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_generateKeyPair>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status NewKeyQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_keyHash>(std::move(data), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "created new key " << f->key_hash_.to_hex() << "\n";
  return td::Status::OK();
}

td::Status ImportPrivateKeyFileQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status ImportPrivateKeyFileQuery::send() {
  TRY_RESULT(data, td::read_file_secure(file_name_));
  TRY_RESULT(pk, ton::PrivateKey::import(data.as_slice()));
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_importPrivateKey>(pk.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ImportPrivateKeyFileQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_keyHash>(std::move(data), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "imported key " << f->key_hash_.to_hex() << "\n";
  return td::Status::OK();
}

td::Status ExportPublicKeyQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status ExportPublicKeyQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_exportPublicKey>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ExportPublicKeyQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::PublicKey>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "got public key: " << td::base64_encode(data.as_slice()) << "\n";
  return td::Status::OK();
}

td::Status ExportPublicKeyFileQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status ExportPublicKeyFileQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_exportPublicKey>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ExportPublicKeyFileQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::PublicKey>(data.as_slice(), true),
                    "received incorrect answer: ");
  TRY_STATUS(td::write_file(file_name_, data.as_slice()));
  td::TerminalIO::out() << "got public key\n";
  return td::Status::OK();
}

td::Status SignQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(data_, tokenizer_.get_token<td::BufferSlice>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status SignQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_sign>(key_hash_.tl(), std::move(data_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SignQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_signature>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "got signature " << td::base64_encode(f->signature_) << "\n";
  return td::Status::OK();
}

td::Status SignFileQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(in_file_, tokenizer_.get_token<std::string>());
  TRY_RESULT_ASSIGN(out_file_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status SignFileQuery::send() {
  TRY_RESULT(data, td::read_file(in_file_));
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_sign>(key_hash_.tl(), std::move(data));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SignFileQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_signature>(data.as_slice(), true),
                    "received incorrect answer: ");
  TRY_STATUS(td::write_file(out_file_, f->signature_.as_slice()));
  td::TerminalIO::out() << "got signature\n";
  return td::Status::OK();
}

td::Status ExportAllPrivateKeysQuery::run() {
  TRY_RESULT_ASSIGN(directory_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  client_pk_ = ton::privkeys::Ed25519::random();
  return td::Status::OK();
}

td::Status ExportAllPrivateKeysQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_exportAllPrivateKeys>(
      client_pk_.compute_public_key().tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ExportAllPrivateKeysQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_exportedPrivateKeys>(data.as_slice(), true),
                    "received incorrect answer: ");
  // Private keys are encrypted using client-provided public key to avoid storing them in
  // non-secure buffers (not td::SecureString)
  TRY_RESULT_PREFIX(decryptor, client_pk_.create_decryptor(), "cannot create decryptor: ");
  TRY_RESULT_PREFIX(keys_data, decryptor->decrypt(f->encrypted_data_.as_slice()), "cannot decrypt data: ");
  SCOPE_EXIT {
    keys_data.as_slice().fill_zero_secure();
  };
  td::Slice slice = keys_data.as_slice();
  if (slice.size() < 32) {
    return td::Status::Error("data is too small");
  }
  slice.remove_suffix(32);
  std::vector<ton::PrivateKey> private_keys;
  while (!slice.empty()) {
    if (slice.size() < 4) {
      return td::Status::Error("unexpected end of data");
    }
    td::uint32 size;
    td::MutableSlice{reinterpret_cast<char *>(&size), 4}.copy_from(slice.substr(0, 4));
    if (size > slice.size()) {
      return td::Status::Error("unexpected end of data");
    }
    slice.remove_prefix(4);
    TRY_RESULT_PREFIX(private_key, ton::PrivateKey::import(slice.substr(0, size)), "cannot parse private key: ");
    if (!private_key.exportable()) {
      return td::Status::Error("private key is not exportable");
    }
    private_keys.push_back(std::move(private_key));
    slice.remove_prefix(size);
  }

  TRY_STATUS_PREFIX(td::mkpath(directory_ + "/"), "cannot create directory " + directory_ + ": ");
  td::TerminalIO::out() << "exported " << private_keys.size() << " private keys" << "\n";
  for (const ton::PrivateKey &private_key : private_keys) {
    std::string hash_hex = private_key.compute_short_id().bits256_value().to_hex();
    TRY_STATUS_PREFIX(td::write_file(directory_ + "/" + hash_hex, private_key.export_as_slice()),
                      "failed to write file: ");
    td::TerminalIO::out() << "pubkey_hash " << hash_hex << "\n";
  }
  td::TerminalIO::out() << "written all files to " << directory_ << "\n";
  return td::Status::OK();
}

td::Status AddAdnlAddrQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(category_, tokenizer_.get_token<td::uint32>());

  if (category_ > 15) {
    return td::Status::Error("too big category");
  }
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddAdnlAddrQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addAdnlId>(key_hash_.tl(), category_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddAdnlAddrQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddDhtIdQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddDhtIdQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addDhtId>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddDhtIdQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddValidatorPermanentKeyQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(election_date_, tokenizer_.get_token<td::uint32>());
  TRY_RESULT_ASSIGN(expire_at_, tokenizer_.get_token<td::uint32>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddValidatorPermanentKeyQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addValidatorPermanentKey>(
      key_hash_.tl(), election_date_, expire_at_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddValidatorPermanentKeyQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddValidatorTempKeyQuery::run() {
  TRY_RESULT_ASSIGN(perm_key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(expire_at_, tokenizer_.get_token<td::uint32>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddValidatorTempKeyQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addValidatorTempKey>(
      perm_key_hash_.tl(), key_hash_.tl(), expire_at_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddValidatorTempKeyQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status ChangeFullNodeAdnlAddrQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status ChangeFullNodeAdnlAddrQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_changeFullNodeAdnlAddress>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ChangeFullNodeAdnlAddrQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddValidatorAdnlAddrQuery::run() {
  TRY_RESULT_ASSIGN(perm_key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(expire_at_, tokenizer_.get_token<td::uint32>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddValidatorAdnlAddrQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addValidatorAdnlAddress>(
      perm_key_hash_.tl(), key_hash_.tl(), expire_at_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddValidatorAdnlAddrQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddLiteServerQuery::run() {
  TRY_RESULT_ASSIGN(port_, tokenizer_.get_token<td::uint16>());
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddLiteServerQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addLiteserver>(key_hash_.tl(), port_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddLiteServerQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status DelAdnlAddrQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelAdnlAddrQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delAdnlId>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelAdnlAddrQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status DelDhtIdQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelDhtIdQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delDhtId>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelDhtIdQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status DelValidatorPermanentKeyQuery::run() {
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelValidatorPermanentKeyQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delValidatorPermanentKey>(key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelValidatorPermanentKeyQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status DelValidatorTempKeyQuery::run() {
  TRY_RESULT_ASSIGN(perm_key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelValidatorTempKeyQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delValidatorTempKey>(perm_key_hash_.tl(),
                                                                                               key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelValidatorTempKeyQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status DelValidatorAdnlAddrQuery::run() {
  TRY_RESULT_ASSIGN(perm_key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(key_hash_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelValidatorAdnlAddrQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delValidatorAdnlAddress>(perm_key_hash_.tl(),
                                                                                                   key_hash_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelValidatorAdnlAddrQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status GetConfigQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetConfigQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getConfig>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetConfigQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_jsonConfig>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "---------\n" << f->data_ << "--------\n";
  return td::Status::OK();
}

td::Status SetVerbosityQuery::run() {
  TRY_RESULT_ASSIGN(verbosity_, tokenizer_.get_token<td::uint8>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status SetVerbosityQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_setVerbosity>(verbosity_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SetVerbosityQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status GetStatsQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetStatsQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getStats>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetStatsQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_stats>(data.as_slice(), true),
                    "received incorrect answer: ");

  for (auto &v : f->stats_) {
    td::TerminalIO::out() << v->key_ << "\t\t\t" << v->value_ << "\n";
  }
  return td::Status::OK();
}

td::Status QuitQuery::send() {
  td::actor::send_closure(console_, &ValidatorEngineConsole::close);
  return td::Status::OK();
}

td::Status AddNetworkAddressQuery::run() {
  TRY_RESULT_ASSIGN(addr_, tokenizer_.get_token<td::IPAddress>());
  TRY_RESULT_ASSIGN(cats_, tokenizer_.get_token_vector<td::int32>());
  TRY_RESULT_ASSIGN(prio_cats_, tokenizer_.get_token_vector<td::int32>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddNetworkAddressQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addListeningPort>(
      static_cast<td::int32>(addr_.get_ipv4()), addr_.get_port(), std::move(cats_), std::move(prio_cats_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddNetworkAddressQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddNetworkProxyAddressQuery::run() {
  TRY_RESULT_ASSIGN(in_addr_, tokenizer_.get_token<td::IPAddress>());
  TRY_RESULT_ASSIGN(out_addr_, tokenizer_.get_token<td::IPAddress>());
  TRY_RESULT_ASSIGN(id_, tokenizer_.get_token<td::Bits256>());
  TRY_RESULT_ASSIGN(shared_secret_, tokenizer_.get_token<td::BufferSlice>());
  TRY_RESULT_ASSIGN(cats_, tokenizer_.get_token_vector<td::int32>());
  TRY_RESULT_ASSIGN(prio_cats_, tokenizer_.get_token_vector<td::int32>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddNetworkProxyAddressQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addProxy>(
      static_cast<td::int32>(in_addr_.get_ipv4()), in_addr_.get_port(), static_cast<td::int32>(out_addr_.get_ipv4()),
      out_addr_.get_port(), ton::create_tl_object<ton::ton_api::adnl_proxy_fast>(id_, std::move(shared_secret_)),
      std::move(cats_), std::move(prio_cats_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddNetworkProxyAddressQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status CreateElectionBidQuery::run() {
  TRY_RESULT_ASSIGN(date_, tokenizer_.get_token<td::uint32>());
  TRY_RESULT_ASSIGN(elector_addr_, tokenizer_.get_token<std::string>());
  TRY_RESULT_ASSIGN(wallet_, tokenizer_.get_token<std::string>());
  TRY_RESULT_ASSIGN(fname_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status CreateElectionBidQuery::send() {
  auto b =
      ton::create_serialize_tl_object<ton::ton_api::engine_validator_createElectionBid>(date_, elector_addr_, wallet_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status CreateElectionBidQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_electionBid>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success: permkey=" << f->perm_key_.to_hex() << " adnl=" << f->adnl_addr_.to_hex() << "\n";
  TRY_STATUS(td::write_file(fname_, f->to_send_payload_.as_slice()));
  return td::Status::OK();
}

td::Status CreateProposalVoteQuery::run() {
  TRY_RESULT_ASSIGN(data_, tokenizer_.get_token<std::string>());
  TRY_RESULT_ASSIGN(fname_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status CreateProposalVoteQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_createProposalVote>(td::BufferSlice(data_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status CreateProposalVoteQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_proposalVote>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success: permkey=" << f->perm_key_.to_hex() << "\n";
  TRY_STATUS(td::write_file(fname_, f->to_send_.as_slice()));
  return td::Status::OK();
}

td::Status CreateComplaintVoteQuery::run() {
  TRY_RESULT_ASSIGN(election_id_, tokenizer_.get_token<td::uint32>());
  TRY_RESULT_ASSIGN(data_, tokenizer_.get_token<std::string>());
  TRY_RESULT_ASSIGN(fname_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status CreateComplaintVoteQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_createComplaintVote>(election_id_,
                                                                                               td::BufferSlice(data_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status CreateComplaintVoteQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_proposalVote>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success: permkey=" << f->perm_key_.to_hex() << "\n";
  TRY_STATUS(td::write_file(fname_, f->to_send_.as_slice()));
  return td::Status::OK();
}

td::Status CheckDhtServersQuery::run() {
  TRY_RESULT_ASSIGN(id_, tokenizer_.get_token<ton::PublicKeyHash>());
  return td::Status::OK();
}

td::Status CheckDhtServersQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_checkDhtServers>(id_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status CheckDhtServersQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_dhtServersStatus>(data.as_slice(), true),
                    "received incorrect answer: ");
  for (auto &s : f->servers_) {
    td::TerminalIO::out() << "id=" << s->id_ << " status=" << (s->status_ ? "SUCCESS" : "FAIL") << "\n";
  }
  return td::Status::OK();
}

td::Status SignCertificateQuery::run() {
  TRY_RESULT_ASSIGN(overlay_, tokenizer_.get_token<td::Bits256>());
  TRY_RESULT_ASSIGN(id_, tokenizer_.get_token<td::Bits256>());
  TRY_RESULT_ASSIGN(expire_at_, tokenizer_.get_token<td::int32>());
  TRY_RESULT_ASSIGN(max_size_, tokenizer_.get_token<td::uint32>());
  TRY_RESULT_ASSIGN(signer_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(out_file_, tokenizer_.get_token<std::string>());
  return td::Status::OK();
}

td::Status SignCertificateQuery::send() {
  auto cid = ton::create_serialize_tl_object<ton::ton_api::overlay_certificateId>(overlay_, id_, expire_at_, max_size_);
  auto sign = ton::create_serialize_tl_object<ton::ton_api::engine_validator_sign>(signer_.tl(), std::move(cid));
  auto pub = ton::create_serialize_tl_object<ton::ton_api::engine_validator_exportPublicKey>(signer_.tl());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(pub),
      td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &SignCertificateQuery::handle_error, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &SignCertificateQuery::receive_pubkey, R.move_as_ok());
        }
      }));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(sign),
      td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &SignCertificateQuery::handle_error, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &SignCertificateQuery::receive_signature, R.move_as_ok());
        }
      }));
  return td::Status::OK();
}

void SignCertificateQuery::receive_pubkey(td::BufferSlice R) {
  auto f = ton::fetch_tl_object<ton::ton_api::PublicKey>(R.as_slice(), true);
  if (f.is_error()) {
    handle_error(f.move_as_error_prefix("Failed to get pubkey: "));
    return;
  }
  pubkey_ = f.move_as_ok();
  has_pubkey_ = true;
  if(has_signature_) {
    save_certificate();
  }
}


td::Status SignCertificateQuery::receive(td::BufferSlice data) {
  UNREACHABLE();
}

void SignCertificateQuery::receive_signature(td::BufferSlice R) {
  auto f = ton::fetch_tl_object<ton::ton_api::engine_validator_signature>(R.as_slice(), true);
  if(f.is_error()){
    handle_error(f.move_as_error_prefix("Failed to get signature: "));
    return;
  }
  signature_ = std::move(f.move_as_ok()->signature_);
  if(has_pubkey_) {
    save_certificate();
  }
}

void SignCertificateQuery::save_certificate() {
  auto c = ton::create_serialize_tl_object<ton::ton_api::overlay_certificate>(
        std::move(pubkey_), expire_at_, max_size_, std::move(signature_));
  auto w = td::write_file(out_file_, c.as_slice());
  if(w.is_error()) {
    handle_error(w.move_as_error_prefix("Failed to write certificate to file: "));
    return;
  }
  td::TerminalIO::out() << "saved certificate\n";
  stop();
}

td::Status ImportCertificateQuery::run() {
  TRY_RESULT_ASSIGN(overlay_, tokenizer_.get_token<td::Bits256>());
  TRY_RESULT_ASSIGN(id_, tokenizer_.get_token<td::Bits256>());
  TRY_RESULT_ASSIGN(kh_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(in_file_, tokenizer_.get_token<std::string>());
  return td::Status::OK();
}

td::Status ImportCertificateQuery::send() {
  TRY_RESULT(data, td::read_file(in_file_));
  TRY_RESULT_PREFIX(cert, ton::fetch_tl_object<ton::ton_api::overlay_Certificate>(data.as_slice(), true),
                    "incorrect certificate");
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_importCertificate>(
                overlay_,
                ton::create_tl_object<ton::ton_api::adnl_id_short>(id_),
                ton::create_tl_object<ton::ton_api::engine_validator_keyHash>(kh_.tl()),
                std::move(cert)
           );
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}
td::Status GetOverlaysStatsQuery::run() {
  return td::Status::OK();
}

td::Status GetOverlaysStatsQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getOverlaysStats>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetOverlaysStatsQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_overlaysStats>(data.as_slice(), true),
                    "received incorrect answer: ");
  for (auto &s : f->overlays_) {
    td::StringBuilder sb;
    sb << "overlay_id: " << s->overlay_id_ << " adnl_id: " << s->adnl_id_ << " scope: " << s->scope_ << "\n";
    sb << "  nodes:\n";

    auto print_traffic = [&](const char *name, const char *indent,
                             ton::tl_object_ptr<ton::ton_api::engine_validator_overlayStatsTraffic> &t) {
      sb << indent << name << ":\n"
         << indent << " out: " << t->t_out_bytes_ << " bytes/sec, " << t->t_out_pckts_ << " pckts/sec\n"
         << indent << " in: " << t->t_in_bytes_ << " bytes/sec, " << t->t_in_pckts_ << " pckts/sec\n";
    };
    for (auto &n : s->nodes_) {
      sb << "   adnl_id: " << n->adnl_id_ << " ip_addr: " << n->ip_addr_ << " broadcast_errors: " << n->bdcst_errors_
         << " fec_broadcast_errors: " << n->fec_bdcst_errors_ << " last_in_query: " << n->last_in_query_ << " ("
         << time_to_human(n->last_in_query_) << ")"
         << " last_out_query: " << n->last_out_query_ << " (" << time_to_human(n->last_out_query_) << ")"
         << "\n";
      sb << "   is_neighbour: " << n->is_neighbour_ << "  is_alive: " << n->is_alive_
         << "  node_flags: " << n->node_flags_ << "\n";
      if (n->last_ping_time_ >= 0.0) {
        sb << "   last_ping_at: " << (td::uint32)n->last_ping_at_ << " (" << time_to_human((td::uint32)n->last_ping_at_)
           << ")  last_ping_time: " << n->last_ping_time_ << "\n";
      }
      print_traffic("throughput", "   ", n->traffic_);
      print_traffic("throughput (responses only)", "   ", n->traffic_responses_);
    }
    print_traffic("total_throughput", "  ", s->total_traffic_);
    print_traffic("total_throughput (responses only)", "  ", s->total_traffic_responses_);

    sb << "  stats:\n";
    for (auto &t : s->stats_) {
      sb << "    " << t->key_ << "\t" << t->value_ << "\n";
    }
    td::TerminalIO::output(sb.as_cslice());
  }
  return td::Status::OK();
}

td::Status GetOverlaysStatsJsonQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetOverlaysStatsJsonQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getOverlaysStats>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetOverlaysStatsJsonQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_overlaysStats>(data.as_slice(), true),
                    "received incorrect answer: ");
  std::ofstream sb(file_name_);

  sb << "[\n";
  bool rtail = false;
  for (auto &s : f->overlays_) {
    if (rtail) {
      sb << ",\n";
    } else {
      rtail = true;
    }

    sb << "{\n  \"overlay_id\": \"" << s->overlay_id_ << "\",\n  \"adnl_id\": \"" << s->adnl_id_
       << "\",\n  \"scope\": " << s->scope_ << ",\n";
    sb << "  \"nodes\": [\n";

    auto print_traffic = [&](const char *name,
                             ton::tl_object_ptr<ton::ton_api::engine_validator_overlayStatsTraffic> &t) {
      sb << "\"" << name << "\": { \"out_bytes_sec\": " << t->t_out_bytes_ << ", \"out_pckts_sec\": " << t->t_out_pckts_
         << ", \"in_bytes_sec\": " << t->t_in_bytes_ << ", \"in_pckts_sec\": " << t->t_in_pckts_ << " }";
    };

    bool tail = false;
    for (auto &n : s->nodes_) {
      if (tail) {
        sb << ",\n";
      } else {
        tail = true;
      }

      sb << "   {\n    \"adnl_id\": \"" << n->adnl_id_ << "\",\n    \"ip_addr\": \"" << n->ip_addr_
         << "\",\n    \"broadcast_errors\": " << n->bdcst_errors_
         << ",\n    \"fec_broadcast_errors\": " << n->fec_bdcst_errors_
         << ",\n    \"last_in_query_unix\": " << n->last_in_query_ << ",\n    \"last_in_query_human\": \""
         << time_to_human(n->last_in_query_) << "\",\n"
         << "    \"last_out_query_unix\": " << n->last_out_query_ << ",\n    \"last_out_query_human\": \""
         << time_to_human(n->last_out_query_) << "\",\n";
      if (n->last_ping_time_ >= 0.0) {
        sb << "    \"last_ping_at\": " << (td::uint32)n->last_ping_at_ << ", \"last_ping_at_human\": \""
           << time_to_human((td::uint32)n->last_ping_at_) << "\", \"last_ping_time\": " << n->last_ping_time_ << ",\n";
      }
      sb << "\n    ";
      print_traffic("throughput", n->traffic_);
      sb << ",\n    ";
      print_traffic("throughput_responses", n->traffic_responses_);
      sb << "\n   }";
    }
    sb << "  ],\n  ";

    print_traffic("total_throughput", s->total_traffic_);
    sb << ",\n  ";
    print_traffic("total_throughput_responses", s->total_traffic_responses_);
    sb << ",\n";

    sb << "  \"stats\": {\n";

    tail = false;
    for (auto &t : s->stats_) {
      if (tail) {
        sb << ",\n";
      } else {
        tail = true;
      }

      sb << "   \"" << t->key_ << "\": \"" << t->value_ << "\"";
    }
    sb << "\n  }";
    if (!s->extra_.empty()) {
      sb << ",\n  \"extra\": ";
      for (char c : s->extra_) {
        if (c == '\n') {
          sb << "\n  ";
        } else {
          sb << c;
        }
      }
    }
    sb << "\n}\n";
  }
  sb << "]\n";
  sb << std::flush;

  td::TerminalIO::output(std::string("wrote stats to " + file_name_ + "\n"));
  return td::Status::OK();
}


td::Status ImportCertificateQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "successfully sent certificate to overlay manager\n";
  return td::Status::OK();
}


td::Status SignShardOverlayCertificateQuery::run() {
  TRY_RESULT_ASSIGN(shard_, tokenizer_.get_token<ton::ShardIdFull>() );
  TRY_RESULT_ASSIGN(key_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(expire_at_, tokenizer_.get_token<td::int32>());
  TRY_RESULT_ASSIGN(max_size_, tokenizer_.get_token<td::uint32>());
  TRY_RESULT_ASSIGN(out_file_, tokenizer_.get_token<std::string>());

  return td::Status::OK();
}

td::Status SignShardOverlayCertificateQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_signShardOverlayCertificate>(
      shard_.workchain, shard_.shard, ton::create_tl_object<ton::ton_api::engine_validator_keyHash>(key_.tl()),
      expire_at_, max_size_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SignShardOverlayCertificateQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(c, ton::fetch_tl_object<ton::ton_api::overlay_certificate>(data.as_slice(), true),
                    "received incorrect cert: ");
  auto w = td::write_file(out_file_, data.as_slice());
  if(w.is_error()) {
    return w.move_as_error_prefix("Failed to write certificate to file: ");
  }
  td::TerminalIO::out() << "saved certificate\n";

  return td::Status::OK();
}

td::Status ImportShardOverlayCertificateQuery::run() {
  TRY_RESULT_ASSIGN(shard_, tokenizer_.get_token<ton::ShardIdFull>());
  TRY_RESULT_ASSIGN(key_, tokenizer_.get_token<ton::PublicKeyHash>());
  TRY_RESULT_ASSIGN(in_file_, tokenizer_.get_token<std::string>());

  return td::Status::OK();
}

td::Status ImportShardOverlayCertificateQuery::send() {
  TRY_RESULT(data, td::read_file(in_file_));
  TRY_RESULT_PREFIX(cert, ton::fetch_tl_object<ton::ton_api::overlay_Certificate>(data.as_slice(), true),
                    "incorrect certificate");
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_importShardOverlayCertificate>(
      shard_.workchain, shard_.shard, ton::create_tl_object<ton::ton_api::engine_validator_keyHash>(key_.tl()),
      std::move(cert));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ImportShardOverlayCertificateQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "successfully sent certificate to overlay manager\n";
  return td::Status::OK();
}
td::Status GetActorStatsQuery::run() {
 auto r_file_name = tokenizer_.get_token<std::string>();
 if (r_file_name.is_ok()) {
    file_name_ = r_file_name.move_as_ok();
 }
 return td::Status::OK();
}
td::Status GetActorStatsQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getActorTextStats>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetActorStatsQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_textStats>(data.as_slice(), true),
                    "received incorrect answer: ");
  if (file_name_.empty()) {
    td::TerminalIO::out() << f->data_;
  } else {
    std::ofstream sb(file_name_);
    sb << f->data_;
    sb << std::flush;
    td::TerminalIO::output(std::string("wrote stats to " + file_name_ + "\n"));
  }
  return td::Status::OK();
}

td::Status GetPerfTimerStatsJsonQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetPerfTimerStatsJsonQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getPerfTimerStats>("");
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetPerfTimerStatsJsonQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_perfTimerStats>(data.as_slice(), true),
                    "received incorrect answer: ");
  std::ofstream sb(file_name_);

  sb << "{";
  bool gtail = false;
  for (const auto &v : f->stats_) {
    if (gtail) {
      sb << ",";
    } else {
      gtail = true;
    }

    sb << "\n \"" << v->name_ << "\": {";
    bool tail = false;
    for (const auto &stat : v->stats_) {
      if (tail) {
        sb << ",";
      } else {
        tail = true;
      }

      sb << "\n  \"" << stat->time_ << "\": [";
      sb << "\n   " << stat->min_ << ",";
      sb << "\n   " << stat->avg_ << ",";
      sb << "\n   " << stat->max_;
      sb << "\n  ]";
    }
    sb << "\n }";
  }
  sb << "\n}\n";
  sb << std::flush;

  td::TerminalIO::output(std::string("wrote stats to " + file_name_ + "\n"));
  return td::Status::OK();
}

td::Status GetShardOutQueueSizeQuery::run() {
  TRY_RESULT(shard, tokenizer_.get_token<ton::ShardIdFull>());
  block_id_.workchain = shard.workchain;
  block_id_.shard = shard.shard;
  TRY_RESULT_ASSIGN(block_id_.seqno, tokenizer_.get_token<int>());
  if (!tokenizer_.endl()) {
    TRY_RESULT_ASSIGN(dest_, tokenizer_.get_token<ton::ShardIdFull>());
  }
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetShardOutQueueSizeQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_getShardOutQueueSize>(
      dest_.is_valid() ? 1 : 0, ton::create_tl_block_id_simple(block_id_), dest_.workchain, dest_.shard);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetShardOutQueueSizeQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_shardOutQueueSize>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "Queue_size: " << f->size_ << "\n";
  return td::Status::OK();
}

td::Status SetExtMessagesBroadcastDisabledQuery::run() {
  TRY_RESULT(x, tokenizer_.get_token<int>());
  if (x < 0 || x > 1) {
    return td::Status::Error("value should be 0 or 1");
  }
  value = x;
  return td::Status::OK();
}

td::Status SetExtMessagesBroadcastDisabledQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_setExtMessagesBroadcastDisabled>(value);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SetExtMessagesBroadcastDisabledQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status AddCustomOverlayQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddCustomOverlayQuery::send() {
  TRY_RESULT(data, td::read_file(file_name_));
  TRY_RESULT(json, td::json_decode(data.as_slice()));
  auto overlay = ton::create_tl_object<ton::ton_api::engine_validator_customOverlay>();
  TRY_STATUS(ton::ton_api::from_json(*overlay, json.get_object()));
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addCustomOverlay>(std::move(overlay));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddCustomOverlayQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status DelCustomOverlayQuery::run() {
  TRY_RESULT_ASSIGN(name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelCustomOverlayQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delCustomOverlay>(name_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelCustomOverlayQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status ShowCustomOverlaysQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status ShowCustomOverlaysQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_showCustomOverlays>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ShowCustomOverlaysQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_customOverlaysConfig>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << f->overlays_.size() << " custom overlays:\n\n";
  for (const auto &overlay : f->overlays_) {
    td::TerminalIO::out() << "Overlay \"" << overlay->name_ << "\": " << overlay->nodes_.size() << " nodes\n";
    for (const auto &node : overlay->nodes_) {
      td::TerminalIO::out() << "  " << node->adnl_id_
                            << (node->msg_sender_
                                    ? (PSTRING() << " (msg sender, p=" << node->msg_sender_priority_ << ")")
                                    : "")
                            << (node->block_sender_ ? " (block sender)" : "") << "\n";
    }
    if (!overlay->sender_shards_.empty()) {
      td::TerminalIO::out() << "Sender shards:\n";
      for (const auto &shard : overlay->sender_shards_) {
        td::TerminalIO::out() << "  " << ton::create_shard_id(shard).to_str() << "\n";
      }
    }
    td::TerminalIO::out() << "\n";
  }
  return td::Status::OK();
}

td::Status SetStateSerializerEnabledQuery::run() {
  TRY_RESULT(value, tokenizer_.get_token<int>());
  if (value != 0 && value != 1) {
    return td::Status::Error("expected 0 or 1");
  }
  TRY_STATUS(tokenizer_.check_endl());
  enabled_ = value;
  return td::Status::OK();
}

td::Status SetStateSerializerEnabledQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_setStateSerializerEnabled>(enabled_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SetStateSerializerEnabledQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status SetCollatorOptionsJsonQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status SetCollatorOptionsJsonQuery::send() {
  TRY_RESULT(data, td::read_file(file_name_));
  auto b =
      ton::create_serialize_tl_object<ton::ton_api::engine_validator_setCollatorOptionsJson>(data.as_slice().str());
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status SetCollatorOptionsJsonQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status ResetCollatorOptionsQuery::run() {
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status ResetCollatorOptionsQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_setCollatorOptionsJson>("{}");
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status ResetCollatorOptionsQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "success\n";
  return td::Status::OK();
}

td::Status GetCollatorOptionsJsonQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetCollatorOptionsJsonQuery::send() {
  auto b =
      ton::create_serialize_tl_object<ton::ton_api::engine_validator_getCollatorOptionsJson>();
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetCollatorOptionsJsonQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_jsonConfig>(data.as_slice(), true),
                    "received incorrect answer: ");
  TRY_STATUS(td::write_file(file_name_, f->data_));
  td::TerminalIO::out() << "saved config to " << file_name_ << "\n";
  return td::Status::OK();
}

td::Status GetAdnlStatsJsonQuery::run() {
  TRY_RESULT_ASSIGN(file_name_, tokenizer_.get_token<std::string>());
  if (!tokenizer_.endl()) {
    TRY_RESULT(s, tokenizer_.get_token<std::string>());
    if (s == "all") {
      all_ = true;
    } else {
      return td::Status::Error(PSTRING() << "unexpected token " << s);
    }
  }
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetAdnlStatsJsonQuery::send() {
  auto b =
      ton::create_serialize_tl_object<ton::ton_api::engine_validator_getAdnlStats>(all_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetAdnlStatsJsonQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::adnl_stats>(data.as_slice(), true),
                    "received incorrect answer: ");
  auto s = td::json_encode<std::string>(td::ToJson(*f), true);
  TRY_STATUS(td::write_file(file_name_, s));
  td::TerminalIO::out() << "saved adnl stats to " << file_name_ << "\n";
  return td::Status::OK();
}

td::Status GetAdnlStatsQuery::run() {
  if (!tokenizer_.endl()) {
    TRY_RESULT(s, tokenizer_.get_token<std::string>());
    if (s == "all") {
      all_ = true;
    } else {
      return td::Status::Error(PSTRING() << "unexpected token " << s);
    }
  }
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status GetAdnlStatsQuery::send() {
  auto b =
      ton::create_serialize_tl_object<ton::ton_api::engine_validator_getAdnlStats>(all_);
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status GetAdnlStatsQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(stats, ton::fetch_tl_object<ton::ton_api::adnl_stats>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::StringBuilder sb;
  sb << "================================= ADNL STATS =================================\n";
  bool first = true;
  double now = td::Clocks::system();
  for (auto &local_id : stats->local_ids_) {
    if (first) {
      first = false;
    } else {
      sb << "\n";
    }
    sb << "LOCAL ID " << local_id->short_id_ << "\n";
    if (!local_id->current_decrypt_.empty()) {
      std::sort(local_id->current_decrypt_.begin(), local_id->current_decrypt_.end(),
                [](const ton::tl_object_ptr<ton::ton_api::adnl_stats_ipPackets> &a,
                   const ton::tl_object_ptr<ton::ton_api::adnl_stats_ipPackets> &b) {
                  return a->packets_ > b->packets_;
                });
      td::uint64 total = 0;
      for (auto &x : local_id->current_decrypt_) {
        total += x->packets_;
      }
      sb << "  Packets in decryptor: total=" << total;
      for (auto &x : local_id->current_decrypt_) {
        sb << " " << (x->ip_str_.empty() ? "unknown" : x->ip_str_) << "=" << x->packets_;
      }
      sb << "\n";
    }
    auto print_local_id_packets = [&](const std::string &name,
                                      std::vector<ton::tl_object_ptr<ton::ton_api::adnl_stats_ipPackets>> &vec) {
      if (vec.empty()) {
        return;
      }
      std::sort(vec.begin(), vec.end(),
                [](const ton::tl_object_ptr<ton::ton_api::adnl_stats_ipPackets> &a,
                   const ton::tl_object_ptr<ton::ton_api::adnl_stats_ipPackets> &b) {
                  return a->packets_ > b->packets_;
                });
      td::uint64 total = 0;
      for (auto &x : vec) {
        total += x->packets_;
      }
      sb << "  " << name << ": total=" << total;
      int cnt = 0;
      for (auto &x : vec) {
        ++cnt;
        if (cnt >= 8) {
          sb << " ...";
          break;
        }
        sb << " " << (x->ip_str_.empty() ? "unknown" : x->ip_str_) << "=" << x->packets_;
      }
      sb << "\n";
    };
    print_local_id_packets("Decrypted packets (recent)", local_id->packets_recent_->decrypted_packets_);
    print_local_id_packets("Dropped packets   (recent)", local_id->packets_recent_->dropped_packets_);
    print_local_id_packets("Decrypted packets (total)", local_id->packets_total_->decrypted_packets_);
    print_local_id_packets("Dropped packets   (total)", local_id->packets_total_->dropped_packets_);
    sb << "  PEERS (" << local_id->peers_.size() << "):\n";
    std::sort(local_id->peers_.begin(), local_id->peers_.end(),
              [](const ton::tl_object_ptr<ton::ton_api::adnl_stats_peerPair> &a,
                 const ton::tl_object_ptr<ton::ton_api::adnl_stats_peerPair> &b) {
                return a->packets_recent_->in_bytes_ + a->packets_recent_->out_bytes_ >
                       b->packets_recent_->in_bytes_ + b->packets_recent_->out_bytes_;
              });
    for (auto &peer : local_id->peers_) {
      sb << "    PEER " << peer->peer_id_ << "\n";
      sb << "      Address: " << (peer->ip_str_.empty() ? "unknown" : peer->ip_str_) << "\n";
      sb << "      Connection " << (peer->connection_ready_ ? "ready" : "not ready") << ", ";
      switch (peer->channel_status_) {
        case 0:
          sb << "channel: none\n";
          break;
        case 1:
          sb << "channel: inited\n";
          break;
        case 2:
          sb << "channel: ready\n";
          break;
        default:
          sb << "\n";
      }

      auto print_packets = [&](const std::string &name,
                               const ton::tl_object_ptr<ton::ton_api::adnl_stats_packets> &obj) {
        if (obj->in_packets_) {
          sb << "      In  (" << name << "): " << obj->in_packets_ << " packets ("
             << td::format::as_size(obj->in_bytes_) << "), channel: " << obj->in_packets_channel_ << " packets ("
             << td::format::as_size(obj->in_bytes_channel_) << ")\n";
        }
        if (obj->out_packets_) {
          sb << "      Out (" << name << "): " << obj->out_packets_ << " packets ("
             << td::format::as_size(obj->out_bytes_) << "), channel: " << obj->out_packets_channel_ << " packets ("
             << td::format::as_size(obj->out_bytes_channel_) << ")\n";
        }
        if (obj->out_expired_messages_) {
          sb << "      Out expired (" << name << "): " << obj->out_expired_messages_ << " messages ("
             << td::format::as_size(obj->out_expired_bytes_) << ")\n";
        }
      };
      print_packets("recent", peer->packets_recent_);
      print_packets("total", peer->packets_total_);

      sb << "      Last in packet: ";
      if (peer->last_in_packet_ts_) {
        sb << now - peer->last_in_packet_ts_ << " s ago";
      } else {
        sb << "never";
      }
      sb << "    Last out packet: ";
      if (peer->last_out_packet_ts_) {
        sb << now - peer->last_out_packet_ts_ << " s ago";
      } else {
        sb << "never";
      }
      sb << "\n";
      if (peer->out_queue_messages_) {
        sb << "      Out message queue: " << peer->out_queue_messages_ << " messages ("
           << td::format::as_size(peer->out_queue_bytes_) << ")\n";
      }
    }
  }
  sb << "==============================================================================\n";
  td::TerminalIO::out() << sb.as_cslice();
  return td::Status::OK();
}

td::Status AddShardQuery::run() {
  TRY_RESULT_ASSIGN(shard_, tokenizer_.get_token<ton::ShardIdFull>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status AddShardQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_addShard>(ton::create_tl_shard_id(shard_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status AddShardQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "successfully added shard\n";
  return td::Status::OK();
}

td::Status DelShardQuery::run() {
  TRY_RESULT_ASSIGN(shard_, tokenizer_.get_token<ton::ShardIdFull>());
  TRY_STATUS(tokenizer_.check_endl());
  return td::Status::OK();
}

td::Status DelShardQuery::send() {
  auto b = ton::create_serialize_tl_object<ton::ton_api::engine_validator_delShard>(ton::create_tl_shard_id(shard_));
  td::actor::send_closure(console_, &ValidatorEngineConsole::envelope_send_query, std::move(b), create_promise());
  return td::Status::OK();
}

td::Status DelShardQuery::receive(td::BufferSlice data) {
  TRY_RESULT_PREFIX(f, ton::fetch_tl_object<ton::ton_api::engine_validator_success>(data.as_slice(), true),
                    "received incorrect answer: ");
  td::TerminalIO::out() << "successfully removed shard\n";
  return td::Status::OK();
}