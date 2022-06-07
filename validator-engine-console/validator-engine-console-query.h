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
#pragma once

#include "td/utils/Status.h"
#include "td/utils/buffer.h"
#include "td/utils/misc.h"
#include "td/utils/SharedSlice.h"
#include "td/utils/port/IPAddress.h"
#include "td/actor/actor.h"

#include "keys/keys.hpp"

class ValidatorEngineConsole;

class Tokenizer {
 public:
  Tokenizer(td::BufferSlice data);

  void skipspc();
  bool endl();
  td::Status check_endl() {
    if (!endl()) {
      return td::Status::Error("extra data after query");
    } else {
      return td::Status::OK();
    }
  }

  td::Result<td::Slice> get_raw_token();
  td::Result<td::Slice> peek_raw_token();

  template <typename T>
  inline td::Result<T> get_token() {
    TRY_RESULT(S, get_raw_token());
    return td::to_integer_safe<T>(S);
  }
  template <typename T>
  inline td::Result<std::vector<T>> get_token_vector();

 private:
  td::BufferSlice data_;
  td::Slice remaining_;
};

template <>
inline td::Result<td::Slice> Tokenizer::get_token() {
  return get_raw_token();
}

template <>
inline td::Result<std::string> Tokenizer::get_token() {
  TRY_RESULT(S, get_raw_token());
  return S.str();
}

template <>
inline td::Result<td::BufferSlice> Tokenizer::get_token() {
  TRY_RESULT(S, get_raw_token());
  TRY_RESULT(F, td::hex_decode(S));
  return td::BufferSlice{F};
}

template <>
inline td::Result<td::SharedSlice> Tokenizer::get_token() {
  TRY_RESULT(S, get_raw_token());
  TRY_RESULT(F, td::hex_decode(S));
  return td::SharedSlice{F};
}

template <>
inline td::Result<ton::PublicKeyHash> Tokenizer::get_token() {
  TRY_RESULT(S, get_raw_token());
  TRY_RESULT(F, td::hex_decode(S));
  if (F.size() == 32) {
    return ton::PublicKeyHash{td::Slice{F}};
  } else {
    return td::Status::Error("cannot parse keyhash: bad length");
  }
}

template <>
inline td::Result<td::Bits256> Tokenizer::get_token() {
  TRY_RESULT(S, get_raw_token());
  TRY_RESULT(F, td::hex_decode(S));
  if (F.size() == 32) {
    td::Bits256 v;
    v.as_slice().copy_from(F);
    return v;
  } else {
    return td::Status::Error("cannot parse keyhash: bad length");
  }
}

template <>
inline td::Result<td::IPAddress> Tokenizer::get_token() {
  TRY_RESULT(S, get_raw_token());
  td::IPAddress addr;
  TRY_STATUS(addr.init_host_port(S.str()));
  return addr;
}

template <typename T>
inline td::Result<std::vector<T>> Tokenizer::get_token_vector() {
  TRY_RESULT(word, get_token<std::string>());
  if (word != "[") {
    return td::Status::Error("'[' expected");
  }

  std::vector<T> res;
  while (true) {
    TRY_RESULT(w, peek_raw_token());

    if (w == "]") {
      get_raw_token();
      return res;
    }
    TRY_RESULT(val, get_token<T>());
    res.push_back(std::move(val));
  }
}

class QueryRunner {
 public:
  virtual ~QueryRunner() = default;
  virtual std::string name() const = 0;
  virtual std::string help() const = 0;
  virtual td::Status run(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer) const = 0;
};

template <class T>
class QueryRunnerImpl : public QueryRunner {
 public:
  std::string name() const override {
    return T::get_name();
  }
  std::string help() const override {
    return T::get_help();
  }
  td::Status run(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer) const override {
    td::actor::create_actor<T>(PSTRING() << "query " << name(), std::move(console), std::move(tokenizer)).release();
    return td::Status::OK();
  }
  QueryRunnerImpl() {
  }
};

class Query : public td::actor::Actor {
 public:
  virtual ~Query() = default;
  Query(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : console_(console), tokenizer_(std::move(tokenizer)) {
  }
  void start_up() override;
  virtual td::Status run() = 0;
  virtual td::Status send() = 0;
  void receive_wrap(td::BufferSlice R);
  virtual td::Status receive(td::BufferSlice R) = 0;
  auto create_promise() {
    return td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &Query::handle_error, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &Query::receive_wrap, R.move_as_ok());
      }
    });
  }
  virtual std::string name() const = 0;
  void handle_error(td::Status error);

 protected:
  td::actor::ActorId<ValidatorEngineConsole> console_;
  Tokenizer tokenizer_;
};

class GetTimeQuery : public Query {
 public:
  GetTimeQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "gettime";
  }
  static std::string get_help() {
    return "gettime\tshows current server unixtime";
  }
  std::string name() const override {
    return get_name();
  }

 private:
};

class GetHelpQuery : public Query {
 public:
  GetHelpQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "help";
  }
  static std::string get_help() {
    return "help [command]\tshows help";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  std::string command_;
};

class GetLicenseQuery : public Query {
 public:
  GetLicenseQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "license";
  }
  static std::string get_help() {
    return "license\tshows license info";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  std::string command_;
};

class NewKeyQuery : public Query {
 public:
  NewKeyQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "newkey";
  }
  static std::string get_help() {
    return "newkey\tgenerates new key pair on server";
  }
  std::string name() const override {
    return get_name();
  }

 private:
};

class ImportPrivateKeyFileQuery : public Query {
 public:
  ImportPrivateKeyFileQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "importf";
  }
  static std::string get_help() {
    return "importf <filename>\timport private key";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  std::string file_name_;
};

class ExportPublicKeyQuery : public Query {
 public:
  ExportPublicKeyQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "exportpub";
  }
  static std::string get_help() {
    return "exportpub <keyhash>\texports public key by key hash";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
};

class ExportPublicKeyFileQuery : public Query {
 public:
  ExportPublicKeyFileQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice R) override;
  static std::string get_name() {
    return "exportpubf";
  }
  static std::string get_help() {
    return "exportpubf <keyhash> <filename>\texports public key by key hash";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
  std::string file_name_;
};

class SignQuery : public Query {
 public:
  SignQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "sign";
  }
  static std::string get_help() {
    return "sign <keyhash> <data>\tsigns bytestring with privkey";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
  td::BufferSlice data_;
};

class SignFileQuery : public Query {
 public:
  SignFileQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "signf";
  }
  static std::string get_help() {
    return "signf <keyhash> <infile> <outfile>\tsigns bytestring with privkey";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
  std::string in_file_;
  std::string out_file_;
};

class AddAdnlAddrQuery : public Query {
 public:
  AddAdnlAddrQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addadnl";
  }
  static std::string get_help() {
    return "addadnl <keyhash> <category>\tuse key as ADNL addr";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
  td::uint32 category_;
};

class AddDhtIdQuery : public Query {
 public:
  AddDhtIdQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "adddht";
  }
  static std::string get_help() {
    return "adddht <keyhash>\tcreate DHT node with specified ADNL addr";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
};

class AddValidatorPermanentKeyQuery : public Query {
 public:
  AddValidatorPermanentKeyQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addpermkey";
  }
  static std::string get_help() {
    return "addpermkey <keyhash> <election-date> <expire-at>\tadd validator permanent key";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
  td::uint32 election_date_;
  td::uint32 expire_at_;
};

class AddValidatorTempKeyQuery : public Query {
 public:
  AddValidatorTempKeyQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addtempkey";
  }
  static std::string get_help() {
    return "addtempkey <permkeyhash> <keyhash> <expireat>\tadd validator temp key";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash perm_key_hash_;
  ton::PublicKeyHash key_hash_;
  td::uint32 expire_at_;
};

class AddValidatorAdnlAddrQuery : public Query {
 public:
  AddValidatorAdnlAddrQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addvalidatoraddr";
  }
  static std::string get_help() {
    return "addvalidatoraddr <permkeyhash> <keyhash> <expireat>\tadd validator ADNL addr";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash perm_key_hash_;
  ton::PublicKeyHash key_hash_;
  td::uint32 expire_at_;
};

class ChangeFullNodeAdnlAddrQuery : public Query {
 public:
  ChangeFullNodeAdnlAddrQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "changefullnodeaddr";
  }
  static std::string get_help() {
    return "changefullnodeaddr <keyhash>\tchanges fullnode ADNL address";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
};

class AddLiteServerQuery : public Query {
 public:
  AddLiteServerQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addliteserver";
  }
  static std::string get_help() {
    return "addliteserver <port> <keyhash>\tadd liteserver";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::uint16 port_;
  ton::PublicKeyHash key_hash_;
};

class DelAdnlAddrQuery : public Query {
 public:
  DelAdnlAddrQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "deladnl";
  }
  static std::string get_help() {
    return "deladnl <keyhash>\tdel unused ADNL addr";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
};

class DelDhtIdQuery : public Query {
 public:
  DelDhtIdQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "deldht";
  }
  static std::string get_help() {
    return "deldht <keyhash>\tdel unused DHT node";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
};

class DelValidatorPermanentKeyQuery : public Query {
 public:
  DelValidatorPermanentKeyQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "delpermkey";
  }
  static std::string get_help() {
    return "delpermkey <keyhash>\tforce del unused validator permanent key";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash key_hash_;
};

class DelValidatorTempKeyQuery : public Query {
 public:
  DelValidatorTempKeyQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "deltempkey";
  }
  static std::string get_help() {
    return "deltempkey <permkeyhash> <keyhash>\tforce del unused validator temp key";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash perm_key_hash_;
  ton::PublicKeyHash key_hash_;
};

class DelValidatorAdnlAddrQuery : public Query {
 public:
  DelValidatorAdnlAddrQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "delvalidatoraddr";
  }
  static std::string get_help() {
    return "delvalidatoraddr <permkeyhash> <keyhash>\tforce del unused validator ADNL addr";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash perm_key_hash_;
  ton::PublicKeyHash key_hash_;
};

class GetConfigQuery : public Query {
 public:
  GetConfigQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "getconfig";
  }
  static std::string get_help() {
    return "getconfig\tdownloads current config";
  }
  std::string name() const override {
    return get_name();
  }

 private:
};

class SetVerbosityQuery : public Query {
 public:
  SetVerbosityQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "setverbosity";
  }
  static std::string get_help() {
    return "setverbosity <value>\tchanges verbosity level";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::uint32 verbosity_;
};

class GetStatsQuery : public Query {
 public:
  GetStatsQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "getstats";
  }
  static std::string get_help() {
    return "getstats\tprints stats";
  }
  std::string name() const override {
    return get_name();
  }
};

class QuitQuery : public Query {
 public:
  QuitQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override {
    return tokenizer_.check_endl();
  }
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override {
    UNREACHABLE();
  }
  static std::string get_name() {
    return "quit";
  }
  static std::string get_help() {
    return "quit\tcloses client";
  }
  std::string name() const override {
    return get_name();
  }

 private:
};

class AddNetworkAddressQuery : public Query {
 public:
  AddNetworkAddressQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addaddr";
  }
  static std::string get_help() {
    return "addaddr <ip> {cats...} {priocats...}\tadds ip address to address list";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::IPAddress addr_;
  std::vector<td::int32> cats_;
  std::vector<td::int32> prio_cats_;
};

class AddNetworkProxyAddressQuery : public Query {
 public:
  AddNetworkProxyAddressQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "addproxyaddr";
  }
  static std::string get_help() {
    return "addproxyaddr <inip> <outip> <id> <secret> {cats...} {priocats...}\tadds ip address to address list";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::IPAddress in_addr_;
  td::IPAddress out_addr_;
  td::Bits256 id_;
  td::BufferSlice shared_secret_;
  std::vector<td::int32> cats_;
  std::vector<td::int32> prio_cats_;
};

class CreateElectionBidQuery : public Query {
 public:
  CreateElectionBidQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "createelectionbid";
  }
  static std::string get_help() {
    return "createelectionbid <date> <elector> <wallet> <fname>\tcreate election bid";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::uint32 date_;
  std::string elector_addr_;
  std::string wallet_;
  std::string fname_;
};

class CreateProposalVoteQuery : public Query {
 public:
  CreateProposalVoteQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "createproposalvote";
  }
  static std::string get_help() {
    return "createproposalvote <data> <fname>\tcreate proposal vote";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  std::string data_;
  std::string fname_;
};

class CreateComplaintVoteQuery : public Query {
 public:
  CreateComplaintVoteQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "createcomplaintvote";
  }
  static std::string get_help() {
    return "createcomplaintvote <election-id> <data> <fname>\tcreate proposal vote";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::uint32 election_id_;
  std::string data_;
  std::string fname_;
};

class CheckDhtServersQuery : public Query {
 public:
  CheckDhtServersQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "checkdht";
  }
  static std::string get_help() {
    return "checkdht <adnlid>\tchecks, which root DHT servers are accessible from this ADNL addr";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  ton::PublicKeyHash id_;
};

class GetOverlaysStatsQuery : public Query {
 public:
  GetOverlaysStatsQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "getoverlaysstats";
  }
  static std::string get_help() {
    return "getoverlaysstats\tgets stats for all overlays";
  }
  static std::string time_to_human(int unixtime) {
    char time_buffer[80];
    time_t rawtime = unixtime;
    struct tm tInfo;
    struct tm* timeinfo = localtime_r(&rawtime, &tInfo);
    assert(timeinfo == &tInfo);
    strftime(time_buffer, 80, "%c", timeinfo);
    return std::string(time_buffer);
  }
  std::string name() const override {
    return get_name();
  }
};

class GetOverlaysStatsJsonQuery : public Query {
 public:
  GetOverlaysStatsJsonQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "getoverlaysstatsjson";
  }
  static std::string get_help() {
    return "getoverlaysstatsjson <outfile>\tgets stats for all overlays and writes to json file";
  }
  static std::string time_to_human(int unixtime) {
    char time_buffer[80];
    time_t rawtime = unixtime;
    struct tm tInfo;
    struct tm* timeinfo = localtime_r(&rawtime, &tInfo);
    assert(timeinfo == &tInfo);
    strftime(time_buffer, 80, "%c", timeinfo);
    return std::string(time_buffer);
  }
  std::string name() const override {
    return get_name();
  }
  
private:
 std::string file_name_;
};

class SignCertificateQuery : public Query {
 public:
  SignCertificateQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "signcert";
  }
  static std::string get_help() {
    return "signcert <overlayid> <adnlid> <expireat> <maxsize> <signwith> <outfile>\tsign overlay certificate by <signwith> key";
  }
  std::string name() const override {
    return get_name();
  }
  void receive_pubkey(td::BufferSlice R);
  void receive_signature(td::BufferSlice R);


 private:
   void save_certificate();

  td::Bits256 overlay_;
  td::Bits256 id_;
  td::int32 expire_at_;
  td::uint32 max_size_;
  std::string out_file_;
  ton::PublicKeyHash signer_;
  td::BufferSlice signature_;
  std::unique_ptr<ton::ton_api::PublicKey> pubkey_;
  bool has_signature_{0};
  bool has_pubkey_{0};
};

class ImportCertificateQuery : public Query {
 public:
  ImportCertificateQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "importcert";
  }
  static std::string get_help() {
    return "importcert <overlayid> <adnlid> <key> <certfile>\timport overlay certificate for specific key";
  }
  std::string name() const override {
    return get_name();
  }

 private:
  td::Bits256 overlay_;
  td::Bits256 id_;
  ton::PublicKeyHash kh_;
  std::string in_file_;
};

class SignShardOverlayCertificateQuery : public Query {
 public:
  SignShardOverlayCertificateQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "signshardoverlaycert";
  }
  static std::string get_help() {
    return "signshardoverlaycert <workchain> <shardprefix> <key> <expireat> <maxsize> <outfile>\tsign certificate for <key> in currently active shard overlay";
  }
  std::string name() const override {
    return get_name();
  }

 private:

  td::int32 wc_;
  td::int64 shard_;
  td::int32 expire_at_;
  ton::PublicKeyHash key_;
  td::uint32 max_size_;
  std::string out_file_;
};


class ImportShardOverlayCertificateQuery : public Query {
 public:
  ImportShardOverlayCertificateQuery(td::actor::ActorId<ValidatorEngineConsole> console, Tokenizer tokenizer)
      : Query(console, std::move(tokenizer)) {
  }
  td::Status run() override;
  td::Status send() override;
  td::Status receive(td::BufferSlice data) override;
  static std::string get_name() {
    return "importshardoverlaycert";
  }
  static std::string get_help() {
    return "importshardoverlaycert <workchain> <shardprefix> <key> <certfile>\timport certificate for <key> in currently active shard overlay";
  }
  std::string name() const override {
    return get_name();
  }

 private:

  td::int32 wc_;
  td::int64 shard_;
  ton::PublicKeyHash key_;
  std::string in_file_;
};

