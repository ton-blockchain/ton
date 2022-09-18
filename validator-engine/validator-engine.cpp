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
#include "validator-engine.hpp"

#include "auto/tl/ton_api.h"
#include "overlay-manager.h"
#include "td/actor/actor.h"
#include "tl-utils/tl-utils.hpp"
#include "tl/TlObject.h"
#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

#include "common/errorlog.h"

#include "crypto/vm/cp0.h"
#include "crypto/fift/utils.h"

#include "td/utils/filesystem.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/overloaded.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/user.h"
#include "td/utils/port/rlimit.h"
#include "td/utils/ThreadSafeCounter.h"
#include "td/utils/TsFileLog.h"
#include "td/utils/Random.h"

#include "auto/tl/lite_api.h"

#include "memprof/memprof.h"

#include "dht/dht.hpp"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <limits>
#include <set>
#include "git.h"


Config::Config() {
  out_port = 3278;
  full_node = ton::PublicKeyHash::zero();
}

Config::Config(ton::ton_api::engine_validator_config &config) {
  full_node = ton::PublicKeyHash::zero();
  out_port = static_cast<td::uint16>(config.out_port_);
  if (!out_port) {
    out_port = 3278;
  }
  for (auto &addr : config.addrs_) {
    td::IPAddress in_ip;
    td::IPAddress out_ip;
    std::shared_ptr<ton::adnl::AdnlProxy> proxy = nullptr;
    std::vector<AdnlCategory> categories;
    std::vector<AdnlCategory> priority_categories;
    ton::ton_api::downcast_call(
        *addr.get(),
        td::overloaded(
            [&](const ton::ton_api::engine_addr &obj) {
              in_ip.init_ipv4_port(td::IPAddress::ipv4_to_str(obj.ip_), static_cast<td::uint16>(obj.port_)).ensure();
              out_ip = in_ip;
              for (auto cat : obj.categories_) {
                categories.push_back(td::narrow_cast<td::uint8>(cat));
              }
              for (auto cat : obj.priority_categories_) {
                priority_categories.push_back(td::narrow_cast<td::uint8>(cat));
              }
            },
            [&](const ton::ton_api::engine_addrProxy &obj) {
              in_ip.init_ipv4_port(td::IPAddress::ipv4_to_str(obj.in_ip_), static_cast<td::uint16>(obj.in_port_))
                  .ensure();
              out_ip.init_ipv4_port(td::IPAddress::ipv4_to_str(obj.out_ip_), static_cast<td::uint16>(obj.out_port_))
                  .ensure();
              if (obj.proxy_type_) {
                auto R = ton::adnl::AdnlProxy::create(*obj.proxy_type_.get());
                R.ensure();
                proxy = R.move_as_ok();
                for (auto cat : obj.categories_) {
                  categories.push_back(td::narrow_cast<td::uint8>(cat));
                }
                for (auto cat : obj.priority_categories_) {
                  priority_categories.push_back(td::narrow_cast<td::uint8>(cat));
                }
              }
            }));

    config_add_network_addr(in_ip, out_ip, std::move(proxy), categories, priority_categories).ensure();
  }
  for (auto &adnl : config.adnl_) {
    config_add_adnl_addr(ton::PublicKeyHash{adnl->id_}, td::narrow_cast<td::uint8>(adnl->category_)).ensure();
  }
  for (auto &dht : config.dht_) {
    config_add_dht_node(ton::PublicKeyHash{dht->id_}).ensure();
  }
  for (auto &val : config.validators_) {
    auto key = ton::PublicKeyHash{val->id_};
    config_add_validator_permanent_key(key, val->election_date_, val->expire_at_).ensure();
    for (auto &temp : val->temp_keys_) {
      config_add_validator_temp_key(key, ton::PublicKeyHash{temp->key_}, temp->expire_at_).ensure();
    }
    for (auto &adnl : val->adnl_addrs_) {
      config_add_validator_adnl_id(key, ton::PublicKeyHash{adnl->id_}, adnl->expire_at_).ensure();
    }
  }
  config_add_full_node_adnl_id(ton::PublicKeyHash{config.fullnode_}).ensure();

  for (auto &s : config.fullnodeslaves_) {
    td::IPAddress ip;
    ip.init_ipv4_port(td::IPAddress::ipv4_to_str(s->ip_), static_cast<td::uint16>(s->port_)).ensure();
    config_add_full_node_slave(ip, ton::PublicKey{s->adnl_}).ensure();
  }

  for (auto &s : config.fullnodemasters_) {
    config_add_full_node_master(s->port_, ton::PublicKeyHash{s->adnl_}).ensure();
  }

  for (auto &serv : config.liteservers_) {
    config_add_lite_server(ton::PublicKeyHash{serv->id_}, serv->port_).ensure();
  }

  for (auto &serv : config.control_) {
    auto key = ton::PublicKeyHash{serv->id_};
    config_add_control_interface(key, serv->port_).ensure();

    for (auto &proc : serv->allowed_) {
      config_add_control_process(key, serv->port_, ton::PublicKeyHash{proc->id_}, proc->permissions_).ensure();
    }
  }

  if (config.gc_) {
    for (auto &gc : config.gc_->ids_) {
      config_add_gc(ton::PublicKeyHash{gc}).ensure();
    }
  }
}

ton::tl_object_ptr<ton::ton_api::engine_validator_config> Config::tl() const {
  std::vector<ton::tl_object_ptr<ton::ton_api::engine_Addr>> addrs_vec;
  for (auto &x : addrs) {
    if (x.second.proxy) {
      addrs_vec.push_back(ton::create_tl_object<ton::ton_api::engine_addrProxy>(
          static_cast<td::int32>(x.second.in_addr.get_ipv4()), x.second.in_addr.get_port(),
          static_cast<td::int32>(x.first.addr.get_ipv4()), x.first.addr.get_port(), x.second.proxy->tl(),
          std::vector<td::int32>(x.second.cats.begin(), x.second.cats.end()),
          std::vector<td::int32>(x.second.priority_cats.begin(), x.second.priority_cats.end())));
    } else {
      addrs_vec.push_back(ton::create_tl_object<ton::ton_api::engine_addr>(
          static_cast<td::int32>(x.first.addr.get_ipv4()), x.first.addr.get_port(),
          std::vector<td::int32>(x.second.cats.begin(), x.second.cats.end()),
          std::vector<td::int32>(x.second.priority_cats.begin(), x.second.priority_cats.end())));
    }
  }
  std::vector<ton::tl_object_ptr<ton::ton_api::engine_adnl>> adnl_vec;
  for (auto &x : adnl_ids) {
    adnl_vec.push_back(ton::create_tl_object<ton::ton_api::engine_adnl>(x.first.tl(), x.second));
  }
  std::vector<ton::tl_object_ptr<ton::ton_api::engine_dht>> dht_vec;
  for (auto &x : dht_ids) {
    dht_vec.push_back(ton::create_tl_object<ton::ton_api::engine_dht>(x.tl()));
  }

  std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator>> val_vec;
  for (auto &val : validators) {
    std::vector<ton::tl_object_ptr<ton::ton_api::engine_validatorTempKey>> temp_vec;
    for (auto &t : val.second.temp_keys) {
      temp_vec.push_back(ton::create_tl_object<ton::ton_api::engine_validatorTempKey>(t.first.tl(), t.second));
    }
    std::vector<ton::tl_object_ptr<ton::ton_api::engine_validatorAdnlAddress>> adnl_val_vec;
    for (auto &t : val.second.adnl_ids) {
      adnl_val_vec.push_back(ton::create_tl_object<ton::ton_api::engine_validatorAdnlAddress>(t.first.tl(), t.second));
    }
    val_vec.push_back(ton::create_tl_object<ton::ton_api::engine_validator>(
        val.first.tl(), std::move(temp_vec), std::move(adnl_val_vec), val.second.election_date, val.second.expire_at));
  }

  std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_fullNodeSlave>> full_node_slaves_vec;
  for (auto &x : full_node_slaves) {
    full_node_slaves_vec.push_back(ton::create_tl_object<ton::ton_api::engine_validator_fullNodeSlave>(
        x.addr.get_ipv4(), x.addr.get_port(), x.key.tl()));
  }
  std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_fullNodeMaster>> full_node_masters_vec;
  for (auto &x : full_node_masters) {
    full_node_masters_vec.push_back(
        ton::create_tl_object<ton::ton_api::engine_validator_fullNodeMaster>(x.first, x.second.tl()));
  }

  std::vector<ton::tl_object_ptr<ton::ton_api::engine_liteServer>> liteserver_vec;
  for (auto &x : liteservers) {
    liteserver_vec.push_back(ton::create_tl_object<ton::ton_api::engine_liteServer>(x.second.tl(), x.first));
  }

  std::vector<ton::tl_object_ptr<ton::ton_api::engine_controlInterface>> control_vec;
  for (auto &x : controls) {
    std::vector<ton::tl_object_ptr<ton::ton_api::engine_controlProcess>> control_proc_vec;
    for (auto &y : x.second.clients) {
      control_proc_vec.push_back(ton::create_tl_object<ton::ton_api::engine_controlProcess>(y.first.tl(), y.second));
    }
    control_vec.push_back(ton::create_tl_object<ton::ton_api::engine_controlInterface>(x.second.key.tl(), x.first,
                                                                                       std::move(control_proc_vec)));
  }

  auto gc_vec = ton::create_tl_object<ton::ton_api::engine_gc>(std::vector<td::Bits256>{});
  for (auto &id : gc) {
    gc_vec->ids_.push_back(id.tl());
  }
  return ton::create_tl_object<ton::ton_api::engine_validator_config>(
      out_port, std::move(addrs_vec), std::move(adnl_vec), std::move(dht_vec), std::move(val_vec), full_node.tl(),
      std::move(full_node_slaves_vec), std::move(full_node_masters_vec), std::move(liteserver_vec),
      std::move(control_vec), std::move(gc_vec));
}

td::Result<bool> Config::config_add_network_addr(td::IPAddress in_ip, td::IPAddress out_ip,
                                                 std::shared_ptr<ton::adnl::AdnlProxy> proxy,
                                                 std::vector<AdnlCategory> cats, std::vector<AdnlCategory> prio_cats) {
  Addr addr{out_ip};

  auto it = addrs.find(addr);
  if (it != addrs.end()) {
    bool mod = false;
    if (!(it->second.in_addr == in_ip)) {
      it->second.in_addr = in_ip;
      mod = true;
    }
    if (it->second.proxy != proxy) {
      it->second.proxy = std::move(proxy);
      mod = true;
    }
    for (auto &c : cats) {
      if (it->second.cats.insert(c).second) {
        mod = true;
      }
    }
    for (auto &c : prio_cats) {
      if (it->second.priority_cats.insert(c).second) {
        mod = true;
      }
    }
    return mod;
  } else {
    it = addrs.emplace(std::move(addr), AddrCats{}).first;
    it->second.in_addr = in_ip;
    it->second.proxy = std::move(proxy);
    for (auto &c : cats) {
      it->second.cats.insert(c);
    }
    for (auto &c : prio_cats) {
      it->second.priority_cats.insert(c);
    }
    return true;
  }
}

td::Result<bool> Config::config_add_adnl_addr(ton::PublicKeyHash addr, AdnlCategory cat) {
  auto it = adnl_ids.find(addr);
  if (it != adnl_ids.end()) {
    if (it->second != cat) {
      it->second = cat;
      return true;
    } else {
      return false;
    }
  } else {
    incref(addr);
    adnl_ids.emplace(addr, cat);
    return true;
  }
}

td::Result<bool> Config::config_add_dht_node(ton::PublicKeyHash id) {
  if (dht_ids.count(id) > 0) {
    return false;
  }
  if (adnl_ids.count(id) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "to-be-added dht node not in adnl nodes list");
  }
  incref(id);
  dht_ids.insert(id);
  return true;
}

td::Result<bool> Config::config_add_validator_permanent_key(ton::PublicKeyHash id, ton::UnixTime election_date,
                                                            ton::UnixTime expire_at) {
  for (auto &x : validators) {
    if (x.second.election_date == election_date && x.first != id) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "duplicate election date");
    }
  }
  auto it = validators.find(id);
  if (it != validators.end()) {
    if (it->second.election_date != election_date) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "election date changed");
    }
    if (it->second.expire_at != expire_at) {
      it->second.expire_at = expire_at;
      return true;
    } else {
      return false;
    }
  } else {
    incref(id);
    validators[id] = Validator{{}, {}, election_date, expire_at};
    return true;
  }
}

td::Result<bool> Config::config_add_validator_temp_key(ton::PublicKeyHash perm_key, ton::PublicKeyHash id,
                                                       ton::UnixTime expire_at) {
  if (validators.count(perm_key) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "unknown permanent validator key");
  }
  auto &v = validators[perm_key];

  auto it = v.temp_keys.find(id);
  if (it != v.temp_keys.end()) {
    if (it->second != expire_at) {
      it->second = expire_at;
      return true;
    } else {
      return false;
    }
  } else {
    incref(id);
    v.temp_keys.emplace(id, expire_at);
    return true;
  }
}

td::Result<bool> Config::config_add_validator_adnl_id(ton::PublicKeyHash perm_key, ton::PublicKeyHash adnl_id,
                                                      ton::UnixTime expire_at) {
  if (adnl_ids.count(adnl_id) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "to-be-added validator adnl address not in adnl nodes list");
  }
  if (validators.count(perm_key) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "unknown permanent validator key");
  }
  auto &v = validators[perm_key];

  auto it = v.adnl_ids.find(adnl_id);
  if (it != v.adnl_ids.end()) {
    if (it->second != expire_at) {
      it->second = expire_at;
      return true;
    } else {
      return false;
    }
  } else {
    incref(adnl_id);
    v.adnl_ids.emplace(adnl_id, expire_at);
    return true;
  }
}

td::Result<bool> Config::config_add_full_node_adnl_id(ton::PublicKeyHash id) {
  if (full_node == id) {
    return false;
  }
  if (adnl_ids.count(id) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "to-be-added full node adnl address not in adnl nodes list");
  }
  if (!full_node.is_zero()) {
    decref(full_node);
  }
  if (!id.is_zero()) {
    incref(id);
  }
  full_node = id;
  return true;
}

td::Result<bool> Config::config_add_full_node_slave(td::IPAddress addr, ton::PublicKey id) {
  for (auto &s : full_node_slaves) {
    if (s.addr == addr) {
      if (s.key == id) {
        return true;
      } else {
        return td::Status::Error(ton::ErrorCode::error, "duplicate slave ip");
      }
    }
  }
  full_node_slaves.push_back(FullNodeSlave{id, addr});
  return true;
}

td::Result<bool> Config::config_add_full_node_master(td::int32 port, ton::PublicKeyHash id) {
  if (adnl_ids.count(id) == 0) {
    return td::Status::Error(ton::ErrorCode::notready,
                             "to-be-added full node master adnl address not in adnl nodes list");
  }
  auto it = full_node_masters.find(port);
  if (it != full_node_masters.end()) {
    if (it->second == id) {
      return false;
    } else {
      return td::Status::Error("duplicate master port");
    }
  }
  if (liteservers.count(port) > 0 || controls.count(port) > 0) {
    return td::Status::Error("duplicate master port");
  }
  incref(id);
  full_node_masters.emplace(port, id);
  return true;
}

td::Result<bool> Config::config_add_lite_server(ton::PublicKeyHash key, td::int32 port) {
  if (controls.count(port) > 0) {
    return td::Status::Error(ton::ErrorCode::error, "duplicate port");
  }
  auto it = liteservers.find(port);
  if (it != liteservers.end()) {
    if (it->second == key) {
      return false;
    } else {
      return td::Status::Error(ton::ErrorCode::error, "duplicate port");
    }
  } else {
    incref(key);
    liteservers.emplace(port, key);
    return true;
  }
}

td::Result<bool> Config::config_add_control_interface(ton::PublicKeyHash key, td::int32 port) {
  if (liteservers.count(port) > 0) {
    return td::Status::Error(ton::ErrorCode::error, "duplicate port");
  }
  auto it = controls.find(port);
  if (it != controls.end()) {
    if (it->second.key == key) {
      return false;
    } else {
      return td::Status::Error(ton::ErrorCode::error, "duplicate port");
    }
  } else {
    incref(key);
    controls.emplace(port, Control{key, {}});
    return true;
  }
}

td::Result<bool> Config::config_add_control_process(ton::PublicKeyHash key, td::int32 port, ton::PublicKeyHash id,
                                                    td::uint32 permissions) {
  if (controls.count(port) == 0) {
    return td::Status::Error(ton::ErrorCode::error, "unknown control interface");
  }
  auto &v = controls[port];
  if (v.key != key) {
    return td::Status::Error(ton::ErrorCode::error, "unknown control interface");
  }

  auto it = v.clients.find(id);
  if (it != v.clients.end()) {
    if (!permissions) {
      v.clients.erase(id);
      return true;
    }
    if (it->second != permissions) {
      it->second = permissions;
      return true;
    } else {
      return false;
    }
  } else {
    if (!permissions) {
      return false;
    }
    v.clients.emplace(id, permissions);
    return true;
  }
}

td::Result<bool> Config::config_add_gc(ton::PublicKeyHash key) {
  return gc.insert(key).second;
}

void Config::decref(ton::PublicKeyHash key) {
  auto v = keys_refcnt[key]--;
  CHECK(v > 0);
  if (v == 1) {
    config_add_gc(key).ensure();
  }
}

td::Result<bool> Config::config_del_network_addr(td::IPAddress a, std::vector<AdnlCategory> cats,
                                                 std::vector<AdnlCategory> prio_cats) {
  Addr addr{a};

  auto it = addrs.find(addr);
  if (it != addrs.end()) {
    bool mod = false;
    for (auto &c : cats) {
      if (it->second.cats.erase(c)) {
        mod = true;
      }
    }
    for (auto &c : prio_cats) {
      if (it->second.priority_cats.erase(c)) {
        mod = true;
      }
    }
    if (it->second.cats.size() == 0 && it->second.priority_cats.size() == 0) {
      addrs.erase(it);
    }
    return mod;
  } else {
    return false;
  }
}

td::Result<bool> Config::config_del_adnl_addr(ton::PublicKeyHash addr) {
  if (adnl_ids.count(addr) == 0) {
    return false;
  }

  if (dht_ids.count(addr)) {
    return td::Status::Error(ton::ErrorCode::error, "adnl addr still in use");
  }
  if (full_node == addr) {
    return td::Status::Error(ton::ErrorCode::error, "adnl addr still in use");
  }

  for (auto &x : validators) {
    if (x.second.adnl_ids.count(addr)) {
      return td::Status::Error(ton::ErrorCode::error, "adnl addr still in use");
    }
  }

  decref(addr);
  adnl_ids.erase(addr);
  return true;
}

td::Result<bool> Config::config_del_dht_node(ton::PublicKeyHash id) {
  if (dht_ids.count(id) == 0) {
    return false;
  }
  decref(id);
  dht_ids.erase(id);
  return true;
}

td::Result<bool> Config::config_del_validator_permanent_key(ton::PublicKeyHash id) {
  if (validators.count(id) == 0) {
    return false;
  }
  auto &v = validators[id];
  for (auto &temp_key : v.temp_keys) {
    decref(temp_key.first);
  }
  for (auto &adnl_id : v.adnl_ids) {
    decref(adnl_id.first);
  }
  decref(id);
  validators.erase(id);
  return true;
}

td::Result<bool> Config::config_del_validator_temp_key(ton::PublicKeyHash perm_key, ton::PublicKeyHash id) {
  if (validators.count(perm_key) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "unknown permanent validator key");
  }
  auto &v = validators[perm_key];

  auto it = v.temp_keys.find(id);
  if (it != v.temp_keys.end()) {
    decref(id);
    v.temp_keys.erase(id);
    return true;
  } else {
    return false;
  }
}

td::Result<bool> Config::config_del_validator_adnl_id(ton::PublicKeyHash perm_key, ton::PublicKeyHash adnl_id) {
  if (validators.count(perm_key) == 0) {
    return td::Status::Error(ton::ErrorCode::notready, "unknown permanent validator key");
  }
  auto &v = validators[perm_key];

  auto it = v.adnl_ids.find(adnl_id);
  if (it != v.temp_keys.end()) {
    decref(adnl_id);
    v.adnl_ids.erase(adnl_id);
    return true;
  } else {
    return false;
  }
}

td::Result<bool> Config::config_del_full_node_adnl_id() {
  return config_add_full_node_adnl_id(ton::PublicKeyHash::zero());
}

td::Result<bool> Config::config_del_lite_server(td::int32 port) {
  auto it = liteservers.find(port);
  if (it != liteservers.end()) {
    decref(it->second);
    liteservers.erase(it);
    return true;
  } else {
    return false;
  }
}

td::Result<bool> Config::config_del_control_interface(td::int32 port) {
  auto it = controls.find(port);
  if (it != controls.end()) {
    decref(it->second.key);
    controls.erase(it);
    return true;
  } else {
    return false;
  }
}

td::Result<bool> Config::config_del_control_process(td::int32 port, ton::PublicKeyHash id) {
  auto it = controls.find(port);
  if (it != controls.end()) {
    return it->second.clients.erase(id);
  } else {
    return false;
  }
}

td::Result<bool> Config::config_del_gc(ton::PublicKeyHash key) {
  return gc.erase(key);
}

class ValidatorElectionBidCreator : public td::actor::Actor {
 public:
  ValidatorElectionBidCreator(td::uint32 date, std::string addr, std::string wallet, std::string dir,
                              std::vector<ton::PublicKeyHash> old_keys, td::actor::ActorId<ValidatorEngine> engine,
                              td::actor::ActorId<ton::keyring::Keyring> keyring, td::Promise<td::BufferSlice> promise)
      : date_(date)
      , addr_(addr)
      , wallet_(wallet)
      , dir_(dir)
      , old_keys_(std::move(old_keys))
      , engine_(engine)
      , keyring_(keyring)
      , promise_(std::move(promise)) {
    ttl_ = date_ + 7 * 86400;
  }

  void start_up() override {
    if (old_keys_.size() > 0) {
      CHECK(old_keys_.size() == 3);

      adnl_addr_ = ton::adnl::AdnlNodeIdShort{old_keys_[2]};
      perm_key_ = old_keys_[0];

      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ton::PublicKey> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::got_perm_public_key, R.move_as_ok());
        }
      });
      td::actor::send_closure(keyring_, &ton::keyring::Keyring::get_public_key, perm_key_, std::move(P));
      return;
    }
    auto pk1 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    perm_key_full_ = pk1.compute_public_key();
    perm_key_ = perm_key_full_.compute_short_id();

    auto pk2 = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    adnl_key_full_ = ton::adnl::AdnlNodeIdFull{pk2.compute_public_key()};
    adnl_addr_ = adnl_key_full_.compute_short_id();

    td::MultiPromise mp;

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::abort_query,
                                R.move_as_error_prefix("keyring fail: "));
      } else {
        td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::written_keys);
      }
    });
    auto ig = mp.init_guard();
    ig.add_promise(std::move(P));

    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk1), false, ig.get_promise());
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk2), false, ig.get_promise());
  }

  void written_keys() {
    td::MultiPromise mp;

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::abort_query,
                                R.move_as_error_prefix("update config fail: "));
      } else {
        td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::updated_config);
      }
    });
    auto ig = mp.init_guard();
    ig.add_promise(std::move(P));

    td::actor::send_closure(engine_, &ValidatorEngine::add_key_to_set, perm_key_full_);
    td::actor::send_closure(engine_, &ValidatorEngine::add_key_to_set, adnl_key_full_.pubkey());
    td::actor::send_closure(engine_, &ValidatorEngine::try_add_validator_permanent_key, perm_key_, date_, ttl_,
                            ig.get_promise());
    td::actor::send_closure(engine_, &ValidatorEngine::try_add_validator_temp_key, perm_key_, perm_key_, ttl_,
                            ig.get_promise());
    td::actor::send_closure(engine_, &ValidatorEngine::try_add_adnl_node, adnl_addr_.pubkey_hash(), cat_,
                            ig.get_promise());
    td::actor::send_closure(engine_, &ValidatorEngine::try_add_validator_adnl_addr, perm_key_, adnl_addr_.pubkey_hash(),
                            ttl_, ig.get_promise());
  }

  void got_perm_public_key(ton::PublicKey pub) {
    perm_key_full_ = pub;
    updated_config();
  }

  void updated_config() {
    auto codeR = td::read_file_str(dir_ + "/validator-elect-req.fif");
    if (codeR.is_error()) {
      abort_query(codeR.move_as_error_prefix("fif not found (validator-elect-req.fif)"));
      return;
    }
    auto R = fift::mem_run_fift(codeR.move_as_ok(),
                                {"validator-elect-req.fif", wallet_, td::to_string(date_), td::to_string(frac),
                                 adnl_addr_.bits256_value().to_hex(), "OUTPUT"},
                                dir_ + "/");
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("fift fail (validator-elect-req.fif)"));
      return;
    }

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::abort_query,
                                R.move_as_error_prefix("sign fail: "));
      } else {
        td::actor::send_closure(SelfId, &ValidatorElectionBidCreator::signed_bid, R.move_as_ok());
      }
    });

    auto res = R.move_as_ok();
    auto to_signR = res.source_lookup.read_file("OUTPUT");
    if (to_signR.is_error()) {
      abort_query(td::Status::Error(PSTRING() << "strange error: no to sign file. Output: " << res.output));
      return;
    }

    td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_message, perm_key_,
                            td::BufferSlice{to_signR.move_as_ok().data}, std::move(P));
  }

  void signed_bid(td::BufferSlice signature) {
    signature_ = std::move(signature);

    auto codeR = td::read_file_str(dir_ + "/validator-elect-signed.fif");
    if (codeR.is_error()) {
      abort_query(codeR.move_as_error_prefix("fif not found (validator-elect-req.fif)"));
      return;
    }
    auto R = fift::mem_run_fift(
        codeR.move_as_ok(),
        {"validator-elect-signed.fif", wallet_, td::to_string(date_), td::to_string(frac),
         adnl_addr_.bits256_value().to_hex(), td::base64_encode(perm_key_full_.export_as_slice().as_slice()),
         td::base64_encode(signature_.as_slice()), "OUTPUT"},
        dir_ + "/");
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("fift fail (validator-elect-req.fif)"));
      return;
    }

    auto res = R.move_as_ok();
    auto dataR = res.source_lookup.read_file("OUTPUT");
    if (dataR.is_error()) {
      abort_query(td::Status::Error("strage error: no result boc"));
      return;
    }

    result_ = td::BufferSlice(dataR.move_as_ok().data);
    finish_query();
  }

  void abort_query(td::Status error) {
    promise_.set_value(ValidatorEngine::create_control_query_error(std::move(error)));
    stop();
  }
  void finish_query() {
    promise_.set_value(ton::create_serialize_tl_object<ton::ton_api::engine_validator_electionBid>(
        date_, perm_key_.tl(), adnl_addr_.bits256_value(), std::move(result_)));
    stop();
  }

 private:
  td::uint32 date_;
  std::string addr_;
  std::string wallet_;
  std::string dir_;
  std::vector<ton::PublicKeyHash> old_keys_;
  td::actor::ActorId<ValidatorEngine> engine_;
  td::actor::ActorId<ton::keyring::Keyring> keyring_;

  td::Promise<td::BufferSlice> promise_;

  td::uint32 ttl_;
  AdnlCategory cat_ = 2;
  double frac = 2.7;

  ton::PublicKeyHash perm_key_;
  ton::PublicKey perm_key_full_;
  ton::adnl::AdnlNodeIdShort adnl_addr_;
  ton::adnl::AdnlNodeIdFull adnl_key_full_;

  td::BufferSlice signature_;
  td::BufferSlice result_;
};

class ValidatorProposalVoteCreator : public td::actor::Actor {
 public:
  ValidatorProposalVoteCreator(td::BufferSlice proposal, std::string dir, td::actor::ActorId<ValidatorEngine> engine,
                               td::actor::ActorId<ton::keyring::Keyring> keyring, td::Promise<td::BufferSlice> promise)
      : proposal_(std::move(proposal))
      , dir_(std::move(dir))
      , engine_(engine)
      , keyring_(keyring)
      , promise_(std::move(promise)) {
  }

  void start_up() override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::pair<ton::PublicKey, size_t>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorProposalVoteCreator::abort_query,
                                R.move_as_error_prefix("failed to find self permanent key: "));
      } else {
        auto v = R.move_as_ok();
        td::actor::send_closure(SelfId, &ValidatorProposalVoteCreator::got_id, v.first, v.second);
      }
    });
    td::actor::send_closure(engine_, &ValidatorEngine::get_current_validator_perm_key, std::move(P));
  }

  void got_id(ton::PublicKey pubkey, size_t idx) {
    pubkey_ = std::move(pubkey);
    idx_ = idx;
    auto codeR = td::read_file_str(dir_ + "/config-proposal-vote-req.fif");
    if (codeR.is_error()) {
      abort_query(codeR.move_as_error_prefix("fif not found (validator-elect-req.fif)"));
      return;
    }
    auto data = proposal_.as_slice().str();
    auto R = fift::mem_run_fift(codeR.move_as_ok(), {"config-proposal-vote-req.fif", "-i", td::to_string(idx_), data},
                                dir_ + "/");
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("fift fail (cofig-proposal-vote-req.fif)"));
      return;
    }
    auto res = R.move_as_ok();
    auto to_signR = res.source_lookup.read_file("validator-to-sign.req");
    if (to_signR.is_error()) {
      abort_query(td::Status::Error(PSTRING() << "strange error: no to sign file. Output: " << res.output));
      return;
    }
    auto to_sign = td::BufferSlice{to_signR.move_as_ok().data};

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorProposalVoteCreator::abort_query,
                                R.move_as_error_prefix("sign fail: "));
      } else {
        td::actor::send_closure(SelfId, &ValidatorProposalVoteCreator::signed_vote, R.move_as_ok());
      }
    });

    td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_message, pubkey_.compute_short_id(),
                            std::move(to_sign), std::move(P));
  }

  void signed_vote(td::BufferSlice signature) {
    signature_ = std::move(signature);

    auto codeR = td::read_file_str(dir_ + "/config-proposal-vote-signed.fif");
    if (codeR.is_error()) {
      abort_query(codeR.move_as_error_prefix("fif not found (config-proposal-vote-signed.fif)"));
      return;
    }

    auto key = td::base64_encode(pubkey_.export_as_slice().as_slice());
    auto sig = td::base64_encode(signature_.as_slice());

    auto data = proposal_.as_slice().str();
    auto R = fift::mem_run_fift(
        codeR.move_as_ok(), {"config-proposal-vote-signed.fif", "-i", td::to_string(idx_), data, key, sig}, dir_ + "/");
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("fift fail (config-proposal-vote-signed.fif)"));
      return;
    }

    auto res = R.move_as_ok();
    auto dataR = res.source_lookup.read_file("vote-msg-body.boc");
    if (dataR.is_error()) {
      abort_query(td::Status::Error("strage error: no result boc"));
      return;
    }

    result_ = td::BufferSlice(dataR.move_as_ok().data);
    finish_query();
  }

  void abort_query(td::Status error) {
    promise_.set_value(ValidatorEngine::create_control_query_error(std::move(error)));
    stop();
  }
  void finish_query() {
    promise_.set_value(ton::create_serialize_tl_object<ton::ton_api::engine_validator_proposalVote>(
        pubkey_.compute_short_id().bits256_value(), std::move(result_)));
    stop();
  }

 private:
  td::BufferSlice proposal_;
  std::string dir_;

  ton::PublicKey pubkey_;
  size_t idx_;

  td::BufferSlice signature_;
  td::BufferSlice result_;
  td::actor::ActorId<ValidatorEngine> engine_;
  td::actor::ActorId<ton::keyring::Keyring> keyring_;

  td::Promise<td::BufferSlice> promise_;

  ton::PublicKeyHash perm_key_;
  ton::PublicKey perm_key_full_;
  ton::adnl::AdnlNodeIdShort adnl_addr_;
  ton::adnl::AdnlNodeIdFull adnl_key_full_;
};

class ValidatorPunishVoteCreator : public td::actor::Actor {
 public:
  ValidatorPunishVoteCreator(td::uint32 election_id, td::BufferSlice proposal, std::string dir,
                             td::actor::ActorId<ValidatorEngine> engine,
                             td::actor::ActorId<ton::keyring::Keyring> keyring, td::Promise<td::BufferSlice> promise)
      : election_id_(election_id)
      , proposal_(std::move(proposal))
      , dir_(std::move(dir))
      , engine_(engine)
      , keyring_(keyring)
      , promise_(std::move(promise)) {
  }

  void start_up() override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<std::pair<ton::PublicKey, size_t>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorPunishVoteCreator::abort_query,
                                R.move_as_error_prefix("failed to find self permanent key: "));
      } else {
        auto v = R.move_as_ok();
        td::actor::send_closure(SelfId, &ValidatorPunishVoteCreator::got_id, v.first, v.second);
      }
    });
    td::actor::send_closure(engine_, &ValidatorEngine::get_current_validator_perm_key, std::move(P));
  }

  void got_id(ton::PublicKey pubkey, size_t idx) {
    pubkey_ = std::move(pubkey);
    idx_ = idx;
    auto codeR = td::read_file_str(dir_ + "/complaint-vote-req.fif");
    if (codeR.is_error()) {
      abort_query(codeR.move_as_error_prefix("fif not found (complaint-vote-req.fif)"));
      return;
    }
    auto data = proposal_.as_slice().str();
    auto R = fift::mem_run_fift(codeR.move_as_ok(),
                                {"complaint-vote-req.fif", td::to_string(idx_), td::to_string(election_id_), data},
                                dir_ + "/");
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("fift fail (complaint-vote-req.fif)"));
      return;
    }
    auto res = R.move_as_ok();
    auto to_signR = res.source_lookup.read_file("validator-to-sign.req");
    if (to_signR.is_error()) {
      abort_query(td::Status::Error(PSTRING() << "strange error: no to sign file. Output: " << res.output));
      return;
    }
    auto to_sign = td::BufferSlice{to_signR.move_as_ok().data};

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ValidatorPunishVoteCreator::abort_query,
                                R.move_as_error_prefix("sign fail: "));
      } else {
        td::actor::send_closure(SelfId, &ValidatorPunishVoteCreator::signed_vote, R.move_as_ok());
      }
    });

    td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_message, pubkey_.compute_short_id(),
                            std::move(to_sign), std::move(P));
  }

  void signed_vote(td::BufferSlice signature) {
    signature_ = std::move(signature);

    auto codeR = td::read_file_str(dir_ + "/complaint-vote-signed.fif");
    if (codeR.is_error()) {
      abort_query(codeR.move_as_error_prefix("fif not found (complaint-vote-signed.fif)"));
      return;
    }

    auto key = td::base64_encode(pubkey_.export_as_slice().as_slice());
    auto sig = td::base64_encode(signature_.as_slice());

    auto data = proposal_.as_slice().str();
    auto R = fift::mem_run_fift(
        codeR.move_as_ok(),
        {"complaint-vote-signed.fif", td::to_string(idx_), td::to_string(election_id_), data, key, sig}, dir_ + "/");
    if (R.is_error()) {
      abort_query(R.move_as_error_prefix("fift fail (complaint-vote-signed.fif)"));
      return;
    }

    auto res = R.move_as_ok();
    auto dataR = res.source_lookup.read_file("vote-query.boc");
    if (dataR.is_error()) {
      abort_query(td::Status::Error("strage error: no result boc"));
      return;
    }

    result_ = td::BufferSlice(dataR.move_as_ok().data);
    finish_query();
  }

  void abort_query(td::Status error) {
    promise_.set_value(ValidatorEngine::create_control_query_error(std::move(error)));
    stop();
  }
  void finish_query() {
    promise_.set_value(ton::create_serialize_tl_object<ton::ton_api::engine_validator_proposalVote>(
        pubkey_.compute_short_id().bits256_value(), std::move(result_)));
    stop();
  }

 private:
  td::uint32 election_id_;
  td::BufferSlice proposal_;
  std::string dir_;

  ton::PublicKey pubkey_;
  size_t idx_;

  td::BufferSlice signature_;
  td::BufferSlice result_;
  td::actor::ActorId<ValidatorEngine> engine_;
  td::actor::ActorId<ton::keyring::Keyring> keyring_;

  td::Promise<td::BufferSlice> promise_;

  ton::PublicKeyHash perm_key_;
  ton::PublicKey perm_key_full_;
  ton::adnl::AdnlNodeIdShort adnl_addr_;
  ton::adnl::AdnlNodeIdFull adnl_key_full_;
};

class CheckDhtServerStatusQuery : public td::actor::Actor {
 public:
  void start_up() override {
    auto &n = dht_config_->nodes();

    result_.resize(n.size(), false);

    pending_ = n.size();
    for (td::uint32 i = 0; i < n.size(); i++) {
      auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), idx = i](td::Result<td::BufferSlice> R) {
        td::actor::send_closure(SelfId, &CheckDhtServerStatusQuery::got_result, idx, R.is_ok());
      });

      auto &E = n.list().at(i);

      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_peer, local_id_, E.adnl_id(), E.addr_list());
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::send_query, local_id_, E.adnl_id().compute_short_id(), "ping",
                              std::move(P), td::Timestamp::in(1.0),
                              ton::create_serialize_tl_object<ton::ton_api::dht_getSignedAddressList>());
    }
  }

  void got_result(td::uint32 idx, bool result) {
    result_[idx] = result;
    CHECK(pending_ > 0);
    if (!--pending_) {
      finish_query();
    }
  }

  void finish_query() {
    std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_dhtServerStatus>> vec;
    auto &n = dht_config_->nodes();
    for (td::uint32 i = 0; i < n.size(); i++) {
      auto &E = n.list().at(i);
      vec.push_back(ton::create_tl_object<ton::ton_api::engine_validator_dhtServerStatus>(
          E.adnl_id().compute_short_id().bits256_value(), result_[i] ? 1 : 0));
    }
    promise_.set_value(
        ton::create_serialize_tl_object<ton::ton_api::engine_validator_dhtServersStatus>(std::move(vec)));
    stop();
  }

  CheckDhtServerStatusQuery(std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config, ton::adnl::AdnlNodeIdShort local_id,
                            td::actor::ActorId<ton::adnl::Adnl> adnl, td::Promise<td::BufferSlice> promise)
      : dht_config_(std::move(dht_config)), local_id_(local_id), adnl_(adnl), promise_(std::move(promise)) {
  }

 private:
  std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config_;

  std::vector<bool> result_;
  td::uint32 pending_;

  ton::adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorId<ton::adnl::Adnl> adnl_;
  td::Promise<td::BufferSlice> promise_;
};

void ValidatorEngine::set_local_config(std::string str) {
  local_config_ = str;
}
void ValidatorEngine::set_global_config(std::string str) {
  global_config_ = str;
}
void ValidatorEngine::set_db_root(std::string db_root) {
  db_root_ = db_root;
}
void ValidatorEngine::start_up() {
  alarm_timestamp() = td::Timestamp::in(1.0 + td::Random::fast(0, 100) * 0.01);
}

void ValidatorEngine::alarm() {
  alarm_timestamp() = td::Timestamp::in(1.0 + td::Random::fast(0, 100) * 0.01);

  if (started_) {
    if (!validator_manager_.empty()) {
      auto P = td::PromiseCreator::lambda(
          [SelfId = actor_id(this)](td::Result<td::Ref<ton::validator::MasterchainState>> R) {
            if (R.is_ok()) {
              td::actor::send_closure(SelfId, &ValidatorEngine::got_state, R.move_as_ok());
            }
          });
      td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::get_top_masterchain_state,
                              std::move(P));
    }
    if (state_.not_null()) {
      bool need_write = false;

      auto configR = state_->get_config_holder();
      configR.ensure();
      auto config = configR.move_as_ok();
      auto cur_t = config->get_validator_set_start_stop(0);
      CHECK(cur_t.first > 0);

      auto val_set = state_->get_total_validator_set(0);
      auto e = val_set->export_vector();
      std::set<ton::PublicKeyHash> to_del;
      for (auto &val : config_.validators) {
        bool is_validator = false;
        if (val_set->is_validator(ton::NodeIdShort{val.first.bits256_value()})) {
          is_validator = true;
        }
        if (!is_validator && val.second.election_date < cur_t.first && cur_t.first + 600 < state_->get_unix_time()) {
          to_del.insert(val.first);
          continue;
        }
      }
      for (auto &x : to_del) {
        config_.config_del_validator_permanent_key(x);
        if (!validator_manager_.empty()) {
          td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::del_permanent_key, x,
                                  [](td::Unit) {});
        }
        if (!full_node_.empty()) {
          td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::del_permanent_key, x,
                                  [](td::Unit) {});
        }
        need_write = true;
      }

      if (need_write) {
        write_config([](td::Unit) {});
      }
    }
    for (auto &x : config_.gc) {
      if (running_gc_.count(x) == 0) {
        running_gc_.insert(x);
        keys_.erase(x);

        auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), x](td::Result<td::Unit> R) {
          R.ensure();
          td::actor::send_closure(SelfId, &ValidatorEngine::deleted_key, x);
        });
        td::actor::send_closure(keyring_, &ton::keyring::Keyring::del_key, x, std::move(P));
      }
    }
  }
}

void ValidatorEngine::deleted_key(ton::PublicKeyHash x) {
  CHECK(running_gc_.count(x) == 1);
  running_gc_.erase(x);
  auto R = config_.config_del_gc(x);
  R.ensure();
  if (R.move_as_ok()) {
    write_config([](td::Unit) {});
  }
}

td::Status ValidatorEngine::load_global_config() {
  TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
  TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

  ton::ton_api::config_global conf;
  TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

  // TODO
  // add adnl static nodes
  if (conf.adnl_) {
    if (conf.adnl_->static_nodes_) {
      TRY_RESULT_PREFIX_ASSIGN(adnl_static_nodes_, ton::adnl::AdnlNodesList::create(conf.adnl_->static_nodes_),
                               "bad static adnl nodes: ");
    }
  }
  if (!conf.dht_) {
    return td::Status::Error(ton::ErrorCode::error, "does not contain [dht] section");
  }

  TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
  dht_config_ = std::move(dht);

  if (!conf.validator_) {
    return td::Status::Error(ton::ErrorCode::error, "does not contain [validator] section");
  }

  if (!conf.validator_->zero_state_) {
    return td::Status::Error(ton::ErrorCode::error, "[validator] section does not contain [zero_state]");
  }

  auto zero_state = ton::create_block_id(conf.validator_->zero_state_);
  if (zero_state.id.workchain != ton::masterchainId || zero_state.id.shard != ton::shardIdAll ||
      zero_state.id.seqno != 0) {
    return td::Status::Error(ton::ErrorCode::error, "[validator] section contains invalid [zero_state]");
  }
  if (zero_state.root_hash.is_zero() || zero_state.file_hash.is_zero()) {
    return td::Status::Error(ton::ErrorCode::error, "[validator] section contains incomplete [zero_state]");
  }

  ton::BlockIdExt init_block;
  if (!conf.validator_->init_block_) {
    LOG(INFO) << "no init block in config. using zero state";
    init_block = zero_state;
  } else {
    init_block = ton::create_block_id(conf.validator_->init_block_);
    LOG(INFO) << "found init block " << init_block;
    if (init_block.id.workchain != ton::masterchainId || init_block.id.shard != ton::shardIdAll) {
      return td::Status::Error(ton::ErrorCode::error, "[validator] section contains invalid [init_block]");
    }
    if (init_block.root_hash.is_zero() || init_block.file_hash.is_zero()) {
      return td::Status::Error(ton::ErrorCode::error, "[validator] section contains incomplete [init_block]");
    }
  }

  validator_options_ = ton::validator::ValidatorManagerOptions::create(zero_state, init_block);
  validator_options_.write().set_shard_check_function(
      [](ton::ShardIdFull shard, ton::CatchainSeqno cc_seqno,
         ton::validator::ValidatorManagerOptions::ShardCheckMode mode) -> bool {
        if (mode == ton::validator::ValidatorManagerOptions::ShardCheckMode::m_monitor) {
          return true;
        }
        CHECK(mode == ton::validator::ValidatorManagerOptions::ShardCheckMode::m_validate);
        return true;
        /*ton::ShardIdFull p{ton::basechainId, ((cc_seqno * 1ull % 4) << 62) + 1};
        auto s = ton::shard_prefix(p, 2);
        return shard.is_masterchain() || ton::shard_intersects(shard, s);*/
      });
  if (state_ttl_ != 0) {
    validator_options_.write().set_state_ttl(state_ttl_);
  }
  if (max_mempool_num_ != 0) {
    validator_options_.write().set_max_mempool_num(max_mempool_num_);
  }
  if (block_ttl_ != 0) {
    validator_options_.write().set_block_ttl(block_ttl_);
  }
  if (sync_ttl_ != 0) {
    validator_options_.write().set_sync_blocks_before(sync_ttl_);
  }
  if (archive_ttl_ != 0) {
    validator_options_.write().set_archive_ttl(archive_ttl_);
  }
  if (key_proof_ttl_ != 0) {
    validator_options_.write().set_key_proof_ttl(key_proof_ttl_);
  }
  for (auto seq : unsafe_catchains_) {
    validator_options_.write().add_unsafe_resync_catchain(seq);
  }
  for (auto rot : unsafe_catchain_rotations_) {
    validator_options_.write().add_unsafe_catchain_rotate(rot.first, rot.second.first, rot.second.second);
  }
  if (truncate_seqno_ > 0) {
    validator_options_.write().truncate_db(truncate_seqno_);
  }
  if (!session_logs_file_.empty()) {
    validator_options_.write().set_session_logs_file(session_logs_file_);
  }

  std::vector<ton::BlockIdExt> h;
  for (auto &x : conf.validator_->hardforks_) {
    auto b = ton::create_block_id(x);
    if (!b.is_masterchain()) {
      return td::Status::Error(ton::ErrorCode::error,
                               "[validator/hardforks] section contains not masterchain block id");
    }
    if (!b.is_valid_full()) {
      return td::Status::Error(ton::ErrorCode::error, "[validator/hardforks] section contains invalid block_id");
    }
    for (auto &y : h) {
      if (y.is_valid() && y.seqno() >= b.seqno()) {
        y.invalidate();
      }
    }
    h.push_back(b);
  }
  validator_options_.write().set_hardforks(std::move(h));

  return td::Status::OK();
}

void ValidatorEngine::load_empty_local_config(td::Promise<td::Unit> promise) {
  auto ret_promise = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorEngine::write_config, std::move(promise));
        }
      });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(ret_promise));

  for (auto &addr : addrs_) {
    config_
        .config_add_network_addr(addr, addr, nullptr, std::vector<AdnlCategory>{0, 1, 2, 3},
                                 std::vector<AdnlCategory>{})
        .ensure();
  }

  {
    auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
    auto id = pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, ig.get_promise());
    config_.config_add_adnl_addr(id, 0).ensure();
    config_.config_add_dht_node(id).ensure();
  }

  {
    auto adnl_pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    keys_.emplace(adnl_pk.compute_short_id(), adnl_pk.compute_public_key());
    auto adnl_short_id = adnl_pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(adnl_pk), false, ig.get_promise());
    config_.config_add_adnl_addr(adnl_short_id, 1).ensure();
    config_.config_add_full_node_adnl_id(adnl_short_id).ensure();
  }
}

void ValidatorEngine::load_local_config(td::Promise<td::Unit> promise) {
  if (local_config_.size() == 0) {
    load_empty_local_config(std::move(promise));
    return;
  }
  auto conf_data_R = td::read_file(local_config_);
  if (conf_data_R.is_error()) {
    promise.set_error(conf_data_R.move_as_error_prefix("failed to read: "));
    return;
  }
  auto conf_data = conf_data_R.move_as_ok();
  auto conf_json_R = td::json_decode(conf_data.as_slice());
  if (conf_json_R.is_error()) {
    promise.set_error(conf_data_R.move_as_error_prefix("failed to parse json: "));
    return;
  }
  auto conf_json = conf_json_R.move_as_ok();

  ton::ton_api::config_local conf;
  auto S = ton::ton_api::from_json(conf, conf_json.get_object());
  if (S.is_error()) {
    promise.set_error(S.move_as_error_prefix("json does not fit TL scheme"));
    return;
  }

  auto ret_promise = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorEngine::write_config, std::move(promise));
        }
      });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(ret_promise));

  for (auto &addr : addrs_) {
    config_
        .config_add_network_addr(addr, addr, nullptr, std::vector<AdnlCategory>{0, 1, 2, 3},
                                 std::vector<AdnlCategory>{})
        .ensure();
  }

  for (auto &local_id : conf.local_ids_) {
    ton::PrivateKey pk{local_id->id_};
    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, ig.get_promise());
  }

  td::uint32 max_time = 2000000000;

  if (conf.dht_.size() > 0) {
    for (auto &d : conf.dht_) {
      ton::ton_api::downcast_call(*d.get(), td::overloaded(
                                                [&](ton::ton_api::dht_config_local &obj) {
                                                  auto node_id = ton::adnl::AdnlNodeIdShort{obj.id_->id_};
                                                  auto it = keys_.find(node_id.pubkey_hash());
                                                  if (it == keys_.end()) {
                                                    ig.get_promise().set_error(td::Status::Error(
                                                        ton::ErrorCode::error, "cannot find private key for dht"));
                                                    return;
                                                  }

                                                  config_.config_add_adnl_addr(node_id.pubkey_hash(), 0).ensure();
                                                  config_.config_add_dht_node(node_id.pubkey_hash()).ensure();
                                                },
                                                [&](ton::ton_api::dht_config_random_local &obj) {
                                                  for (td::int32 i = 0; i < obj.cnt_; i++) {
                                                    auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
                                                    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
                                                    auto id = pk.compute_short_id();
                                                    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key,
                                                                            std::move(pk), false, ig.get_promise());
                                                    config_.config_add_adnl_addr(id, 0).ensure();
                                                    config_.config_add_dht_node(id).ensure();
                                                  }
                                                }));
    }
  } else {
    auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
    auto id = pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, ig.get_promise());
    config_.config_add_adnl_addr(id, 0).ensure();
    config_.config_add_dht_node(id).ensure();
  }

  if (conf.validators_.size() > 0) {
    for (auto &val : conf.validators_) {
      ton::ton_api::downcast_call(
          *val.get(), td::overloaded(
                          [&](ton::ton_api::validator_config_local &obj) {
                            auto id = ton::PublicKeyHash{obj.id_->id_};
                            auto it = keys_.find(id);
                            if (it == keys_.end()) {
                              ig.get_promise().set_error(
                                  td::Status::Error(ton::ErrorCode::error, "cannot find private key for dht"));
                              return;
                            }

                            config_.config_add_adnl_addr(id, 2).ensure();
                            config_.config_add_validator_permanent_key(id, 0, max_time).ensure();
                            config_.config_add_validator_temp_key(id, id, max_time).ensure();
                            config_.config_add_validator_adnl_id(id, id, max_time).ensure();
                          },
                          [&](ton::ton_api::validator_config_random_local &obj) {
                            auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
                            keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
                            auto id = pk.compute_short_id();
                            td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false,
                                                    ig.get_promise());

                            config_.config_add_adnl_addr(id, 2).ensure();
                            config_.config_add_validator_permanent_key(id, 0, max_time).ensure();
                            config_.config_add_validator_temp_key(id, id, max_time).ensure();
                            config_.config_add_validator_adnl_id(id, id, max_time).ensure();
                          }));
    }
  } else {
    // DO NOTHING
  }

  {
    auto adnl_pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    keys_.emplace(adnl_pk.compute_short_id(), adnl_pk.compute_public_key());
    auto adnl_short_id = adnl_pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(adnl_pk), false, ig.get_promise());
    config_.config_add_adnl_addr(adnl_short_id, 1).ensure();
    config_.config_add_full_node_adnl_id(adnl_short_id).ensure();
  }

  for (auto &ls : conf.liteservers_) {
    ton::ton_api::downcast_call(*ls.get(), td::overloaded(
                                               [&](ton::ton_api::liteserver_config_local &cfg) {
                                                 ton::PrivateKey pk{cfg.id_};
                                                 keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
                                                 auto short_id = pk.compute_short_id();
                                                 td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key,
                                                                         std::move(pk), false, ig.get_promise());
                                                 config_.config_add_lite_server(short_id, cfg.port_).ensure();
                                               },
                                               [&](ton::ton_api::liteserver_config_random_local &cfg) {
                                                 auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
                                                 auto short_id = pk.compute_short_id();
                                                 td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key,
                                                                         std::move(pk), false, ig.get_promise());
                                                 config_.config_add_lite_server(short_id, cfg.port_).ensure();
                                               }));
  }

  for (auto &ci : conf.control_) {
    ton::PrivateKey pk{ci->priv_};
    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
    auto short_id = pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, ig.get_promise());

    config_.config_add_control_interface(short_id, ci->port_).ensure();
    config_.config_add_control_process(short_id, ci->port_, ton::PublicKeyHash{ci->pub_}, 0x7fffffff).ensure();
  }
}

void ValidatorEngine::load_config(td::Promise<td::Unit> promise) {
  if (!config_file_.size()) {
    config_file_ = db_root_ + "/config.json";
  }
  auto conf_data_R = td::read_file(config_file_);
  if (conf_data_R.is_error()) {
    auto P = td::PromiseCreator::lambda(
        [name = local_config_, new_name = config_file_, promise = std::move(promise)](td::Result<td::Unit> R) {
          if (R.is_error()) {
            LOG(ERROR) << "failed to parse local config '" << name << "': " << R.move_as_error();
            std::_Exit(2);
          } else {
            LOG(ERROR) << "created config file '" << new_name << "'";
            LOG(ERROR) << "check it manually before continue";
            std::_Exit(0);
          }
        });

    load_local_config(std::move(P));
    return;
  }

  auto conf_data = conf_data_R.move_as_ok();
  auto conf_json_R = td::json_decode(conf_data.as_slice());
  if (conf_json_R.is_error()) {
    promise.set_error(conf_json_R.move_as_error_prefix("failed to parse json: "));
    return;
  }
  auto conf_json = conf_json_R.move_as_ok();

  ton::ton_api::engine_validator_config conf;
  auto S = ton::ton_api::from_json(conf, conf_json.get_object());
  if (S.is_error()) {
    promise.set_error(S.move_as_error_prefix("json does not fit TL scheme"));
    return;
  }

  config_ = Config{conf};

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &key : config_.keys_refcnt) {
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key_short, key.first, get_key_promise(ig));
  }

  write_config(ig.get_promise());
}

void ValidatorEngine::write_config(td::Promise<td::Unit> promise) {
  auto s = td::json_encode<std::string>(td::ToJson(*config_.tl().get()), true);

  auto S = td::write_file(config_file_, s);
  if (S.is_ok()) {
    promise.set_value(td::Unit());
  } else {
    promise.set_error(std::move(S));
  }
}

td::Promise<ton::PublicKey> ValidatorEngine::get_key_promise(td::MultiPromise::InitGuard &ig) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = ig.get_promise()](td::Result<ton::PublicKey> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorEngine::got_key, R.move_as_ok());
          promise.set_value(td::Unit());
        }
      });
  return std::move(P);
}

void ValidatorEngine::got_key(ton::PublicKey key) {
  keys_[key.compute_short_id()] = key;
}

void ValidatorEngine::start() {
  read_config_ = true;
  start_adnl();
}

void ValidatorEngine::start_adnl() {
  adnl_network_manager_ = ton::adnl::AdnlNetworkManager::create(config_.out_port);
  adnl_ = ton::adnl::Adnl::create(db_root_, keyring_.get());
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, adnl_network_manager_.get());

  for (auto &addr : config_.addrs) {
    add_addr(addr.first, addr.second);
  }
  for (auto &adnl : config_.adnl_ids) {
    add_adnl(adnl.first, adnl.second);
  }

  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(adnl_static_nodes_));
  started_adnl();
}

void ValidatorEngine::add_addr(const Config::Addr &addr, const Config::AddrCats &cats) {
  ton::adnl::AdnlCategoryMask cat_mask;
  for (auto cat : cats.cats) {
    cat_mask[cat] = true;
  }
  for (auto cat : cats.priority_cats) {
    cat_mask[cat] = true;
  }
  if (!cats.proxy) {
    td::actor::send_closure(adnl_network_manager_, &ton::adnl::AdnlNetworkManager::add_self_addr, addr.addr,
                            std::move(cat_mask), cats.cats.size() ? 0 : 1);
  } else {
    td::actor::send_closure(adnl_network_manager_, &ton::adnl::AdnlNetworkManager::add_proxy_addr, cats.in_addr,
                            static_cast<td::uint16>(addr.addr.get_port()), cats.proxy, std::move(cat_mask),
                            cats.cats.size() ? 0 : 1);
  }

  td::uint32 ts = static_cast<td::uint32>(td::Clocks::system());

  for (auto cat : cats.cats) {
    ton::adnl::AdnlAddress x = ton::adnl::AdnlAddressImpl::create(
        ton::create_tl_object<ton::ton_api::adnl_address_udp>(cats.in_addr.get_ipv4(), cats.in_addr.get_port()));
    addr_lists_[cat].add_addr(std::move(x));
    addr_lists_[cat].set_version(ts);
    addr_lists_[cat].set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  }
  for (auto cat : cats.priority_cats) {
    ton::adnl::AdnlAddress x = ton::adnl::AdnlAddressImpl::create(
        ton::create_tl_object<ton::ton_api::adnl_address_udp>(cats.in_addr.get_ipv4(), cats.in_addr.get_port()));
    prio_addr_lists_[cat].add_addr(std::move(x));
    prio_addr_lists_[cat].set_version(ts);
    prio_addr_lists_[cat].set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  }
}

void ValidatorEngine::add_adnl(ton::PublicKeyHash id, AdnlCategory cat) {
  CHECK(keys_.count(id) > 0);
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{keys_[id]}, addr_lists_[cat], cat);
}

void ValidatorEngine::started_adnl() {
  start_dht();
}

void ValidatorEngine::add_dht(ton::PublicKeyHash id) {
  auto D = ton::dht::Dht::create(ton::adnl::AdnlNodeIdShort{id}, db_root_, dht_config_, keyring_.get(), adnl_.get());
  D.ensure();

  dht_nodes_[id] = D.move_as_ok();
  if (default_dht_node_.is_zero()) {
    default_dht_node_ = id;
  }
}

void ValidatorEngine::start_dht() {
  for (auto &dht : config_.dht_ids) {
    add_dht(dht);
  }

  if (default_dht_node_.is_zero()) {
    LOG(ERROR) << "trying to work without DHT";
  } else {
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_dht_node, dht_nodes_[default_dht_node_].get());
  }

  started_dht();
}

void ValidatorEngine::started_dht() {
  start_rldp();
}

void ValidatorEngine::start_rldp() {
  rldp_ = ton::rldp::Rldp::create(adnl_.get());
  started_rldp();
}

void ValidatorEngine::started_rldp() {
  start_overlays();
}

void ValidatorEngine::start_overlays() {
  if (!default_dht_node_.is_zero()) {
    overlay_manager_ =
        ton::overlay::Overlays::create(db_root_, keyring_.get(), adnl_.get(), dht_nodes_[default_dht_node_].get());
  }
  started_overlays();
}

void ValidatorEngine::started_overlays() {
  start_validator();
}

void ValidatorEngine::start_validator() {
  validator_options_.write().set_allow_blockchain_init(config_.validators.size() > 0);
  validator_manager_ = ton::validator::ValidatorManagerFactory::create(
      validator_options_, db_root_, keyring_.get(), adnl_.get(), rldp_.get(), overlay_manager_.get());

  for (auto &v : config_.validators) {
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_permanent_key, v.first,
                            [](td::Unit) {});

    for (auto &t : v.second.temp_keys) {
      td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_temp_key, t.first,
                              [](td::Unit) {});
    }
  }

  started_validator();
}

void ValidatorEngine::started_validator() {
  start_full_node();
}

void ValidatorEngine::start_full_node() {
  if (!config_.full_node.is_zero() || config_.full_node_slaves.size() > 0) {
    auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
    auto short_id = pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), true, [](td::Unit) {});
    if (config_.full_node_slaves.size() > 0) {
      std::vector<std::pair<ton::adnl::AdnlNodeIdFull, td::IPAddress>> vec;
      for (auto &x : config_.full_node_slaves) {
        vec.emplace_back(ton::adnl::AdnlNodeIdFull{x.key}, x.addr);
      }
      class Cb : public ton::adnl::AdnlExtClient::Callback {
       public:
        void on_ready() override {
        }
        void on_stop_ready() override {
        }
      };
      full_node_client_ = ton::adnl::AdnlExtMultiClient::create(std::move(vec), std::make_unique<Cb>());
    }
    full_node_ = ton::validator::fullnode::FullNode::create(
        short_id, ton::adnl::AdnlNodeIdShort{config_.full_node}, validator_options_->zero_block_id().file_hash,
        keyring_.get(), adnl_.get(), rldp_.get(),
        default_dht_node_.is_zero() ? td::actor::ActorId<ton::dht::Dht>{} : dht_nodes_[default_dht_node_].get(),
        overlay_manager_.get(), validator_manager_.get(), full_node_client_.get(), db_root_);
  }

  for (auto &v : config_.validators) {
    td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::add_permanent_key, v.first,
                            [](td::Unit) {});
  }

  started_full_node();
}

void ValidatorEngine::started_full_node() {
  start_lite_server();
}

void ValidatorEngine::add_lite_server(ton::PublicKeyHash id, td::uint16 port) {
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{keys_[id]},
                          ton::adnl::AdnlAddressList{}, static_cast<td::uint8>(255));
  td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_ext_server_id,
                          ton::adnl::AdnlNodeIdShort{id});
  td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_ext_server_port, port);
}

void ValidatorEngine::start_lite_server() {
  for (auto &s : config_.liteservers) {
    add_lite_server(s.second, static_cast<td::uint16>(s.first));
  }

  started_lite_server();
}

void ValidatorEngine::started_lite_server() {
  start_control_interface();
}

void ValidatorEngine::add_control_interface(ton::PublicKeyHash id, td::uint16 port) {
  class Callback : public ton::adnl::Adnl::Callback {
   public:
    void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                         td::BufferSlice data) override {
    }
    void receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &ValidatorEngine::process_control_query, port_, src, dst, std::move(data),
                              std::move(promise));
    }

    Callback(td::actor::ActorId<ValidatorEngine> id, td::uint16 port) : id_(id), port_(port) {
    }

   private:
    td::actor::ActorId<ValidatorEngine> id_;
    td::uint16 port_;
  };

  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{keys_[id]},
                          ton::adnl::AdnlAddressList{}, static_cast<td::uint8>(255));
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::subscribe, ton::adnl::AdnlNodeIdShort{id}, std::string(""),
                          std::make_unique<Callback>(actor_id(this), port));
  td::actor::send_closure(control_ext_server_, &ton::adnl::AdnlExtServer::add_local_id, ton::adnl::AdnlNodeIdShort{id});
  td::actor::send_closure(control_ext_server_, &ton::adnl::AdnlExtServer::add_tcp_port, port);
}

void ValidatorEngine::add_control_process(ton::PublicKeyHash id, td::uint16 port, ton::PublicKeyHash pub,
                                          td::int32 permissions) {
  control_permissions_[CI_key{id, port, pub}] |= permissions;
}

void ValidatorEngine::start_control_interface() {
  std::vector<ton::adnl::AdnlNodeIdShort> c_ids;

  std::vector<td::uint16> ports;
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<ton::adnl::AdnlExtServer>> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &ValidatorEngine::started_control_interface, R.move_as_ok());
      });
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::create_ext_server, std::move(c_ids), std::move(ports), std::move(P));
}

void ValidatorEngine::started_control_interface(td::actor::ActorOwn<ton::adnl::AdnlExtServer> control_ext_server) {
  control_ext_server_ = std::move(control_ext_server);
  for (auto &s : config_.controls) {
    add_control_interface(s.second.key, static_cast<td::uint16>(s.first));

    for (auto &p : s.second.clients) {
      add_control_process(s.second.key, static_cast<td::uint16>(s.first), p.first, p.second);
    }
  }
  start_full_node_masters();
}

void ValidatorEngine::start_full_node_masters() {
  for (auto &x : config_.full_node_masters) {
    full_node_masters_.emplace(
        static_cast<td::uint16>(x.first),
        ton::validator::fullnode::FullNodeMaster::create(
            ton::adnl::AdnlNodeIdShort{x.second}, static_cast<td::uint16>(x.first),
            validator_options_->zero_block_id().file_hash, keyring_.get(), adnl_.get(), validator_manager_.get()));
  }
  started_full_node_masters();
}

void ValidatorEngine::started_full_node_masters() {
  started();
}

void ValidatorEngine::started() {
  started_ = true;
}

void ValidatorEngine::try_add_adnl_node(ton::PublicKeyHash key, AdnlCategory cat, td::Promise<td::Unit> promise) {
  if (cat > max_cat()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::protoviolation, "bad category value"));
    return;
  }

  auto R = config_.config_add_adnl_addr(key, cat);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  add_adnl(key, cat);

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_dht_node(ton::PublicKeyHash key_hash, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_dht_node(key_hash);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  add_dht(key_hash);

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_validator_permanent_key(ton::PublicKeyHash key_hash, td::uint32 election_date,
                                                      td::uint32 ttl, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_validator_permanent_key(key_hash, election_date, ttl);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  if (!validator_manager_.empty()) {
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_permanent_key, key_hash,
                            ig.get_promise());
  }
  if (!full_node_.empty()) {
    td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::add_permanent_key, key_hash,
                            ig.get_promise());
  }

  write_config(ig.get_promise());
}

void ValidatorEngine::try_add_validator_temp_key(ton::PublicKeyHash perm_key, ton::PublicKeyHash temp_key,
                                                 td::uint32 ttl, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_validator_temp_key(perm_key, temp_key, ttl);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  if (!validator_manager_.empty()) {
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_temp_key, temp_key,
                            ig.get_promise());
  }
  write_config(ig.get_promise());
}

void ValidatorEngine::try_add_validator_adnl_addr(ton::PublicKeyHash perm_key, ton::PublicKeyHash adnl_id,
                                                  td::uint32 ttl, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_validator_adnl_id(perm_key, adnl_id, ttl);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_full_node_adnl_addr(ton::PublicKeyHash id, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_full_node_adnl_id(id);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  if (!full_node_.empty()) {
    td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::update_adnl_id,
                            ton::adnl::AdnlNodeIdShort{id}, [](td::Unit) {});
  }

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_liteserver(ton::PublicKeyHash id, td::int32 port, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_lite_server(id, port);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  add_lite_server(id, static_cast<td::uint16>(port));

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_control_interface(ton::PublicKeyHash id, td::int32 port, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_control_interface(id, port);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  add_control_interface(id, static_cast<td::uint16>(port));

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_control_process(ton::PublicKeyHash id, td::int32 port, ton::PublicKeyHash pub,
                                              td::int32 permissions, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_control_process(id, port, pub, permissions);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  add_control_process(id, static_cast<td::uint16>(port), pub, permissions);

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_adnl_node(ton::PublicKeyHash pub, td::Promise<td::Unit> promise) {
  auto R = config_.config_del_adnl_addr(pub);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  td::actor::send_closure(adnl_, &ton::adnl::Adnl::del_id, ton::adnl::AdnlNodeIdShort{pub}, [](td::Unit) {});

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_dht_node(ton::PublicKeyHash pub, td::Promise<td::Unit> promise) {
  if (dht_nodes_.size() == 1 && pub == default_dht_node_) {
    promise.set_error(td::Status::Error(ton::ErrorCode::error, "cannot remove last dht node"));
    return;
  }
  auto R = config_.config_del_dht_node(pub);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  if (pub == default_dht_node_) {
    default_dht_node_ = *config_.dht_ids.begin();
    auto d = dht_nodes_[default_dht_node_].get();
    CHECK(!d.empty());
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_dht_node, d);
    td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::update_dht_node, d);
    if (!full_node_.empty()) {
      td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::update_dht_node, d);
    }
  }
  dht_nodes_.erase(pub);

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_validator_permanent_key(ton::PublicKeyHash pub, td::Promise<td::Unit> promise) {
  auto R = config_.config_del_validator_permanent_key(pub);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  if (!validator_manager_.empty()) {
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::del_permanent_key, pub,
                            [](td::Unit) {});
  }
  if (!full_node_.empty()) {
    td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::del_permanent_key, pub, [](td::Unit) {});
  }

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_validator_temp_key(ton::PublicKeyHash perm, ton::PublicKeyHash temp_key,
                                                 td::Promise<td::Unit> promise) {
  auto R = config_.config_del_validator_temp_key(perm, temp_key);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  if (!validator_manager_.empty()) {
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::del_temp_key, temp_key,
                            [](td::Unit) {});
  }

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_validator_adnl_addr(ton::PublicKeyHash perm, ton::PublicKeyHash adnl_id,
                                                  td::Promise<td::Unit> promise) {
  auto R = config_.config_del_validator_adnl_id(perm, adnl_id);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  write_config(std::move(promise));
}

void ValidatorEngine::reload_adnl_addrs() {
  addr_lists_.clear();
  prio_addr_lists_.clear();
  for (auto &addr : config_.addrs) {
    add_addr(addr.first, addr.second);
  }
  for (auto &adnl : config_.adnl_ids) {
    add_adnl(adnl.first, adnl.second);
  }
}

void ValidatorEngine::try_add_listening_port(td::uint32 ip, td::int32 port, std::vector<AdnlCategory> cats,
                                             std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise) {
  td::IPAddress a;
  a.init_ipv4_port(td::IPAddress::ipv4_to_str(ip), static_cast<td::uint16>(port)).ensure();
  auto R = config_.config_add_network_addr(a, a, nullptr, std::move(cats), std::move(prio_cats));
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  reload_adnl_addrs();

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_listening_port(td::uint32 ip, td::int32 port, std::vector<AdnlCategory> cats,
                                             std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise) {
  td::IPAddress a;
  a.init_ipv4_port(td::IPAddress::ipv4_to_str(ip), static_cast<td::uint16>(port)).ensure();
  auto R = config_.config_del_network_addr(a, std::move(cats), std::move(prio_cats));
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  reload_adnl_addrs();

  write_config(std::move(promise));
}

void ValidatorEngine::try_add_proxy(td::uint32 in_ip, td::int32 in_port, td::uint32 out_ip, td::int32 out_port,
                                    std::shared_ptr<ton::adnl::AdnlProxy> proxy, std::vector<AdnlCategory> cats,
                                    std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise) {
  td::IPAddress in_addr;
  in_addr.init_ipv4_port(td::IPAddress::ipv4_to_str(in_ip), static_cast<td::uint16>(in_port)).ensure();
  td::IPAddress out_addr;
  out_addr.init_ipv4_port(td::IPAddress::ipv4_to_str(out_ip), static_cast<td::uint16>(out_port)).ensure();
  auto R = config_.config_add_network_addr(in_addr, out_addr, std::move(proxy), std::move(cats), std::move(prio_cats));
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  reload_adnl_addrs();

  write_config(std::move(promise));
}

void ValidatorEngine::try_del_proxy(td::uint32 ip, td::int32 port, std::vector<AdnlCategory> cats,
                                    std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise) {
  td::IPAddress a;
  a.init_ipv4_port(td::IPAddress::ipv4_to_str(ip), static_cast<td::uint16>(port)).ensure();
  auto R = config_.config_del_network_addr(a, std::move(cats), std::move(prio_cats));
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  reload_adnl_addrs();

  write_config(std::move(promise));
}

void ValidatorEngine::check_key(ton::PublicKeyHash id, td::Promise<td::Unit> promise) {
  if (keys_.count(id) == 1) {
    promise.set_value(td::Unit());
    return;
  }
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<ton::PublicKey> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &ValidatorEngine::got_key, R.move_as_ok());
          promise.set_value(td::Unit());
        }
      });
  td::actor::send_closure(keyring_, &ton::keyring::Keyring::get_public_key, id, std::move(P));
}

td::BufferSlice ValidatorEngine::create_control_query_error(td::Status error) {
  return ton::serialize_tl_object(
      ton::create_tl_object<ton::ton_api::engine_validator_controlQueryError>(error.code(), error.message().str()),
      true);
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_getTime &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  auto obj = ton::create_tl_object<ton::ton_api::engine_validator_time>(static_cast<td::int32>(td::Clocks::system()));
  promise.set_value(ton::serialize_tl_object(obj, true));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_importPrivateKey &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started keyring")));
    return;
  }

  auto pk = ton::PrivateKey{query.key_};
  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), hash = pk.compute_short_id()](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error()));
        } else {
          promise.set_value(
              ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_keyHash>(hash.tl()), true));
        }
      });

  td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_exportPrivateKey &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_unsafe)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started keyring")));
    return;
  }

  promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not implemented")));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_exportPublicKey &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started keyring")));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<ton::PublicKey> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error()));
    } else {
      auto pub = R.move_as_ok();
      promise.set_value(ton::serialize_tl_object(pub.tl(), true));
    }
  });

  td::actor::send_closure(keyring_, &ton::keyring::Keyring::get_public_key, ton::PublicKeyHash{query.key_hash_},
                          std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_generateKeyPair &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started keyring")));
    return;
  }

  auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};

  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), hash = pk.compute_short_id()](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error()));
        } else {
          promise.set_value(
              ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_keyHash>(hash.tl()), true));
        }
      });

  td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addAdnlId &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};
  TRY_RESULT_PROMISE(promise, cat, td::narrow_cast_safe<td::uint8>(query.category_));

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id, cat, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
          return;
        }
        auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add adnl node: ")));
          } else {
            promise.set_value(
                ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
          }
        });
        td::actor::send_closure(SelfId, &ValidatorEngine::try_add_adnl_node, id, cat, std::move(P));
      });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addDhtId &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), id, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
          return;
        }
        auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add dht node: ")));
          } else {
            promise.set_value(
                ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
          }
        });
        td::actor::send_closure(SelfId, &ValidatorEngine::try_add_dht_node, id, std::move(P));
      });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addValidatorPermanentKey &query,
                                        td::BufferSlice data, ton::PublicKeyHash src, td::uint32 perm,
                                        td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id, election_date = query.election_date_,
                                       ttl = query.ttl_, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
      return;
    }
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_value(
            create_control_query_error(R.move_as_error_prefix("failed to add validator permanent key: ")));
      } else {
        promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
      }
    });
    td::actor::send_closure(SelfId, &ValidatorEngine::try_add_validator_permanent_key, id, election_date, ttl,
                            std::move(P));
  });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addValidatorTempKey &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), perm_key = ton::PublicKeyHash{query.permanent_key_hash_}, id,
                                  ttl = query.ttl_, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
          return;
        }
        auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add validator temp key: ")));
          } else {
            promise.set_value(
                ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
          }
        });
        td::actor::send_closure(SelfId, &ValidatorEngine::try_add_validator_temp_key, perm_key, id, ttl, std::move(P));
      });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addValidatorAdnlAddress &query,
                                        td::BufferSlice data, ton::PublicKeyHash src, td::uint32 perm,
                                        td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this),
                                       perm_key = ton::PublicKeyHash{query.permanent_key_hash_}, id, ttl = query.ttl_,
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
      return;
    }
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add validator adnl address: ")));
      } else {
        promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
      }
    });
    td::actor::send_closure(SelfId, &ValidatorEngine::try_add_validator_adnl_addr, perm_key, id, ttl, std::move(P));
  });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_changeFullNodeAdnlAddress &query,
                                        td::BufferSlice data, ton::PublicKeyHash src, td::uint32 perm,
                                        td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.adnl_id_};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id,
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
      return;
    }
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to change full node address: ")));
      } else {
        promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
      }
    });
    td::actor::send_closure(SelfId, &ValidatorEngine::try_add_full_node_adnl_addr, id, std::move(P));
  });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addLiteserver &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id, port = static_cast<td::uint16>(query.port_),
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
      return;
    }
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add liteserver: ")));
      } else {
        promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
      }
    });
    td::actor::send_closure(SelfId, &ValidatorEngine::try_add_liteserver, id, port, std::move(P));
  });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addControlInterface &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id, port = static_cast<td::uint16>(query.port_),
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
      return;
    }
    auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
      if (R.is_error()) {
        promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add control interface: ")));
      } else {
        promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
      }
    });
    td::actor::send_closure(SelfId, &ValidatorEngine::try_add_control_interface, id, port, std::move(P));
  });

  check_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delAdnlId &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del adnl node: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });

  try_del_adnl_node(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delDhtId &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del adnl node: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });
  try_del_dht_node(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delValidatorPermanentKey &query,
                                        td::BufferSlice data, ton::PublicKeyHash src, td::uint32 perm,
                                        td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del validator permanent key: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });
  try_del_validator_permanent_key(id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delValidatorTempKey &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del validator temp key: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });
  try_del_validator_temp_key(ton::PublicKeyHash{query.permanent_key_hash_}, id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delValidatorAdnlAddress &query,
                                        td::BufferSlice data, ton::PublicKeyHash src, td::uint32 perm,
                                        td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto id = ton::PublicKeyHash{query.key_hash_};

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del validator adnl addr: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });
  try_del_validator_adnl_addr(ton::PublicKeyHash{query.permanent_key_hash_}, id, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addListeningPort &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add listening port: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });

  std::vector<td::uint8> cats;
  for (auto cat : query.categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    cats.push_back(c);
  }
  std::vector<td::uint8> prio_cats;
  for (auto cat : query.priority_categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    prio_cats.push_back(c);
  }
  try_add_listening_port(query.ip_, query.port_, std::move(cats), std::move(prio_cats), std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delListeningPort &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del listening port: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });

  std::vector<td::uint8> cats;
  for (auto cat : query.categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    cats.push_back(c);
  }
  std::vector<td::uint8> prio_cats;
  for (auto cat : query.priority_categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    prio_cats.push_back(c);
  }
  try_del_listening_port(query.ip_, query.port_, std::move(cats), std::move(prio_cats), std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_addProxy &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto R = ton::adnl::AdnlProxy::create(*query.proxy_.get());
  if (R.is_error()) {
    promise.set_value(create_control_query_error(R.move_as_error_prefix("bad proxy type: ")));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add listening proxy: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });

  std::vector<td::uint8> cats;
  for (auto cat : query.categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    cats.push_back(c);
  }
  std::vector<td::uint8> prio_cats;
  for (auto cat : query.priority_categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    prio_cats.push_back(c);
  }
  try_add_proxy(query.in_ip_, query.in_port_, query.out_ip_, query.out_port_, R.move_as_ok(), std::move(cats),
                std::move(prio_cats), std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_delProxy &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to del listening proxy: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });

  std::vector<td::uint8> cats;
  for (auto cat : query.categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    cats.push_back(c);
  }
  std::vector<td::uint8> prio_cats;
  for (auto cat : query.priority_categories_) {
    TRY_RESULT_PROMISE(promise, c, td::narrow_cast_safe<td::uint8>(cat));
    prio_cats.push_back(c);
  }

  try_del_proxy(query.out_ip_, query.out_port_, std::move(cats), std::move(prio_cats), std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_getConfig &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }

  auto s = td::json_encode<std::string>(td::ToJson(*config_.tl().get()), true);
  promise.set_value(ton::create_serialize_tl_object<ton::ton_api::engine_validator_jsonConfig>(s));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_sign &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_unsafe)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  LOG(WARNING) << "received sign request: src=" << src.bits256_value().to_hex() << " key=" << query.key_hash_.to_hex()
               << " string=\n"
               << td::base64_encode(query.data_.as_slice());
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error()));
    } else {
      promise.set_value(ton::serialize_tl_object(
          ton::create_tl_object<ton::ton_api::engine_validator_signature>(R.move_as_ok()), true));
    }
  });
  td::actor::send_closure(keyring_, &ton::keyring::Keyring::sign_message, ton::PublicKeyHash{query.key_hash_},
                          std::move(query.data_), std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_setVerbosity &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }

  if (query.verbosity_ < 0 || query.verbosity_ > 10) {
    promise.set_value(
        create_control_query_error(td::Status::Error(ton::ErrorCode::error, "verbosity should be in range [0..10]")));
    return;
  }

  SET_VERBOSITY_LEVEL(VERBOSITY_NAME(ERROR) + query.verbosity_);

  promise.set_value(ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_getStats &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }

  if (validator_manager_.empty()) {
    promise.set_value(
        create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "validator manager not started")));
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise)](td::Result<std::vector<std::pair<std::string, std::string>>> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error()));
        } else {
          auto r = R.move_as_ok();
          std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_oneStat>> vec;
          for (auto &s : r) {
            vec.push_back(ton::create_tl_object<ton::ton_api::engine_validator_oneStat>(s.first, s.second));
          }
          promise.set_value(ton::create_serialize_tl_object<ton::ton_api::engine_validator_stats>(std::move(vec)));
        }
      });
  td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::prepare_stats, std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_createElectionBid &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  if (fift_dir_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "no fift dir")));
    return;
  }

  std::vector<ton::PublicKeyHash> v;
  for (auto &x : config_.validators) {
    if (x.second.election_date == static_cast<ton::UnixTime>(query.election_date_)) {
      if (x.second.temp_keys.size() == 0 || x.second.adnl_ids.size() == 0) {
        promise.set_value(
            create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "prev bid is partial")));
        return;
      }
      v.push_back(x.first);
      v.push_back(x.second.temp_keys.begin()->first);
      v.push_back(x.second.adnl_ids.begin()->first);
    }
  }

  td::actor::create_actor<ValidatorElectionBidCreator>("bidcreate", query.election_date_, query.election_addr_,
                                                       query.wallet_, fift_dir_, std::move(v), actor_id(this),
                                                       keyring_.get(), std::move(promise))
      .release();
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_checkDhtServers &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }
  if (!dht_config_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "no dht config")));
    return;
  }

  if (config_.adnl_ids.count(ton::PublicKeyHash{query.id_}) == 0) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "no dht config")));
    return;
  }

  td::actor::create_actor<CheckDhtServerStatusQuery>("pinger", dht_config_, ton::adnl::AdnlNodeIdShort{query.id_},
                                                     adnl_.get(), std::move(promise))
      .release();
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_createProposalVote &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }

  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  if (fift_dir_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "no fift dir")));
    return;
  }

  td::actor::create_actor<ValidatorProposalVoteCreator>("votecreate", std::move(query.vote_), fift_dir_, actor_id(this),
                                                        keyring_.get(), std::move(promise))
      .release();
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_createComplaintVote &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }

  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  if (fift_dir_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "no fift dir")));
    return;
  }

  td::actor::create_actor<ValidatorPunishVoteCreator>("votecomplaintcreate", query.election_id_, std::move(query.vote_),
                                                      fift_dir_, actor_id(this), keyring_.get(), std::move(promise))
      .release();
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_importCertificate &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }

  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }
  auto r = ton::overlay::Certificate::create(std::move(query.cert_));
  if(r.is_error()) {
    promise.set_value(create_control_query_error(r.move_as_error_prefix("Invalid certificate: ")));
  }
  //TODO force Overlays::update_certificate to return result
  /*auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error()));
        } else {
          promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
        }
      });
  */
  td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::update_certificate,
                            ton::adnl::AdnlNodeIdShort{query.local_id_->id_},
                            ton::overlay::OverlayIdShort{query.overlay_id_},
                            ton::PublicKeyHash{query.signed_key_->key_hash_},
                            r.move_as_ok());
  promise.set_value(
            ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true)
  );
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_importShardOverlayCertificate &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }

  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }
  auto r = ton::overlay::Certificate::create(std::move(query.cert_));
  if(r.is_error()) {
    promise.set_value(create_control_query_error(r.move_as_error_prefix("Invalid certificate: ")));
  }
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to import cert: ")));
    } else {
      promise.set_value(
          ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
    }
  });
  ton::ShardIdFull shard_id{ton::WorkchainId{query.workchain_}, static_cast<ton::ShardId>(query.shard_)};
  td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::import_shard_overlay_certificate,
                            shard_id, ton::PublicKeyHash{query.signed_key_->key_hash_}, r.move_as_ok(), std::move(P));
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_signShardOverlayCertificate &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }

  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }
  ton::ShardIdFull shard_id{ton::WorkchainId{query.workchain_}, static_cast<ton::ShardId>(query.shard_)};
  auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    if (R.is_error()) {
      promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to import cert: ")));
    } else {
      promise.set_value(R.move_as_ok());
    }
  });
  td::actor::send_closure(full_node_, &ton::validator::fullnode::FullNode::sign_shard_overlay_certificate,
                            shard_id, ton::PublicKeyHash{query.signed_key_->key_hash_}, query.expire_at_, query.max_size_, std::move(P));
}


void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_getOverlaysStats &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }

  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "keyring not started")));
    return;
  }

  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  td::actor::send_closure(overlay_manager_, &ton::overlay::Overlays::get_stats,
                          [promise = std::move(promise)](
                              td::Result<ton::tl_object_ptr<ton::ton_api::engine_validator_overlaysStats>> R) mutable {
                            if (R.is_ok()) {
                              promise.set_value(ton::serialize_tl_object(R.move_as_ok(), true));
                            } else {
                              promise.set_value(create_control_query_error(
                                  td::Status::Error(ton::ErrorCode::notready, "overlay manager not ready")));
                            }
                          });
}

void ValidatorEngine::run_control_query(ton::ton_api::engine_validator_getPerfTimerStats &query, td::BufferSlice data,
                                        ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & ValidatorEnginePermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }

  if (validator_manager_.empty()) {
    promise.set_value(
        create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "validator manager not started")));
    return;
  }

  auto P = td::PromiseCreator::lambda(
      [promise = std::move(promise), query = std::move(query)](td::Result<std::vector<ton::validator::PerfTimerStats>> R) mutable {
        const std::vector<int> times{60, 300, 3600};
        double now = td::Time::now();
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error()));
        } else {
          auto r = R.move_as_ok();
          std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_perfTimerStatsByName>> by_name;
          for (const auto &stats : r) {
            if (stats.name == query.name_ || query.name_.empty()) {
              std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_onePerfTimerStat>> by_time;
              for (const auto &t : times) {
                double min = std::numeric_limits<double>::lowest();
                double max = std::numeric_limits<double>::max();
                double sum = 0;
                int cnt = 0;
                for (const auto &stat : stats.stats) {
                  double time = stat.first;
                  double duration = stat.second;
                  if (now - time <= static_cast<double>(t)) {
                    min = td::min<double>(min, duration);
                    max = td::max<double>(max, duration);
                    sum += duration;
                    ++cnt;
                  }
                }
                by_time.push_back(ton::create_tl_object<ton::ton_api::engine_validator_onePerfTimerStat>(t, min, sum / static_cast<double>(cnt), max));
              }
              by_name.push_back(ton::create_tl_object<ton::ton_api::engine_validator_perfTimerStatsByName>(stats.name, std::move(by_time)));
            }
          }
          promise.set_value(ton::create_serialize_tl_object<ton::ton_api::engine_validator_perfTimerStats>(std::move(by_name)));
        }
      });
  td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::prepare_perf_timer_stats, std::move(P));
}

void ValidatorEngine::process_control_query(td::uint16 port, ton::adnl::AdnlNodeIdShort src,
                                            ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                                            td::Promise<td::BufferSlice> promise) {
  auto it = control_permissions_.find(CI_key{dst.pubkey_hash(), port, src.pubkey_hash()});
  if (it == control_permissions_.end()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "forbidden")));
    return;
  }

  auto E = ton::fetch_tl_object<ton::lite_api::liteServer_query>(data.clone(), true);
  if (E.is_ok()) {
    if (!started_) {
      return;
    }
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::run_ext_query,
                            std::move(data), std::move(promise));
    return;
  }

  auto G = ton::fetch_tl_object<ton::ton_api::engine_validator_controlQuery>(std::move(data), true);
  if (G.is_error()) {
    promise.set_value(create_control_query_error(G.move_as_error_prefix("failed to parse validator query: ")));
    return;
  }

  data = std::move(G.move_as_ok()->data_);
  auto F = ton::fetch_tl_object<ton::ton_api::Function>(data.clone(), true);
  if (F.is_error()) {
    promise.set_value(create_control_query_error(F.move_as_error_prefix("failed to parse validator query: ")));
    return;
  }
  auto f = F.move_as_ok();

  ton::ton_api::downcast_call(*f.get(), [&](auto &obj) {
    run_control_query(obj, std::move(data), src.pubkey_hash(), it->second, std::move(promise));
  });
}

void ValidatorEngine::run() {
  td::mkdir(db_root_).ensure();
  ton::errorlog::ErrorLog::create(db_root_);

  auto Sr = load_global_config();
  if (Sr.is_error()) {
    LOG(ERROR) << "failed to load global config'" << global_config_ << "': " << Sr;
    std::_Exit(2);
  }

  keyring_ = ton::keyring::Keyring::create(db_root_ + "/keyring");
  // TODO wait for password
  started_keyring_ = true;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      LOG(ERROR) << "failed to parse config: " << R.move_as_error();
      std::_Exit(2);
    } else {
      td::actor::send_closure(SelfId, &ValidatorEngine::start);
    }
  });
  load_config(std::move(P));
}

void ValidatorEngine::get_current_validator_perm_key(td::Promise<std::pair<ton::PublicKey, size_t>> promise) {
  if (state_.is_null()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::notready, "not started"));
    return;
  }

  auto val_set = state_->get_total_validator_set(0);
  CHECK(val_set.not_null());
  auto vec = val_set->export_vector();
  for (size_t idx = 0; idx < vec.size(); idx++) {
    auto &el = vec[idx];
    ton::PublicKey pub{ton::pubkeys::Ed25519{el.key.as_bits256()}};
    auto pubkey_hash = pub.compute_short_id();

    auto it = config_.validators.find(pubkey_hash);
    if (it != config_.validators.end()) {
      promise.set_value(std::make_pair(pub, idx));
      return;
    }
  }
  promise.set_error(td::Status::Error(ton::ErrorCode::notready, "not a validator"));
}

std::atomic<bool> need_stats_flag{false};
void need_stats(int sig) {
  need_stats_flag.store(true);
}
std::atomic<bool> rotate_logs_flags{false};
void force_rotate_logs(int sig) {
  rotate_logs_flags.store(true);
}
std::atomic<bool> need_scheduler_status_flag{false};
void need_scheduler_status(int sig) {
  need_scheduler_status_flag.store(true);
}

void dump_memory_stats() {
  if (!is_memprof_on()) {
    return;
  }
  LOG(WARNING) << "memory_dump";
  std::vector<AllocInfo> v;
  dump_alloc([&](const AllocInfo &info) { v.push_back(info); });
  std::sort(v.begin(), v.end(), [](const AllocInfo &a, const AllocInfo &b) { return a.size > b.size; });
  size_t total_size = 0;
  size_t other_size = 0;
  int cnt = 0;
  for (auto &info : v) {
    if (cnt++ < 50) {
      LOG(WARNING) << td::format::as_size(info.size) << td::format::as_array(info.backtrace);
    } else {
      other_size += info.size;
    }
    total_size += info.size;
  }
  LOG(WARNING) << td::tag("other", td::format::as_size(other_size));
  LOG(WARNING) << td::tag("total", td::format::as_size(total_size));
  LOG(WARNING) << td::tag("total traces", get_ht_size());
  LOG(WARNING) << td::tag("fast_backtrace_success_rate", get_fast_backtrace_success_rate());
}

void dump_stats() {
  dump_memory_stats();
  LOG(WARNING) << td::NamedThreadSafeCounter::get_default();
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<ValidatorEngine> x;
  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  LOG_STATUS(td::change_maximize_rlimit(td::RlimitType::nofile, 786432));

  std::vector<std::function<void()>> acts;

  td::OptionParser p;
  p.set_description("validator or full node for TON network");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('V', "version", "shows validator-engine build information", [&]() {
    std::cout << "validator-engine build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('C', "global-config", "file to read global config", [&](td::Slice fname) {
    acts.push_back(
        [&x, fname = fname.str()]() { td::actor::send_closure(x, &ValidatorEngine::set_global_config, fname); });
  });
  p.add_option('c', "local-config", "file to read local config", [&](td::Slice fname) {
    acts.push_back(
        [&x, fname = fname.str()]() { td::actor::send_closure(x, &ValidatorEngine::set_local_config, fname); });
  });
  p.add_checked_option('I', "ip", "ip:port of instance", [&](td::Slice arg) {
    td::IPAddress addr;
    TRY_STATUS(addr.init_host_port(arg.str()));
    acts.push_back([&x, addr]() { td::actor::send_closure(x, &ValidatorEngine::add_ip, addr); });
    return td::Status::OK();
  });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) {
    acts.push_back([&x, fname = fname.str()]() { td::actor::send_closure(x, &ValidatorEngine::set_db_root, fname); });
  });
  p.add_option('f', "fift-dir", "directory with fift scripts", [&](td::Slice fname) {
    acts.push_back([&x, fname = fname.str()]() { td::actor::send_closure(x, &ValidatorEngine::set_fift_dir, fname); });
  });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
#if TD_DARWIN || TD_LINUX
    close(0);
    setsid();
#endif
    td::set_signal_handler(td::SignalType::HangUp, force_rotate_logs).ensure();
  });
  std::string session_logs_file;
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    if (session_logs_file.empty()) {
      session_logs_file = fname.str() + ".session-stats";
    }
    logger_ = td::TsFileLog::create(fname.str()).move_as_ok();
    td::log_interface = logger_.get();
  });
  p.add_option('s', "state-ttl", "state will be gc'd after this time (in seconds) default=3600", [&](td::Slice fname) {
    auto v = td::to_double(fname);
    acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_state_ttl, v); });
  });
  p.add_option('m', "mempool-num", "Maximal number of mempool external message", [&](td::Slice fname) {
    auto v = td::to_double(fname);
    acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_max_mempool_num, v); });
  });
  p.add_option('b', "block-ttl", "blocks will be gc'd after this time (in seconds) default=7*86400",
               [&](td::Slice fname) {
                 auto v = td::to_double(fname);
                 acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_block_ttl, v); });
               });
  p.add_option('A', "archive-ttl", "archived blocks will be deleted after this time (in seconds) default=365*86400",
               [&](td::Slice fname) {
                 auto v = td::to_double(fname);
                 acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_archive_ttl, v); });
               });
  p.add_option('K', "key-proof-ttl", "key blocks will be deleted after this time (in seconds) default=365*86400*10",
               [&](td::Slice fname) {
                 auto v = td::to_double(fname);
                 acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_key_proof_ttl, v); });
               });
  p.add_option('S', "sync-before", "in initial sync download all blocks for last given seconds default=3600",
               [&](td::Slice fname) {
                 auto v = td::to_double(fname);
                 acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_sync_ttl, v); });
               });
  p.add_option('T', "truncate-db", "truncate db (with specified seqno as new top masterchain block seqno)",
               [&](td::Slice fname) {
                 auto v = td::to_integer<ton::BlockSeqno>(fname);
                 acts.push_back([&x, v]() { td::actor::send_closure(x, &ValidatorEngine::set_truncate_seqno, v); });
               });
  p.add_option('\0', "session-logs", "file for validator session stats (default: {logname}.session-stats)",
               [&](td::Slice fname) { session_logs_file = fname.str(); });
  acts.push_back([&]() { td::actor::send_closure(x, &ValidatorEngine::set_session_logs_file, session_logs_file); });
  p.add_checked_option(
      'U', "unsafe-catchain-restore", "use SLOW and DANGEROUS catchain recover method", [&](td::Slice id) {
        TRY_RESULT(seq, td::to_integer_safe<ton::CatchainSeqno>(id));
        acts.push_back([&x, seq]() { td::actor::send_closure(x, &ValidatorEngine::add_unsafe_catchain, seq); });
        return td::Status::OK();
      });
  p.add_checked_option('F', "unsafe-catchain-rotate", "use forceful and DANGEROUS catchain rotation",
                       [&](td::Slice params) {
                         auto pos1 = params.find(':');
                         TRY_RESULT(b_seq, td::to_integer_safe<ton::BlockSeqno>(params.substr(0, pos1)));
                         params = params.substr(++pos1, params.size());
                         auto pos2 = params.find(':');
                         TRY_RESULT(cc_seq, td::to_integer_safe<ton::CatchainSeqno>(params.substr(0, pos2)));
                         params = params.substr(++pos2, params.size());
                         auto h = std::stoi(params.substr(0, params.size()).str());
                         acts.push_back([&x, b_seq, cc_seq, h]() {
                           td::actor::send_closure(x, &ValidatorEngine::add_unsafe_catchain_rotation, b_seq, cc_seq, h);
                         });
                         return td::Status::OK();
                       });
  td::uint32 threads = 7;
  p.add_checked_option(
      't', "threads", PSTRING() << "number of threads (default=" << threads << ")", [&](td::Slice fname) {
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
  p.add_checked_option('u', "user", "change user", [&](td::Slice user) { return td::change_user(user.str()); });
  auto S = p.run(argc, argv);
  if (S.is_error()) {
    LOG(ERROR) << "failed to parse options: " << S.move_as_error();
    std::_Exit(2);
  }

  td::set_runtime_signal_handler(1, need_stats).ensure();
  td::set_runtime_signal_handler(2, need_scheduler_status).ensure();

  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({threads});

  scheduler.run_in_context([&] {
    CHECK(vm::init_op_cp0());
    x = td::actor::create_actor<ValidatorEngine>("validator-engine");
    for (auto &act : acts) {
      act();
    }
    acts.clear();
    td::actor::send_closure(x, &ValidatorEngine::run);
  });
  while (scheduler.run(1)) {
    if (need_stats_flag.exchange(false)) {
      dump_stats();
    }
    if (need_scheduler_status_flag.exchange(false)) {
      LOG(ERROR) << "DUMPING SCHEDULER STATISTICS";
      scheduler.get_debug().dump();
    }
    if (rotate_logs_flags.exchange(false)) {
      if (td::log_interface) {
        td::log_interface->rotate();
      }
    }
  }

  return 0;
}
