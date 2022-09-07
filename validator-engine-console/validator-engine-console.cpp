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
#include "validator-engine-console.h"
#include "adnl/adnl-ext-client.h"
#include "tl-utils/lite-utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "auto/tl/lite_api.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/stacktrace.h"
#include "td/utils/port/StdStreams.h"
#include "td/utils/port/FileFd.h"
#include "terminal/terminal.h"
#include "ton/lite-tl.hpp"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/mc-config.h"
#include "block/check-proof.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "ton/ton-shard.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#include <fcntl.h>
#endif
#include <iostream>
#include <sstream>
#include "git.h"

int verbosity;

std::unique_ptr<ton::adnl::AdnlExtClient::Callback> ValidatorEngineConsole::make_callback() {
  class Callback : public ton::adnl::AdnlExtClient::Callback {
   public:
    void on_ready() override {
      td::actor::send_closure(id_, &ValidatorEngineConsole::conn_ready);
    }
    void on_stop_ready() override {
      td::actor::send_closure(id_, &ValidatorEngineConsole::conn_closed);
    }
    Callback(td::actor::ActorId<ValidatorEngineConsole> id) : id_(std::move(id)) {
    }

   private:
    td::actor::ActorId<ValidatorEngineConsole> id_;
  };
  return std::make_unique<Callback>(actor_id(this));
}

void ValidatorEngineConsole::run() {
  class Cb : public td::TerminalIO::Callback {
   public:
    void line_cb(td::BufferSlice line) override {
      td::actor::send_closure(id_, &ValidatorEngineConsole::parse_line, std::move(line));
    }
    Cb(td::actor::ActorId<ValidatorEngineConsole> id) : id_(id) {
    }

   private:
    td::actor::ActorId<ValidatorEngineConsole> id_;
  };
  io_ = td::TerminalIO::create("> ", readline_enabled_, ex_mode_, std::make_unique<Cb>(actor_id(this)));
  td::actor::send_closure(io_, &td::TerminalIO::set_log_interface);

  td::TerminalIO::out() << "connecting to " << remote_addr_ << "\n";
  td::TerminalIO::out() << "local key: " << private_key_.compute_short_id().bits256_value().to_hex() << "\n";
  td::TerminalIO::out() << "remote key: " << server_public_key_.compute_short_id().bits256_value().to_hex() << "\n";

  client_ = ton::adnl::AdnlExtClient::create(ton::adnl::AdnlNodeIdFull{server_public_key_}, private_key_, remote_addr_,
                                             make_callback());

  add_query_runner(std::make_unique<QueryRunnerImpl<GetTimeQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<GetHelpQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<GetLicenseQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<NewKeyQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<ImportPrivateKeyFileQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<ExportPublicKeyQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<ExportPublicKeyFileQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<SignQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<SignFileQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddAdnlAddrQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddDhtIdQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddValidatorPermanentKeyQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddValidatorTempKeyQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddValidatorAdnlAddrQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<ChangeFullNodeAdnlAddrQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddLiteServerQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<DelAdnlAddrQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<DelDhtIdQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<DelValidatorPermanentKeyQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<DelValidatorTempKeyQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<DelValidatorAdnlAddrQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<GetConfigQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<SetVerbosityQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<GetStatsQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<QuitQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddNetworkAddressQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<AddNetworkProxyAddressQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<CreateElectionBidQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<CreateProposalVoteQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<CreateComplaintVoteQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<CheckDhtServersQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<SignCertificateQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<ImportCertificateQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<GetOverlaysStatsQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<GetOverlaysStatsJsonQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<ImportShardOverlayCertificateQuery>>());
  add_query_runner(std::make_unique<QueryRunnerImpl<SignShardOverlayCertificateQuery>>());
}

bool ValidatorEngineConsole::envelope_send_query(td::BufferSlice query, td::Promise<td::BufferSlice> promise) {
  if (!ready_ || client_.empty()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "failed to send query to server: not ready"));
    return false;
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_error(R.move_as_error());
      return;
    }
    auto data = R.move_as_ok();
    auto F = ton::fetch_tl_object<ton::ton_api::engine_validator_controlQueryError>(data.clone(), true);
    if (F.is_ok()) {
      auto f = F.move_as_ok();
      promise.set_error(td::Status::Error(f->code_, f->message_));
      return;
    }
    promise.set_result(std::move(data));
  });
  td::BufferSlice b = ton::serialize_tl_object(
      ton::create_tl_object<ton::ton_api::engine_validator_controlQuery>(std::move(query)), true);
  td::actor::send_closure(client_, &ton::adnl::AdnlExtClient::send_query, "query", std::move(b),
                          td::Timestamp::in(10.0), std::move(P));
  return true;
}

void ValidatorEngineConsole::got_result() {
  running_queries_--;
  if (!running_queries_ && ex_queries_.size() > 0) {
    auto data = std::move(ex_queries_[0]);
    ex_queries_.erase(ex_queries_.begin());
    parse_line(std::move(data));
  }
  if (ex_mode_ && !running_queries_ && ex_queries_.size() == 0) {
    std::_Exit(0);
  }
}

void ValidatorEngineConsole::show_help(std::string command, td::Promise<td::BufferSlice> promise) {
  if (command.size() == 0) {
    td::TerminalIO::out() << "list of available commands:\n";
    for (auto& cmd : query_runners_) {
      td::TerminalIO::out() << cmd.second->help() << "\n";
    }
  } else {
    auto it = query_runners_.find(command);
    if (it != query_runners_.end()) {
      td::TerminalIO::out() << it->second->help() << "\n";
    } else {
      td::TerminalIO::out() << "unknown command '" << command << "'\n";
    }
  }
  promise.set_value(td::BufferSlice{});
}

void ValidatorEngineConsole::show_license(td::Promise<td::BufferSlice> promise) {
  td::TerminalIO::out() << R"(Copyright (C) 2019 Telegram Systems LLP.
License GPLv2+: GNU GPL version 2 or later <https://www.gnu.org/licenses/gpl-2.0.html>
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.)"
                        << "\n";
  promise.set_value(td::BufferSlice{});
}

void ValidatorEngineConsole::parse_line(td::BufferSlice data) {
  Tokenizer tokenizer(std::move(data));
  if (tokenizer.endl()) {
    return;
  }
  auto name = tokenizer.get_token<std::string>().move_as_ok();

  auto it = query_runners_.find(name);
  if (it != query_runners_.end()) {
    running_queries_++;
    it->second->run(actor_id(this), std::move(tokenizer));
  } else {
    td::TerminalIO::out() << "unknown command '" << name << "'\n";
  }
}

void ValidatorEngineConsole::set_private_key(td::BufferSlice file_name) {
  auto R = [&]() -> td::Result<ton::PrivateKey> {
    TRY_RESULT_PREFIX(conf_data, td::read_file(file_name.as_slice().str()), "failed to read: ");

    return ton::PrivateKey::import(conf_data.as_slice());
  }();

  if (R.is_error()) {
    LOG(FATAL) << "bad private key: " << R.move_as_error();
  }
  private_key_ = R.move_as_ok();
}

void ValidatorEngineConsole::set_public_key(td::BufferSlice file_name) {
  auto R = [&]() -> td::Result<ton::PublicKey> {
    TRY_RESULT_PREFIX(conf_data, td::read_file(file_name.as_slice().str()), "failed to read: ");

    return ton::PublicKey::import(conf_data.as_slice());
  }();

  if (R.is_error()) {
    LOG(FATAL) << "bad server public key: " << R.move_as_error();
  }
  server_public_key_ = R.move_as_ok();
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler();

  td::actor::ActorOwn<ValidatorEngineConsole> x;

  td::OptionParser p;
  p.set_description("console for validator for TON Blockchain");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('V', "version", "shows validator-engine-console build information", [&]() {
    std::cout << "validator-engine-console build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_checked_option('a', "address", "server address", [&](td::Slice arg) {
    td::IPAddress addr;
    TRY_STATUS(addr.init_host_port(arg.str()));
    td::actor::send_closure(x, &ValidatorEngineConsole::set_remote_addr, addr);
    return td::Status::OK();
  });
  p.add_option('k', "key", "private key", [&](td::Slice arg) {
    td::actor::send_closure(x, &ValidatorEngineConsole::set_private_key, td::BufferSlice{arg});
  });
  p.add_option('p', "pub", "server public key", [&](td::Slice arg) {
    td::actor::send_closure(x, &ValidatorEngineConsole::set_public_key, td::BufferSlice{arg});
  });
  p.add_option('r', "disable-readline", "disable readline",
               [&]() { td::actor::send_closure(x, &ValidatorEngineConsole::set_readline_enabled, false); });
  p.add_option('R', "enable-readline", "enable readline",
               [&]() { td::actor::send_closure(x, &ValidatorEngineConsole::set_readline_enabled, true); });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 9) ? td::Status::OK() : td::Status::Error("verbosity must be 0..9");
  });
  p.add_option('c', "cmd", "schedule command", [&](td::Slice arg) {
    td::actor::send_closure(x, &ValidatorEngineConsole::add_cmd, td::BufferSlice{arg});
  });
  p.add_option('t', "timeout", "timeout in batch mode", [&](td::Slice arg) {
    auto d = td::to_double(arg);
    td::actor::send_closure(x, &ValidatorEngineConsole::set_fail_timeout, td::Timestamp::in(d));
  });
  td::actor::Scheduler scheduler({2});

  scheduler.run_in_context([&] {
    x = td::actor::create_actor<ValidatorEngineConsole>("console");
    auto S = p.run(argc, argv);
    if (S.is_error()) {
      std::cerr << S.move_as_error().message().str() << std::endl;
      std::_Exit(2);
    }
    td::actor::send_closure(x, &ValidatorEngineConsole::run);
  });
  scheduler.run();

  return 0;
}
