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
#include "adnl/adnl-ext-client.h"
#include "tl-utils/tl-utils.hpp"
#include "ton/ton-types.h"
#include "terminal/terminal.h"
#include "vm/cells.h"
#include "validator-engine-console-query.h"

#include <map>

class ValidatorEngineConsole : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::adnl::AdnlExtClient> client_;
  td::actor::ActorOwn<td::TerminalIO> io_;

  bool readline_enabled_ = true;

  td::IPAddress remote_addr_;
  ton::PrivateKey private_key_;
  ton::PublicKey server_public_key_;

  bool ready_ = false;
  bool inited_ = false;

  td::Timestamp fail_timeout_;
  td::uint32 running_queries_ = 0;
  bool ex_mode_ = false;
  std::vector<td::BufferSlice> ex_queries_;

  std::unique_ptr<ton::adnl::AdnlExtClient::Callback> make_callback();

  std::map<std::string, std::unique_ptr<QueryRunner>> query_runners_;
  void add_query_runner(std::unique_ptr<QueryRunner> runner) {
    auto name = runner->name();
    query_runners_[name] = std::move(runner);
  }

 public:
  void conn_ready() {
    td::TerminalIO::out() << "conn ready\n";
    ready_ = true;
    running_queries_++;
    got_result();
  }
  void conn_closed() {
    td::TerminalIO::out() << "conn failed\n";
    ready_ = false;
  }
  void set_readline_enabled(bool value) {
    readline_enabled_ = value;
  }
  void set_remote_addr(td::IPAddress addr) {
    remote_addr_ = addr;
  }
  void set_private_key(td::BufferSlice file_name);
  void set_public_key(td::BufferSlice file_name);

  void add_cmd(td::BufferSlice data) {
    ex_mode_ = true;
    ex_queries_.push_back(std::move(data));
    set_readline_enabled(false);
  }
  void set_fail_timeout(td::Timestamp ts) {
    fail_timeout_ = ts;
    alarm_timestamp().relax(fail_timeout_);
  }
  void alarm() override {
    if (fail_timeout_.is_in_past()) {
      std::_Exit(7);
    }
    if (ex_mode_ && !running_queries_ && ex_queries_.size() == 0) {
      std::_Exit(0);
    }
    alarm_timestamp().relax(fail_timeout_);
  }

  void close() {
    stop();
  }
  void tear_down() override {
    // FIXME: do not work in windows
    //td::actor::SchedulerContext::get()->stop();
    io_.reset();
    std::_Exit(0);
  }

  bool envelope_send_query(td::BufferSlice query, td::Promise<td::BufferSlice> promise);
  void got_result(bool success = true);
  void show_help(std::string command, td::Promise<td::BufferSlice> promise);
  void show_license(td::Promise<td::BufferSlice> promise);

  void parse_line(td::BufferSlice data);

  ValidatorEngineConsole() {
  }

  void run();
};
