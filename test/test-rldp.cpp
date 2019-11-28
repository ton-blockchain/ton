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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "adnl/adnl-network-manager.h"
#include "adnl/adnl-test-loopback-implementation.h"
#include "adnl/adnl.h"
#include "rldp/rldp.h"

#include "td/utils/port/signals.h"
#include "td/utils/port/path.h"
#include "td/utils/Random.h"

#include <memory>
#include <set>

int main() {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  std::string db_root_ = "tmp-ee";
  td::rmrf(db_root_).ignore();
  td::mkdir(db_root_).ensure();

  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::TestLoopbackNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp;

  ton::adnl::AdnlNodeIdShort src;
  ton::adnl::AdnlNodeIdShort dst;

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] {
    keyring = ton::keyring::Keyring::create(db_root_);
    network_manager = td::actor::create_actor<ton::adnl::TestLoopbackNetworkManager>("test net");
    adnl = ton::adnl::Adnl::create(db_root_, keyring.get());
    rldp = ton::rldp::Rldp::create(adnl.get());
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());

    auto pk1 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    auto pub1 = pk1.compute_public_key();
    src = ton::adnl::AdnlNodeIdShort{pub1.compute_short_id()};
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk1), true, [](td::Unit) {});

    auto pk2 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    auto pub2 = pk2.compute_public_key();
    dst = ton::adnl::AdnlNodeIdShort{pub2.compute_short_id()};
    td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk2), true, [](td::Unit) {});

    auto addr = ton::adnl::TestLoopbackNetworkManager::generate_dummy_addr_list();

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub1}, addr);
    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub2}, addr);
    td::actor::send_closure(rldp, &ton::rldp::Rldp::add_id, src);
    td::actor::send_closure(rldp, &ton::rldp::Rldp::add_id, dst);

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_peer, src, ton::adnl::AdnlNodeIdFull{pub2}, addr);

    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, src, true, true);
    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, dst, true, true);
  });

  auto send_packet = [&](td::uint32 i) {
    td::BufferSlice d{5};
    d.as_slice()[0] = '1';
    d.as_slice().remove_prefix(1).copy_from(td::Slice{reinterpret_cast<td::uint8 *>(&i), 4});

    return d;
  };

  std::atomic<td::uint32> remaining{0};
  scheduler.run_in_context([&] {
    class Callback : public ton::adnl::Adnl::Callback {
     public:
      void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                           td::BufferSlice data) override {
        CHECK(src == src);
        CHECK(dst == dst);
        if (data.size() >= 5) {
          CHECK(td::crc32c(data.as_slice().truncate(data.size() - 4)) ==
                *reinterpret_cast<const td::uint32 *>(data.as_slice().remove_prefix(data.size() - 4).begin()));
        }
        CHECK(remaining_ > 0);
        remaining_--;
      }
      void receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                         td::Promise<td::BufferSlice> promise) override {
        CHECK(data.size() == 5);

        td::uint32 s = *reinterpret_cast<const td::uint32 *>(data.as_slice().remove_prefix(1).begin());

        td::BufferSlice d{s};
        if (s >= 4) {
          td::Random::secure_bytes(d.as_slice().truncate(s - 4));
          auto x = td::crc32c(d.as_slice().truncate(d.size() - 4));

          d.as_slice().remove_prefix(d.size() - 4).copy_from(td::Slice{reinterpret_cast<td::uint8 *>(&x), 4});
        } else {
          td::Random::secure_bytes(d.as_slice());
        }

        promise.set_value(std::move(d));
      }
      Callback(std::atomic<td::uint32> &remaining) : remaining_(remaining) {
      }

     private:
      std::atomic<td::uint32> &remaining_;
    };
    td::actor::send_closure(adnl, &ton::adnl::Adnl::subscribe, dst, "1", std::make_unique<Callback>(remaining));
  });

  std::vector<td::uint32> sizes{1, 1024, 1 << 20, 2 << 20, 3 << 20, 10 << 20, 16 << 20};

  for (auto &size : sizes) {
    LOG(ERROR) << "testing delivering of packet of size " << size;

    auto f = td::Clocks::system();
    scheduler.run_in_context([&] {
      remaining++;
      td::actor::send_closure(rldp, &ton::rldp::Rldp::send_query_ex, src, dst, std::string("t"),
                              td::PromiseCreator::lambda([&](td::Result<td::BufferSlice> R) {
                                R.ensure();
                                remaining--;
                              }),
                              td::Timestamp::in(1024.0), send_packet(size), size + 1024);
    });

    auto t = td::Timestamp::in(1024.0);
    while (scheduler.run(16)) {
      if (!remaining) {
        break;
      }
      if (t.is_in_past()) {
        LOG(FATAL) << "failed to receive packets: remaining=" << remaining;
      }
    }

    LOG(ERROR) << "success. Time=" << (td::Clocks::system() - f);
  }

  scheduler.run_in_context([&] {
    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::set_loss_probability, 0.1);
  });
  LOG(ERROR) << "set loss to 10%";

  for (auto &size : sizes) {
    LOG(ERROR) << "testing delivering of packet of size " << size;

    auto f = td::Clocks::system();
    scheduler.run_in_context([&] {
      remaining++;
      td::actor::send_closure(rldp, &ton::rldp::Rldp::send_query_ex, src, dst, std::string("t"),
                              td::PromiseCreator::lambda([&](td::Result<td::BufferSlice> R) {
                                R.ensure();
                                remaining--;
                              }),
                              td::Timestamp::in(1024.0), send_packet(size), size + 1024);
    });

    auto t = td::Timestamp::in(1024.0);
    while (scheduler.run(16)) {
      if (!remaining) {
        break;
      }
      if (t.is_in_past()) {
        LOG(FATAL) << "failed to receive packets: remaining=" << remaining;
      }
    }

    LOG(ERROR) << "success. Time=" << (td::Clocks::system() - f);
  }

  td::rmrf(db_root_).ensure();
  std::_Exit(0);
  return 0;
}
