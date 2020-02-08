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
#include "adnl/adnl-network-manager.h"
#include "adnl/adnl.h"
#include "adnl/adnl-test-loopback-implementation.h"

#include "td/utils/port/signals.h"
#include "td/utils/port/path.h"
#include "td/utils/Random.h"

#include <memory>
#include <set>
#include <chrono>
#include <thread>

int main() {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  {
    auto id_str = td::Slice("WQUA224U42HFSKN63K6NU23X42VK4IJRLFGG65CU62JAOL6U47HRCHD");
    auto id = ton::adnl::AdnlNodeIdShort::parse(id_str).move_as_ok();
    CHECK(td::hex_decode("a1406b5ca73472c94df6d5e6d35bbf355571098aca637ba2a7b490397ea73e78").ok() == id.as_slice());
    CHECK(id.serialize() == td::to_lower(id_str));
  }

  std::string db_root_ = "tmp-ee";
  td::rmrf(db_root_).ignore();
  td::mkdir(db_root_).ensure();

  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<ton::keyring::Keyring> keyring;
  td::actor::ActorOwn<ton::adnl::TestLoopbackNetworkManager> network_manager;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl;

  ton::adnl::AdnlNodeIdShort src;
  ton::adnl::AdnlNodeIdShort dst;

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] {
    keyring = ton::keyring::Keyring::create(db_root_);
    network_manager = td::actor::create_actor<ton::adnl::TestLoopbackNetworkManager>("test network manager");
    adnl = ton::adnl::Adnl::create(db_root_, keyring.get());
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

    td::actor::send_closure(adnl, &ton::adnl::Adnl::add_peer, src, ton::adnl::AdnlNodeIdFull{pub2}, addr);

    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, src, true, false);
    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, dst, false, true);
  });

  {
    auto a = ton::adnl::Adnl::adnl_start_time();
    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    CHECK(a == ton::adnl::Adnl::adnl_start_time());
  }

  {
    auto obj = ton::create_tl_object<ton::ton_api::adnl_proxy_fast>(td::BufferSlice{"1234"});
    auto R = ton::adnl::AdnlProxy::create(*obj.get());
    R.ensure();
    auto P = R.move_as_ok();
    td::BufferSlice z{64};
    td::Random::secure_bytes(z.as_slice());
    auto packet = P->encrypt(ton::adnl::AdnlProxy::Packet{2, 3, z.clone()});
    td::Bits256 x;
    x.as_slice().copy_from(packet.as_slice().truncate(32));
    CHECK(x.is_zero());
    auto packet2R = P->decrypt(std::move(packet));
    packet2R.ensure();
    auto packet2 = packet2R.move_as_ok();
    CHECK(packet2.ip == 2);
    CHECK(packet2.port == 3);
    CHECK(packet2.data.as_slice() == z.as_slice());
  }

  auto send_packet = [&](td::uint32 i) {
    td::BufferSlice d{i};
    d.as_slice()[0] = '1';
    if (i >= 5) {
      td::Random::secure_bytes(d.as_slice().remove_prefix(1).truncate(d.size() - 5));
      auto x = td::crc32c(d.as_slice().truncate(d.size() - 4));

      d.as_slice().remove_prefix(d.size() - 4).copy_from(td::Slice{reinterpret_cast<td::uint8 *>(&x), 4});
    } else {
      td::Random::secure_bytes(d.as_slice().remove_prefix(1));
    }

    return d;
  };

  std::atomic<td::uint32> remaining{0};
  scheduler.run_in_context([&] {
    class Callback : public ton::adnl::Adnl::Callback {
     public:
      void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                           td::BufferSlice data) override {
        CHECK(data.size() <= ton::adnl::Adnl::huge_packet_max_size());
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
        UNREACHABLE();
      }
      Callback(std::atomic<td::uint32> &remaining) : remaining_(remaining) {
      }

     private:
      std::atomic<td::uint32> &remaining_;
    };
    td::actor::send_closure(adnl, &ton::adnl::Adnl::subscribe, dst, "1", std::make_unique<Callback>(remaining));
  });

  LOG(ERROR) << "testing delivering of all packets";

  auto f = td::Clocks::system();
  scheduler.run_in_context([&] {
    for (td::uint32 i = 1; i <= ton::adnl::Adnl::huge_packet_max_size(); i++) {
      remaining++;
      td::actor::send_closure(adnl, &ton::adnl::Adnl::send_message, src, dst, send_packet(i));
    }
  });

  auto t = td::Timestamp::in(320.0);
  while (scheduler.run(1)) {
    if (!remaining) {
      break;
    }
    if (t.is_in_past()) {
      LOG(FATAL) << "failed to receive packets: remaining=" << remaining;
    }
  }

  LOG(ERROR) << "successfully tested delivering of packets of all sizes. Time=" << (td::Clocks::system() - f);

  scheduler.run_in_context([&] {
    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, src, true, true);
    td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, dst, true, true);
  });

  scheduler.run_in_context(
      [&] { td::actor::send_closure(adnl, &ton::adnl::Adnl::send_message, dst, src, send_packet(1)); });
  t = td::Timestamp::in(1.0);
  while (scheduler.run(1)) {
    if (t.is_in_past()) {
      break;
    }
  }

  LOG(ERROR) << "testing with channels enabled";

  f = td::Clocks::system();
  scheduler.run_in_context([&] {
    for (td::uint32 i = 1; i <= ton::adnl::Adnl::huge_packet_max_size(); i++) {
      remaining++;
      td::actor::send_closure(adnl, &ton::adnl::Adnl::send_message, src, dst, send_packet(i));
    }
  });

  t = td::Timestamp::in(320.0);
  while (scheduler.run(1)) {
    if (!remaining) {
      break;
    }
    if (t.is_in_past()) {
      LOG(FATAL) << "failed to receive packets: remaining=" << remaining;
    }
  }
  LOG(ERROR) << "successfully tested delivering of packets of all sizes with channels enabled. Time="
             << (td::Clocks::system() - f);

  scheduler.run_in_context([&] {
    class Callback : public ton::adnl::Adnl::Callback {
     public:
      void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                           td::BufferSlice data) override {
        UNREACHABLE();
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
      Callback() {
      }
    };
    td::actor::send_closure(adnl, &ton::adnl::Adnl::subscribe, dst, "2", std::make_unique<Callback>());
  });

  LOG(ERROR) << "testing queries";
  for (td::uint32 i = 1; i <= ton::adnl::Adnl::huge_packet_max_size(); i++) {
    remaining++;
    td::BufferSlice d{5};
    d.as_slice()[0] = '2';
    d.as_slice().remove_prefix(1).copy_from(td::Slice{reinterpret_cast<td::uint8 *>(&i), 4});

    auto P = td::PromiseCreator::lambda([&, i](td::Result<td::BufferSlice> R) {
      R.ensure();
      auto data = R.move_as_ok();
      CHECK(data.size() == i);

      if (i >= 4) {
        CHECK(td::crc32c(data.as_slice().truncate(data.size() - 4)) ==
              *reinterpret_cast<const td::uint32 *>(data.as_slice().remove_prefix(data.size() - 4).begin()));
      }

      CHECK(remaining > 0);
      remaining--;
    });
    scheduler.run_in_context([&] {
      td::actor::send_closure(adnl, &ton::adnl::Adnl::send_query, src, dst, PSTRING() << "query" << i, std::move(P),
                              td::Timestamp::in(320.0), std::move(d));
    });
  }

  t = td::Timestamp::in(320.0);
  while (scheduler.run(1)) {
    if (!remaining) {
      break;
    }
    if (t.is_in_past()) {
      LOG(FATAL) << "failed to receive answers: remaining=" << remaining;
    }
  }
  LOG(ERROR) << "successfully tested delivering of quries/answers. Time=" << (td::Clocks::system() - f);

  LOG(ERROR) << "testing packets, that should be ignored";

  // too big answer
  scheduler.run_in_context([&] {
    auto x = ton::adnl::Adnl::huge_packet_max_size() + 1;
    td::BufferSlice d{5};
    d.as_slice()[0] = '2';
    d.as_slice().remove_prefix(1).copy_from(td::Slice{reinterpret_cast<td::uint8 *>(&x), 4});
    auto P = td::PromiseCreator::lambda([](td::Result<td::BufferSlice> R) { CHECK(R.is_error()); });
    td::actor::send_closure(adnl, &ton::adnl::Adnl::send_query, src, dst, PSTRING() << "query" << x, std::move(P),
                            td::Timestamp::in(320.0), std::move(d));
  });
  // too big packet
  scheduler.run_in_context([&] {
    auto x = ton::adnl::Adnl::huge_packet_max_size() + 1;
    td::actor::send_closure(adnl, &ton::adnl::Adnl::send_message, src, dst, send_packet(x));
  });
  // no callbacks
  scheduler.run_in_context([&] {
    td::BufferSlice d{1};
    d.as_slice()[0] = '3';
    td::actor::send_closure(adnl, &ton::adnl::Adnl::send_message, src, dst, std::move(d));
  });
  // no callbacks 2
  scheduler.run_in_context([&] {
    td::BufferSlice d{};
    td::actor::send_closure(adnl, &ton::adnl::Adnl::send_message, src, dst, std::move(d));
  });
  t = td::Timestamp::in(2.0);
  while (scheduler.run(1)) {
    if (!remaining) {
      break;
    }
    if (t.is_in_past()) {
      break;
    }
  }
  LOG(ERROR) << "successfully tested ignoring";

  td::rmrf(db_root_).ensure();
  std::_Exit(0);
  return 0;
}
