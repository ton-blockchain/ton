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
#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "td/utils/port/IPAddress.h"
#include "td/net/UdpServer.h"
#include "td/utils/port/signals.h"
#include "td/utils/OptionsParser.h"
#include "td/utils/FileLog.h"
#include "td/utils/port/path.h"
#include "td/utils/port/user.h"
#include "td/utils/filesystem.h"
#include "common/checksum.h"
#include "common/errorcode.h"
#include "tl-utils/tl-utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "adnl/adnl.h"
#include <map>

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif

namespace ton {

namespace adnl {

class Callback : public adnl::Adnl::Callback {
 public:
  void receive_message(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data) override {
  }
  void receive_query(AdnlNodeIdShort src, AdnlNodeIdShort dst, td::BufferSlice data,
                     td::Promise<td::BufferSlice> promise) override {
    TRY_RESULT_PROMISE_PREFIX(promise, f, fetch_tl_object<ton_api::adnl_ping>(std::move(data), true),
                              "adnl.ping expected");
    promise.set_value(create_serialize_tl_object<ton_api::adnl_pong>(f->value_));
  }

  Callback() {
  }

 private:
};

}  // namespace adnl

}  // namespace ton

std::atomic<bool> rotate_logs_flags{false};
void force_rotate_logs(int sig) {
  rotate_logs_flags.store(true);
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  ton::PrivateKey pk;
  td::IPAddress addr;

  td::set_default_failure_signal_handler().ensure();

  std::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  std::string config = "/var/ton-work/etc/adnl-proxy.conf.json";

  td::OptionsParser p;
  p.set_description("adnl pinger");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
    return td::Status::OK();
  });
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
    return td::Status::OK();
  });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
#if TD_DARWIN || TD_LINUX
    close(0);
    setsid();
#endif
    td::set_signal_handler(td::SignalType::HangUp, force_rotate_logs).ensure();
    return td::Status::OK();
  });
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    auto F = std::make_unique<td::FileLog>();
    TRY_STATUS(F->init(fname.str(), std::numeric_limits<td::uint64>::max(), true));
    logger_ = std::move(F);
    td::log_interface = logger_.get();
    return td::Status::OK();
  });
  td::uint32 threads = 7;
  p.add_option('t', "threads", PSTRING() << "number of threads (default=" << threads << ")", [&](td::Slice fname) {
    td::int32 v;
    try {
      v = std::stoi(fname.str());
    } catch (...) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
    }
    if (v < 1 || v > 256) {
      return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: should be in range [1..256]");
    }
    threads = v;
    return td::Status::OK();
  });
  p.add_option('u', "user", "change user", [&](td::Slice user) { return td::change_user(user); });
  p.add_option('k', "key", "private key", [&](td::Slice key) {
    TRY_RESULT_ASSIGN(pk, ton::PrivateKey::import(key));
    return td::Status::OK();
  });
  p.add_option('a', "addr", "ip:port of instance", [&](td::Slice key) {
    TRY_STATUS(addr.init_host_port(key.str()));
    return td::Status::OK();
  });

  p.run(argc, argv).ensure();

  if (pk.empty()) {
    LOG(FATAL) << "no --key given";
  }
  if (!addr.is_valid()) {
    LOG(FATAL) << "no --addr given";
  }

  td::actor::Scheduler scheduler({threads});

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> network_manager;

  auto pub = pk.compute_public_key();

  scheduler.run_in_context([&]() {
    keyring = ton::keyring::Keyring::create("");
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk), true, [](td::Unit) {});

    adnl = ton::adnl::Adnl::create("", keyring.get());

    network_manager = ton::adnl::AdnlNetworkManager::create(static_cast<td::uint16>(addr.get_port()));

    ton::adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(network_manager, &ton::adnl::AdnlNetworkManager::add_self_addr, addr, std::move(cat_mask),
                            0);

    auto tladdr = ton::create_tl_object<ton::ton_api::adnl_address_udp>(addr.get_ipv4(), addr.get_port());
    auto addr_vec = std::vector<ton::tl_object_ptr<ton::ton_api::adnl_Address>>();
    addr_vec.push_back(std::move(tladdr));
    auto tladdrlist = ton::create_tl_object<ton::ton_api::adnl_addressList>(
        std::move(addr_vec), ton::adnl::Adnl::adnl_start_time(), ton::adnl::Adnl::adnl_start_time(), 0, 2000000000);
    auto addrlist = ton::adnl::AdnlAddressList::create(tladdrlist).move_as_ok();

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub}, std::move(addrlist),
                            static_cast<td::uint8>(0));
    td::actor::send_closure(adnl, &ton::adnl::Adnl::subscribe, ton::adnl::AdnlNodeIdShort{pub.compute_short_id()},
                            ton::adnl::Adnl::int_to_bytestring(ton::ton_api::adnl_ping::ID),
                            std::make_unique<ton::adnl::Callback>());
  });

  while (scheduler.run(1)) {
    if (rotate_logs_flags.exchange(false)) {
      if (td::log_interface) {
        td::log_interface->rotate();
      }
    }
  }
}
