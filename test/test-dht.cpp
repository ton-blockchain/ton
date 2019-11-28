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
#include "dht/dht.h"
#include "dht/dht.hpp"

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
  std::vector<td::actor::ActorOwn<ton::dht::Dht>> dht;
  std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config;

  td::actor::Scheduler scheduler({7});

  std::vector<ton::adnl::AdnlNodeIdFull> dht_ids;
  td::uint32 total_nodes = 11;
  std::atomic<td::uint32> remaining{0};

  scheduler.run_in_context([&] {
    keyring = ton::keyring::Keyring::create(db_root_);
    network_manager = td::actor::create_actor<ton::adnl::TestLoopbackNetworkManager>("test net");
    adnl = ton::adnl::Adnl::create(db_root_, keyring.get());
    td::actor::send_closure(adnl, &ton::adnl::Adnl::register_network_manager, network_manager.get());

    auto addr0 = ton::adnl::TestLoopbackNetworkManager::generate_dummy_addr_list(true);
    auto addr = ton::adnl::TestLoopbackNetworkManager::generate_dummy_addr_list();

    for (td::uint32 i = 0; i < total_nodes; i++) {
      auto pk1 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      auto pub1 = pk1.compute_public_key();
      auto src = ton::adnl::AdnlNodeIdShort{pub1.compute_short_id()};

      if (i == 0) {
        auto obj = ton::create_tl_object<ton::ton_api::dht_node>(pub1.tl(), addr0.tl(), -1, td::BufferSlice());
        auto d = pk1.create_decryptor().move_as_ok();
        obj->signature_ = d->sign(serialize_tl_object(obj, true)).move_as_ok();

        std::vector<ton::tl_object_ptr<ton::ton_api::dht_node>> vec;
        vec.push_back(std::move(obj));
        auto nodes = ton::create_tl_object<ton::ton_api::dht_nodes>(std::move(vec));
        auto conf = ton::create_tl_object<ton::ton_api::dht_config_global>(std::move(nodes), 6, 3);
        auto dht_configR = ton::dht::Dht::create_global_config(std::move(conf));
        dht_configR.ensure();
        dht_config = dht_configR.move_as_ok();
      }
      td::actor::send_closure(keyring, &ton::keyring::Keyring::add_key, std::move(pk1), true, [](td::Unit) {});
      td::actor::send_closure(adnl, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{pub1}, addr);
      td::actor::send_closure(network_manager, &ton::adnl::TestLoopbackNetworkManager::add_node_id, src, true, true);

      dht.push_back(ton::dht::Dht::create(src, db_root_, dht_config, keyring.get(), adnl.get()).move_as_ok());
      dht_ids.push_back(ton::adnl::AdnlNodeIdFull{pub1});
    }
    for (auto &n1 : dht_ids) {
      td::actor::send_closure(adnl, &ton::adnl::Adnl::add_peer, n1.compute_short_id(), dht_ids[0], addr);
    }
  });

  LOG(ERROR) << "testing different values";
  auto key_pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
  auto key_pub = key_pk.compute_public_key();
  auto key_short_id = key_pub.compute_short_id();
  auto key_dec = key_pk.create_decryptor().move_as_ok();
  {
    for (td::uint32 idx = 0; idx <= ton::dht::DhtKey::max_index() + 1; idx++) {
      ton::dht::DhtKey dht_key{key_short_id, "test", idx};
      if (idx <= ton::dht::DhtKey::max_index()) {
        dht_key.check().ensure();
      } else {
        dht_key.check().ensure_error();
      }
    }
    {
      ton::dht::DhtKey dht_key{key_short_id, "test", 0};
      dht_key.check().ensure();
      dht_key = ton::dht::DhtKey{key_short_id, "", 0};
      dht_key.check().ensure_error();
      dht_key =
          ton::dht::DhtKey{key_short_id, td::BufferSlice{ton::dht::DhtKey::max_name_length()}.as_slice().str(), 0};
      dht_key.check().ensure();
      dht_key =
          ton::dht::DhtKey{key_short_id, td::BufferSlice{ton::dht::DhtKey::max_name_length() + 1}.as_slice().str(), 0};
      dht_key.check().ensure_error();
    }
    {
      ton::dht::DhtKey dht_key{key_short_id, "test", 0};
      auto dht_update_rule = ton::dht::DhtUpdateRuleSignature::create().move_as_ok();
      ton::dht::DhtKeyDescription dht_key_description{dht_key.clone(), key_pub, dht_update_rule, td::BufferSlice()};
      dht_key_description.update_signature(key_dec->sign(dht_key_description.to_sign()).move_as_ok());
      dht_key_description.check().ensure();
      dht_key_description = ton::dht::DhtKeyDescription{dht_key.clone(), key_pub, dht_update_rule, td::BufferSlice(64)};
      dht_key_description.check().ensure_error();
      dht_key_description.update_signature(key_dec->sign(dht_key_description.to_sign()).move_as_ok());
      dht_key_description.check().ensure();

      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      auto pub = pk.compute_public_key();
      dht_key_description = ton::dht::DhtKeyDescription{dht_key.clone(), pub, dht_update_rule, td::BufferSlice(64)};
      dht_key_description.update_signature(
          pk.create_decryptor().move_as_ok()->sign(dht_key_description.to_sign()).move_as_ok());
      dht_key_description.check().ensure_error();
    }
  }
  {
    ton::dht::DhtKey dht_key{key_short_id, "test", 0};
    auto dht_update_rule = ton::dht::DhtUpdateRuleSignature::create().move_as_ok();
    ton::dht::DhtKeyDescription dht_key_description{std::move(dht_key), key_pub, std::move(dht_update_rule),
                                                    td::BufferSlice()};
    dht_key_description.update_signature(key_dec->sign(dht_key_description.to_sign()).move_as_ok());

    auto ttl = static_cast<td::uint32>(td::Clocks::system() + 3600);
    ton::dht::DhtValue dht_value{dht_key_description.clone(), td::BufferSlice("value"), ttl, td::BufferSlice("")};
    dht_value.check().ensure_error();
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());
    dht_value.check().ensure();
    CHECK(!dht_value.expired());

    dht_value = ton::dht::DhtValue{dht_key_description.clone(), td::BufferSlice(""), ttl, td::BufferSlice("")};
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());
    dht_value.check().ensure();

    dht_value = ton::dht::DhtValue{dht_key_description.clone(), td::BufferSlice(""),
                                   static_cast<td::uint32>(td::Clocks::system() - 1), td::BufferSlice("")};
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());
    dht_value.check().ensure();
    CHECK(dht_value.expired());

    dht_value = ton::dht::DhtValue{dht_key_description.clone(), td::BufferSlice("value"), ttl, td::BufferSlice("")};
    dht_value.update_signature(td::BufferSlice{64});
    dht_value.check().ensure_error();

    dht_value = ton::dht::DhtValue{dht_key_description.clone(), td::BufferSlice(ton::dht::DhtValue::max_value_size()),
                                   ttl, td::BufferSlice("")};
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());
    dht_value.check().ensure();

    dht_value = ton::dht::DhtValue{dht_key_description.clone(),
                                   td::BufferSlice(ton::dht::DhtValue::max_value_size() + 1), ttl, td::BufferSlice("")};
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());
    dht_value.check().ensure_error();
  }

  {
    ton::dht::DhtKey dht_key{key_short_id, "test", 0};
    auto dht_update_rule = ton::dht::DhtUpdateRuleAnybody::create().move_as_ok();
    ton::dht::DhtKeyDescription dht_key_description{std::move(dht_key), key_pub, std::move(dht_update_rule),
                                                    td::BufferSlice()};
    dht_key_description.update_signature(key_dec->sign(dht_key_description.to_sign()).move_as_ok());

    auto ttl = static_cast<td::uint32>(td::Clocks::system() + 3600);
    ton::dht::DhtValue dht_value{dht_key_description.clone(), td::BufferSlice("value"), ttl, td::BufferSlice()};
    dht_value.check().ensure();
    CHECK(!dht_value.expired());
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());
    dht_value.check().ensure_error();

    dht_value = ton::dht::DhtValue{dht_key_description.clone(), td::BufferSlice(), ttl, td::BufferSlice()};
    dht_value.check().ensure();

    dht_value = ton::dht::DhtValue{dht_key_description.clone(), td::BufferSlice(ton::dht::DhtValue::max_value_size()),
                                   ttl, td::BufferSlice()};
    dht_value.check().ensure();

    dht_value = ton::dht::DhtValue{dht_key_description.clone(),
                                   td::BufferSlice(ton::dht::DhtValue::max_value_size() + 1), ttl, td::BufferSlice()};
    dht_value.check().ensure_error();
  }

  {
    ton::dht::DhtKey dht_key{key_short_id, "test", 0};
    auto dht_update_rule = ton::dht::DhtUpdateRuleOverlayNodes::create().move_as_ok();
    ton::dht::DhtKeyDescription dht_key_description{std::move(dht_key), key_pub, std::move(dht_update_rule),
                                                    td::BufferSlice()};
    dht_key_description.update_signature(key_dec->sign(dht_key_description.to_sign()).move_as_ok());

    auto ttl = static_cast<td::uint32>(td::Clocks::system() + 3600);
    ton::dht::DhtValue dht_value{dht_key_description.clone(), td::BufferSlice(""), ttl, td::BufferSlice()};
    dht_value.check().ensure_error();

    auto obj = ton::create_tl_object<ton::ton_api::overlay_nodes>();
    dht_value =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value.check().ensure();

    for (td::uint32 i = 0; i < 100; i++) {
      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      auto pub = pk.compute_public_key();

      auto date = static_cast<td::int32>(td::Clocks::system() - 10);
      //overlay.node.toSign id:adnl.id.short overlay:int256 version:int = overlay.node.ToSign;
      //overlay.node id:PublicKey overlay:int256 version:int signature:bytes = overlay.Node;
      auto to_sign = ton::create_serialize_tl_object<ton::ton_api::overlay_node_toSign>(
          ton::adnl::AdnlNodeIdShort{pub.compute_short_id()}.tl(), key_short_id.tl(), date);
      auto n = ton::create_tl_object<ton::ton_api::overlay_node>(
          pub.tl(), key_short_id.tl(), date, pk.create_decryptor().move_as_ok()->sign(to_sign.as_slice()).move_as_ok());
      obj->nodes_.push_back(std::move(n));
      dht_value =
          ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
      auto size = ton::serialize_tl_object(obj, true).size();
      if (size <= ton::dht::DhtValue::max_value_size()) {
        dht_value.check().ensure();
      } else {
        dht_value.check().ensure_error();
      }
    }

    obj->nodes_.clear();
    auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    auto pub = pk.compute_public_key();

    auto date = static_cast<td::int32>(td::Clocks::system() - 10);
    //overlay.node.toSign id:adnl.id.short overlay:int256 version:int = overlay.node.ToSign;
    //overlay.node id:PublicKey overlay:int256 version:int signature:bytes = overlay.Node;
    auto to_sign = ton::create_serialize_tl_object<ton::ton_api::overlay_node_toSign>(
        ton::adnl::AdnlNodeIdShort{pub.compute_short_id()}.tl(), key_short_id.tl() ^ td::Bits256::ones(), date);
    auto n = ton::create_tl_object<ton::ton_api::overlay_node>(
        pub.tl(), key_short_id.tl() ^ td::Bits256::ones(), date,
        pk.create_decryptor().move_as_ok()->sign(to_sign.as_slice()).move_as_ok());
    obj->nodes_.push_back(std::move(n));
    dht_value =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value.check().ensure_error();

    obj->nodes_.clear();
    to_sign = ton::create_serialize_tl_object<ton::ton_api::overlay_node_toSign>(
        ton::adnl::AdnlNodeIdShort{pub.compute_short_id()}.tl(), key_short_id.tl(), date);
    n = ton::create_tl_object<ton::ton_api::overlay_node>(
        pub.tl(), key_short_id.tl(), date, pk.create_decryptor().move_as_ok()->sign(to_sign.as_slice()).move_as_ok());
    obj->nodes_.push_back(std::move(n));
    dht_value =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value.check().ensure();

    obj->nodes_.clear();
    //to_sign = ton::create_serialize_tl_object<ton::ton_api::overlay_node_toSign>(
    //    ton::adnl::AdnlNodeIdShort{pub.compute_short_id()}.tl(), key_short_id.tl(), date);
    n = ton::create_tl_object<ton::ton_api::overlay_node>(pub.tl(), key_short_id.tl(), date, td::BufferSlice{64});
    obj->nodes_.push_back(std::move(n));
    dht_value =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value.check().ensure_error();

    obj->nodes_.clear();
    to_sign = ton::create_serialize_tl_object<ton::ton_api::overlay_node_toSign>(
        ton::adnl::AdnlNodeIdShort{pub.compute_short_id()}.tl(), key_short_id.tl(), date);
    n = ton::create_tl_object<ton::ton_api::overlay_node>(
        pub.tl(), key_short_id.tl(), date, pk.create_decryptor().move_as_ok()->sign(to_sign.as_slice()).move_as_ok());
    obj->nodes_.push_back(std::move(n));
    dht_value =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value.check().ensure();

    auto dht_value2 =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value2.check().ensure();
    dht_value.update(std::move(dht_value2)).ensure();
    CHECK(ton::fetch_tl_object<ton::ton_api::overlay_nodes>(dht_value.value().as_slice(), true)
              .move_as_ok()
              ->nodes_.size() == 1);

    obj->nodes_.clear();
    {
      td::BufferSlice x{64};
      td::Random::secure_bytes(x.as_slice());
      auto pk2 = ton::PrivateKey{ton::privkeys::Unenc{x.clone()}};
      n = ton::create_tl_object<ton::ton_api::overlay_node>(
          pk2.compute_public_key().tl(), key_short_id.tl(), date,
          pk2.create_decryptor().move_as_ok()->sign(to_sign.as_slice()).move_as_ok());
      obj->nodes_.push_back(std::move(n));
    }
    dht_value2 =
        ton::dht::DhtValue{dht_key_description.clone(), ton::serialize_tl_object(obj, true), ttl, td::BufferSlice()};
    dht_value2.check().ensure();
    dht_value.update(std::move(dht_value2)).ensure();
    CHECK(ton::fetch_tl_object<ton::ton_api::overlay_nodes>(dht_value.value().as_slice(), true)
              .move_as_ok()
              ->nodes_.size() == 2);
  }
  LOG(ERROR) << "success";

  LOG(ERROR) << "empty run";
  auto t = td::Timestamp::in(10.0);
  while (scheduler.run(1)) {
    if (t.is_in_past()) {
      break;
    }
  }

  LOG(ERROR) << "success";

  for (td::uint32 x = 0; x < 100; x++) {
    ton::dht::DhtKey dht_key{key_short_id, PSTRING() << "test-" << x, x % 8};
    auto dht_update_rule = ton::dht::DhtUpdateRuleSignature::create().move_as_ok();
    ton::dht::DhtKeyDescription dht_key_description{std::move(dht_key), key_pub, std::move(dht_update_rule),
                                                    td::BufferSlice()};
    dht_key_description.update_signature(key_dec->sign(dht_key_description.to_sign()).move_as_ok());

    auto ttl = static_cast<td::uint32>(td::Clocks::system() + 3600);
    td::uint8 v[1];
    v[0] = static_cast<td::uint8>(x);
    ton::dht::DhtValue dht_value{std::move(dht_key_description), td::BufferSlice(td::Slice(v, 1)), ttl,
                                 td::BufferSlice("")};
    dht_value.update_signature(key_dec->sign(dht_value.to_sign()).move_as_ok());

    remaining++;
    auto P = td::PromiseCreator::lambda([&](td::Result<td::Unit> R) {
      R.ensure();
      remaining--;
    });

    scheduler.run_in_context([&] {
      td::actor::send_closure(dht[td::Random::fast(0, total_nodes - 1)], &ton::dht::Dht::set_value,
                              std::move(dht_value), std::move(P));
    });
  }

  LOG(ERROR) << "stores";
  t = td::Timestamp::in(60.0);
  while (scheduler.run(1)) {
    if (!remaining) {
      break;
    }
    if (t.is_in_past()) {
      LOG(FATAL) << "failed: remaining = " << remaining;
    }
  }
  LOG(ERROR) << "success";

  for (td::uint32 x = 0; x < 100; x++) {
    ton::dht::DhtKey dht_key{key_short_id, PSTRING() << "test-" << x, x % 8};

    remaining++;
    auto P = td::PromiseCreator::lambda([&, idx = x](td::Result<ton::dht::DhtValue> R) {
      R.ensure();
      auto v = R.move_as_ok();
      CHECK(v.key().key().public_key_hash() == key_short_id);
      CHECK(v.key().key().name() == (PSTRING() << "test-" << idx));
      CHECK(v.key().key().idx() == idx % 8);
      td::uint8 buf[1];
      buf[0] = static_cast<td::uint8>(idx);
      CHECK(v.value().as_slice() == td::Slice(buf, 1));
      remaining--;
    });

    scheduler.run_in_context([&] {
      td::actor::send_closure(dht[td::Random::fast(0, total_nodes - 1)], &ton::dht::Dht::get_value, dht_key,
                              std::move(P));
    });
  }

  LOG(ERROR) << "gets";
  t = td::Timestamp::in(60.0);
  while (scheduler.run(1)) {
    if (!remaining) {
      break;
    }
    if (t.is_in_past()) {
      LOG(FATAL) << "failed: remaining = " << remaining;
    }
  }
  LOG(ERROR) << "success";

  td::rmrf(db_root_).ensure();
  std::_Exit(0);
  return 0;
}
