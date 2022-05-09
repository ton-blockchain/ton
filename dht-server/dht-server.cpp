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
#include "dht-server.hpp"

#include "td/utils/filesystem.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/overloaded.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/path.h"
#include "td/utils/port/user.h"
#include "td/utils/port/signals.h"
#include "td/utils/ThreadSafeCounter.h"
#include "td/utils/TsFileLog.h"
#include "td/utils/Random.h"

#include "memprof/memprof.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <set>
#include "git.h"

Config::Config() {
  out_port = 3278;
}

Config::Config(ton::ton_api::engine_validator_config &config) {
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
              for (auto &cat : obj.categories_) {
                categories.push_back(td::narrow_cast<td::uint8>(cat));
              }
              for (auto &cat : obj.priority_categories_) {
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
                for (auto &cat : obj.categories_) {
                  categories.push_back(td::narrow_cast<td::uint8>(cat));
                }
                for (auto &cat : obj.priority_categories_) {
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

  std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_fullNodeSlave>> full_node_slaves_vec;
  std::vector<ton::tl_object_ptr<ton::ton_api::engine_validator_fullNodeMaster>> full_node_masters_vec;

  std::vector<ton::tl_object_ptr<ton::ton_api::engine_liteServer>> liteserver_vec;

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
      out_port, std::move(addrs_vec), std::move(adnl_vec), std::move(dht_vec), std::move(val_vec),
      ton::PublicKeyHash::zero().tl(), std::move(full_node_slaves_vec), std::move(full_node_masters_vec),
      std::move(liteserver_vec), std::move(control_vec), std::move(gc_vec));
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

td::Result<bool> Config::config_add_control_interface(ton::PublicKeyHash key, td::int32 port) {
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

void DhtServer::set_local_config(std::string str) {
  local_config_ = str;
}
void DhtServer::set_global_config(std::string str) {
  global_config_ = str;
}
void DhtServer::set_db_root(std::string db_root) {
  db_root_ = db_root;
}
void DhtServer::start_up() {
  alarm_timestamp() = td::Timestamp::in(1.0 + td::Random::fast(0, 100) * 0.01);
}

void DhtServer::alarm() {
  alarm_timestamp() = td::Timestamp::in(1.0 + td::Random::fast(0, 100) * 0.01);

  if (started_) {
    for (auto &x : config_.gc) {
      if (running_gc_.count(x) == 0) {
        running_gc_.insert(x);

        auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), x](td::Result<td::Unit> R) {
          R.ensure();
          td::actor::send_closure(SelfId, &DhtServer::deleted_key, x);
        });
        td::actor::send_closure(keyring_, &ton::keyring::Keyring::del_key, x, std::move(P));
      }
    }
  }
}

void DhtServer::deleted_key(ton::PublicKeyHash x) {
  CHECK(running_gc_.count(x) == 1);
  running_gc_.erase(x);
  auto R = config_.config_del_gc(x);
  R.ensure();
  if (R.move_as_ok()) {
    write_config([](td::Unit) {});
  }
}

td::Status DhtServer::load_global_config() {
  TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
  TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

  ton::ton_api::config_global conf;
  TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

  // TODO
  // add adnl static nodes
  //if (conf.adnl_) {
  //  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config,
  //                          std::move(conf.adnl_->static_nodes_));
  //}
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

  return td::Status::OK();
}

void DhtServer::load_empty_local_config(td::Promise<td::Unit> promise) {
  auto ret_promise = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::Unit> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &DhtServer::write_config, std::move(promise));
        }
      });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(ret_promise));

  for (auto &addr : addrs_) {
    config_.config_add_network_addr(addr, addr, nullptr, std::vector<td::int8>{0, 1, 2, 3}, std::vector<td::int8>{})
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
}

void DhtServer::load_local_config(td::Promise<td::Unit> promise) {
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
          td::actor::send_closure(SelfId, &DhtServer::write_config, std::move(promise));
        }
      });

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(ret_promise));

  for (auto &addr : addrs_) {
    config_.config_add_network_addr(addr, addr, nullptr, std::vector<td::int8>{0, 1, 2, 3}, std::vector<td::int8>{})
        .ensure();
  }

  for (auto &local_id : conf.local_ids_) {
    ton::PrivateKey pk{local_id->id_};
    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, ig.get_promise());
  }

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

  for (auto &ci : conf.control_) {
    ton::PrivateKey pk{ci->priv_};
    keys_.emplace(pk.compute_short_id(), pk.compute_public_key());
    auto short_id = pk.compute_short_id();
    td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), false, ig.get_promise());

    config_.config_add_control_interface(short_id, ci->port_).ensure();
    config_.config_add_control_process(short_id, ci->port_, ton::PublicKeyHash{ci->pub_}, 0x7fffffff).ensure();
  }
}

void DhtServer::load_config(td::Promise<td::Unit> promise) {
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
    promise.set_error(conf_data_R.move_as_error_prefix("failed to parse json: "));
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

void DhtServer::write_config(td::Promise<td::Unit> promise) {
  auto s = td::json_encode<std::string>(td::ToJson(*config_.tl().get()), true);

  auto S = td::write_file(config_file_, s);
  if (S.is_ok()) {
    promise.set_value(td::Unit());
  } else {
    promise.set_error(std::move(S));
  }
}

td::Promise<ton::PublicKey> DhtServer::get_key_promise(td::MultiPromise::InitGuard &ig) {
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = ig.get_promise()](td::Result<ton::PublicKey> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &DhtServer::got_key, R.move_as_ok());
          promise.set_value(td::Unit());
        }
      });
  return std::move(P);
}

void DhtServer::got_key(ton::PublicKey key) {
  keys_[key.compute_short_id()] = key;
}

void DhtServer::start() {
  read_config_ = true;
  start_adnl();
}

void DhtServer::start_adnl() {
  adnl_network_manager_ = ton::adnl::AdnlNetworkManager::create(config_.out_port);
  adnl_ = ton::adnl::Adnl::create(db_root_, keyring_.get());
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_network_manager, adnl_network_manager_.get());

  for (auto &addr : config_.addrs) {
    add_addr(addr.first, addr.second);
  }
  for (auto &adnl : config_.adnl_ids) {
    add_adnl(adnl.first, adnl.second);
  }
  started_adnl();
}

void DhtServer::add_addr(const Config::Addr &addr, const Config::AddrCats &cats) {
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
    CHECK(cat >= 0);
    ton::adnl::AdnlAddress x = ton::adnl::AdnlAddressImpl::create(
        ton::create_tl_object<ton::ton_api::adnl_address_udp>(cats.in_addr.get_ipv4(), cats.in_addr.get_port()));
    addr_lists_[cat].add_addr(std::move(x));
    addr_lists_[cat].set_version(ts);
    addr_lists_[cat].set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  }
  for (auto cat : cats.priority_cats) {
    CHECK(cat >= 0);
    ton::adnl::AdnlAddress x = ton::adnl::AdnlAddressImpl::create(
        ton::create_tl_object<ton::ton_api::adnl_address_udp>(cats.in_addr.get_ipv4(), cats.in_addr.get_port()));
    prio_addr_lists_[cat].add_addr(std::move(x));
    prio_addr_lists_[cat].set_version(ts);
    prio_addr_lists_[cat].set_reinit_date(ton::adnl::Adnl::adnl_start_time());
  }
}

void DhtServer::add_adnl(ton::PublicKeyHash id, AdnlCategory cat) {
  CHECK(addr_lists_[cat].size() > 0);
  CHECK(keys_.count(id) > 0);
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{keys_[id]}, addr_lists_[cat], cat);
}

void DhtServer::started_adnl() {
  start_dht();
}

void DhtServer::start_dht() {
  for (auto &dht : config_.dht_ids) {
    auto D = ton::dht::Dht::create(ton::adnl::AdnlNodeIdShort{dht}, db_root_, dht_config_, keyring_.get(), adnl_.get());
    D.ensure();

    dht_nodes_[dht] = D.move_as_ok();
    if (default_dht_node_.is_zero()) {
      default_dht_node_ = dht;
    }
  }

  CHECK(!default_dht_node_.is_zero());
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::register_dht_node, dht_nodes_[default_dht_node_].get());

  started_dht();
}

void DhtServer::started_dht() {
  start_control_interface();
}

void DhtServer::start_control_interface() {
  class Callback : public ton::adnl::Adnl::Callback {
   public:
    void receive_message(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                         td::BufferSlice data) override {
    }
    void receive_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &DhtServer::process_control_query, src, dst, std::move(data), std::move(promise));
    }

    Callback(td::actor::ActorId<DhtServer> id) : id_(id) {
    }

   private:
    td::actor::ActorId<DhtServer> id_;
  };

  std::vector<ton::adnl::AdnlNodeIdShort> c_ids;
  std::vector<td::uint16> ports;

  for (auto &s : config_.controls) {
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{keys_[s.second.key]},
                            ton::adnl::AdnlAddressList{}, static_cast<td::uint8>(255));
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::subscribe, ton::adnl::AdnlNodeIdShort{s.second.key},
                            std::string(""), std::make_unique<Callback>(actor_id(this)));

    c_ids.push_back(ton::adnl::AdnlNodeIdShort{s.second.key});
    ports.push_back(static_cast<td::uint16>(s.first));

    for (auto &p : s.second.clients) {
      control_permissions_[p.first] |= p.second;
    }
  }

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this)](td::Result<td::actor::ActorOwn<ton::adnl::AdnlExtServer>> R) {
        R.ensure();
        td::actor::send_closure(SelfId, &DhtServer::started_control_interface, R.move_as_ok());
      });
  td::actor::send_closure(adnl_, &ton::adnl::Adnl::create_ext_server, std::move(c_ids), std::move(ports), std::move(P));
}

void DhtServer::started_control_interface(td::actor::ActorOwn<ton::adnl::AdnlExtServer> control_ext_server) {
  control_ext_server_ = std::move(control_ext_server);
  started();
}

void DhtServer::started() {
  started_ = true;
}

void DhtServer::add_adnl_node(ton::PublicKey key, AdnlCategory cat, td::Promise<td::Unit> promise) {
  if (cat < 0 || static_cast<td::uint32>(cat) > max_cat()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::protoviolation, "bad category value"));
    return;
  }

  auto R = config_.config_add_adnl_addr(key.compute_short_id(), cat);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  if (!adnl_.empty()) {
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{key}, addr_lists_[cat], cat);
  }

  write_config(std::move(promise));
}

void DhtServer::add_dht_node(ton::PublicKeyHash key_hash, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_dht_node(key_hash);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  if (dht_nodes_.size() > 0) {
    auto D =
        ton::dht::Dht::create(ton::adnl::AdnlNodeIdShort{key_hash}, db_root_, dht_config_, keyring_.get(), adnl_.get());
    D.ensure();

    dht_nodes_[key_hash] = D.move_as_ok();

    if (default_dht_node_.is_zero()) {
      default_dht_node_ = key_hash;
    }
  }

  write_config(std::move(promise));
}

void DhtServer::add_control_interface(ton::PublicKeyHash id, td::int32 port, td::Promise<td::Unit> promise) {
  auto R = config_.config_add_control_interface(id, port);
  if (R.is_error()) {
    promise.set_error(R.move_as_error());
    return;
  }

  if (!R.move_as_ok()) {
    promise.set_value(td::Unit());
    return;
  }

  td::actor::send_closure(control_ext_server_, &ton::adnl::AdnlExtServer::add_local_id, ton::adnl::AdnlNodeIdShort{id});
  td::actor::send_closure(control_ext_server_, &ton::adnl::AdnlExtServer::add_tcp_port, static_cast<td::uint16>(port));

  write_config(std::move(promise));
}

void DhtServer::add_control_process(ton::PublicKeyHash id, td::int32 port, ton::PublicKeyHash pub,
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

  control_permissions_[pub] |= permissions;

  write_config(std::move(promise));
}

td::BufferSlice DhtServer::create_control_query_error(td::Status error) {
  return ton::serialize_tl_object(
      ton::create_tl_object<ton::ton_api::engine_validator_controlQueryError>(error.code(), error.message().str()),
      true);
}

void DhtServer::run_control_query(ton::ton_api::engine_validator_getTime &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  auto obj = ton::create_tl_object<ton::ton_api::engine_validator_time>(static_cast<td::int32>(td::Clocks::system()));
  promise.set_value(ton::serialize_tl_object(obj, true));
}

void DhtServer::run_control_query(ton::ton_api::engine_validator_importPrivateKey &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_default)) {
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

void DhtServer::run_control_query(ton::ton_api::engine_validator_exportPrivateKey &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_unsafe)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (keyring_.empty()) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started keyring")));
    return;
  }

  promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not implemented")));
}

void DhtServer::run_control_query(ton::ton_api::engine_validator_exportPublicKey &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_default)) {
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

void DhtServer::run_control_query(ton::ton_api::engine_validator_generateKeyPair &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_default)) {
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

void DhtServer::run_control_query(ton::ton_api::engine_validator_addAdnlId &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  TRY_RESULT_PROMISE(promise, cat, td::narrow_cast_safe<td::uint8>(query.category_));

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), cat, promise = std::move(promise)](td::Result<ton::PublicKey> R) mutable {
        if (R.is_error()) {
          promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to get public key: ")));
          return;
        }
        auto pub = R.move_as_ok();
        auto P = td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<td::Unit> R) mutable {
          if (R.is_error()) {
            promise.set_value(create_control_query_error(R.move_as_error_prefix("failed to add adnl node: ")));
          } else {
            promise.set_value(
                ton::serialize_tl_object(ton::create_tl_object<ton::ton_api::engine_validator_success>(), true));
          }
        });
        td::actor::send_closure(SelfId, &DhtServer::add_adnl_node, pub, cat, std::move(P));
      });

  td::actor::send_closure(keyring_, &ton::keyring::Keyring::get_public_key, ton::PublicKeyHash{query.key_hash_},
                          std::move(P));
}

void DhtServer::run_control_query(ton::ton_api::engine_validator_addDhtId &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_modify)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), key_hash = ton::PublicKeyHash{query.key_hash_},
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
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
    td::actor::send_closure(SelfId, &DhtServer::add_dht_node, key_hash, std::move(P));
  });

  td::actor::send_closure(keyring_, &ton::keyring::Keyring::check_key, ton::PublicKeyHash{query.key_hash_},
                          std::move(P));
}

void DhtServer::run_control_query(ton::ton_api::engine_validator_getConfig &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_default)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }

  auto s = td::json_encode<std::string>(td::ToJson(*config_.tl().get()), true);
  promise.set_value(td::BufferSlice{s});
}

void DhtServer::run_control_query(ton::ton_api::engine_validator_sign &query, td::BufferSlice data,
                                  ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise) {
  if (!(perm & DhtServerPermissions::vep_unsafe)) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::error, "not authorized")));
    return;
  }
  if (!started_) {
    promise.set_value(create_control_query_error(td::Status::Error(ton::ErrorCode::notready, "not started")));
    return;
  }

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

void DhtServer::process_control_query(ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                                      td::BufferSlice data, td::Promise<td::BufferSlice> promise) {
  auto G = ton::fetch_tl_object<ton::ton_api::engine_validator_controlQuery>(std::move(data), true);
  if (G.is_error()) {
    promise.set_error(G.move_as_error_prefix("failed to parse validator query: "));
    return;
  }
  data = std::move(G.move_as_ok()->data_);
  auto F = ton::fetch_tl_object<ton::ton_api::Function>(data.clone(), true);
  if (F.is_error()) {
    promise.set_error(F.move_as_error_prefix("failed to parse validator query: "));
    return;
  }
  auto f = F.move_as_ok();

  auto it = control_permissions_.find(src.pubkey_hash());
  if (it == control_permissions_.end()) {
    promise.set_error(td::Status::Error(ton::ErrorCode::protoviolation, "forbidden"));
    return;
  }

  ton::ton_api::downcast_call(*f.get(), [&](auto &obj) {
    run_control_query(obj, std::move(data), src.pubkey_hash(), it->second, std::move(promise));
  });
}

void DhtServer::run() {
  td::mkdir(db_root_).ensure();

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
      td::actor::send_closure(SelfId, &DhtServer::start);
    }
  });
  load_config(std::move(P));
}

std::atomic<bool> need_stats_flag{false};
void need_stats(int sig) {
  need_stats_flag.store(true);
}
std::atomic<bool> rotate_logs_flags{false};
void force_rotate_logs(int sig) {
  rotate_logs_flags.store(true);
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

  td::actor::ActorOwn<DhtServer> x;
  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  std::vector<std::function<void()>> acts;

  td::OptionParser p;
  p.set_description("dht server for TON DHT network");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('V', "version", "shows dht-server build information", [&]() {
    std::cout << "dht-server build information: [ Commit: " << GitMetadata::CommitSHA1() << ", Date: " << GitMetadata::CommitDate() << "]\n";
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
    acts.push_back([&x, fname = fname.str()]() { td::actor::send_closure(x, &DhtServer::set_global_config, fname); });
  });
  p.add_option('c', "local-config", "file to read local config", [&](td::Slice fname) {
    acts.push_back([&x, fname = fname.str()]() { td::actor::send_closure(x, &DhtServer::set_local_config, fname); });
  });
  p.add_checked_option('I', "ip", "ip:port of instance", [&](td::Slice arg) {
    td::IPAddress addr;
    TRY_STATUS(addr.init_host_port(arg.str()));
    acts.push_back([&x, addr]() { td::actor::send_closure(x, &DhtServer::add_ip, addr); });
    return td::Status::OK();
  });
  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) {
    acts.push_back([&x, fname = fname.str()]() { td::actor::send_closure(x, &DhtServer::set_db_root, fname); });
  });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
#if TD_DARWIN || TD_LINUX
    close(0);
    setsid();
#endif
    td::set_signal_handler(td::SignalType::HangUp, force_rotate_logs).ensure();
  });
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    logger_ = td::TsFileLog::create(fname.str()).move_as_ok();
    td::log_interface = logger_.get();
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
  p.run(argc, argv).ensure();

  td::set_runtime_signal_handler(1, need_stats).ensure();

  td::actor::Scheduler scheduler({threads});

  scheduler.run_in_context([&] {
    x = td::actor::create_actor<DhtServer>("dht-server");
    for (auto &act : acts) {
      act();
    }
    acts.clear();
    td::actor::send_closure(x, &DhtServer::run);
  });
  while (scheduler.run(1)) {
    if (need_stats_flag.exchange(false)) {
      dump_stats();
    }
    if (rotate_logs_flags.exchange(false)) {
      if (td::log_interface) {
        td::log_interface->rotate();
      }
    }
  }

  return 0;
}
