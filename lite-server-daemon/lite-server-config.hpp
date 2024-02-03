#include "adnl/adnl.h"
#include "auto/tl/ton_api.h"
#include "rldp/rldp.h"
#include "rldp2/rldp.h"
#include "dht/dht.h"
#include "validator/manager.h"
#include "validator/validator.h"
#include "validator/full-node.h"
#include "validator/full-node-master.h"
#include "adnl/adnl-ext-client.h"

using AdnlCategory = td::uint8;
namespace ton {
namespace liteserver {
class Config {
  struct Addr {
    td::IPAddress addr;

    bool operator<(const Addr &with) const {
      return addr < with.addr;
    }
  };

  void decref(ton::PublicKeyHash key) {
    auto v = keys_refcnt[key]--;
    CHECK(v > 0);
  };

  void incref(ton::PublicKeyHash key) {
    keys_refcnt[key]++;
  };

 public:
  td::IPAddress addr_;
  std::map<ton::PublicKeyHash, AdnlCategory> adnl_ids;
  std::map<td::int32, ton::PublicKeyHash> liteservers;
  std::map<ton::PublicKeyHash, td::uint32> keys_refcnt;

  Config() {
  }

  Config(ton::ton_api::engine_liteserver_config &config) {
    auto tmp = addr_.init_host_port(td::IPAddress::ipv4_to_str(config.ip_), static_cast<td::uint16>(config.out_port_));
    if (tmp.is_error()) {
      LOG(ERROR) << tmp.move_as_error();
      std::_Exit(2);
    }

    for (auto &serv : config.liteservers_) {
      config_add_lite_server(ton::PublicKeyHash{serv->id_}, serv->port_).ensure();
    }

    for (auto &adnl : config.adnl_) {
      config_add_adnl_addr(ton::PublicKeyHash{adnl->id_}, td::narrow_cast<td::uint8>(adnl->category_)).ensure();
    }
  }

  td::Result<bool> config_add_lite_server(ton::PublicKeyHash keyhash, td::int32 port) {
    auto it = liteservers.find(port);
    if (it != liteservers.end()) {
      if (it->second == keyhash) {
        return false;
      } else {
        return td::Status::Error(ton::ErrorCode::error, "duplicate port");
      }
    } else {
      incref(keyhash);
      liteservers.emplace(port, keyhash);
      return true;
    }
  };

  td::Result<bool> set_addr(const std::string &addr) {
    auto s = addr_.init_host_port(addr);
    if (s.is_error()) {
      LOG(ERROR) << s.move_as_error();
      return false;
    } else {
      return true;
    }
  }

  td::Result<bool> set_addr(td::IPAddress addr) {
    addr_ = addr;
    return true;
  }

  td::Result<bool> config_add_adnl_addr(ton::PublicKeyHash keyhash, AdnlCategory cat) {
    auto it = adnl_ids.find(keyhash);
    if (it != adnl_ids.end()) {
      if (it->second != cat) {
        it->second = cat;
        return true;
      } else {
        return false;
      }
    } else {
      incref(keyhash);
      adnl_ids.emplace(keyhash, cat);
      return true;
    }
  }

  ton::tl_object_ptr<ton::ton_api::engine_liteserver_config> tl() const {
    std::vector<ton::tl_object_ptr<ton::ton_api::engine_adnl>> adnl_vec;
    for (auto &x : adnl_ids) {
      adnl_vec.push_back(ton::create_tl_object<ton::ton_api::engine_adnl>(x.first.tl(), x.second));
    }

    std::vector<ton::tl_object_ptr<ton::ton_api::engine_liteServer>> liteserver_vec;
    for (auto &x : liteservers) {
      liteserver_vec.push_back(ton::create_tl_object<ton::ton_api::engine_liteServer>(x.second.tl(), x.first));
    }
    return ton::create_tl_object<ton::ton_api::engine_liteserver_config>(
        addr_.get_ipv4(), addr_.get_port(), std::move(adnl_vec), std::move(liteserver_vec));
  };
};
}  // namespace liteserver
}  // namespace ton