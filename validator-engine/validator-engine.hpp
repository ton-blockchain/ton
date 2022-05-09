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

#include "adnl/adnl.h"
#include "auto/tl/ton_api.h"
#include "rldp/rldp.h"
#include "dht/dht.h"
#include "validator/manager.h"
#include "validator/validator.h"
#include "validator/full-node.h"
#include "validator/full-node-master.h"
#include "adnl/adnl-ext-client.h"

#include "td/actor/MultiPromise.h"

#include "auto/tl/ton_api_json.h"
#include "auto/tl/ton_api.hpp"

enum ValidatorEnginePermissions : td::uint32 { vep_default = 1, vep_modify = 2, vep_unsafe = 4 };

using AdnlCategory = td::uint8;

struct Config {
  struct Addr {
    td::IPAddress addr;

    bool operator<(const Addr &with) const {
      return addr < with.addr;
    }
  };
  struct AddrCats {
    td::IPAddress in_addr;
    std::shared_ptr<ton::adnl::AdnlProxy> proxy;
    std::set<AdnlCategory> cats;
    std::set<AdnlCategory> priority_cats;
  };
  struct Validator {
    std::map<ton::PublicKeyHash, ton::UnixTime> temp_keys;
    std::map<ton::PublicKeyHash, ton::UnixTime> adnl_ids;
    ton::UnixTime election_date;
    ton::UnixTime expire_at;
  };
  struct Control {
    ton::PublicKeyHash key;
    std::map<ton::PublicKeyHash, td::uint32> clients;
  };
  struct FullNodeSlave {
    ton::PublicKey key;
    td::IPAddress addr;
  };

  std::map<ton::PublicKeyHash, td::uint32> keys_refcnt;
  td::uint16 out_port;
  std::map<Addr, AddrCats> addrs;
  std::map<ton::PublicKeyHash, AdnlCategory> adnl_ids;
  std::set<ton::PublicKeyHash> dht_ids;
  std::map<ton::PublicKeyHash, Validator> validators;
  ton::PublicKeyHash full_node = ton::PublicKeyHash::zero();
  std::vector<FullNodeSlave> full_node_slaves;
  std::map<td::int32, ton::PublicKeyHash> full_node_masters;
  std::map<td::int32, ton::PublicKeyHash> liteservers;
  std::map<td::int32, Control> controls;
  std::set<ton::PublicKeyHash> gc;

  void decref(ton::PublicKeyHash key);
  void incref(ton::PublicKeyHash key) {
    keys_refcnt[key]++;
  }

  td::Result<bool> config_add_network_addr(td::IPAddress in_addr, td::IPAddress out_addr,
                                           std::shared_ptr<ton::adnl::AdnlProxy> proxy, std::vector<AdnlCategory> cats,
                                           std::vector<AdnlCategory> prio_cats);
  td::Result<bool> config_add_adnl_addr(ton::PublicKeyHash addr, AdnlCategory cat);
  td::Result<bool> config_add_dht_node(ton::PublicKeyHash id);
  td::Result<bool> config_add_validator_permanent_key(ton::PublicKeyHash id, ton::UnixTime election_date,
                                                      ton::UnixTime expire_at);
  td::Result<bool> config_add_validator_temp_key(ton::PublicKeyHash perm_key, ton::PublicKeyHash id,
                                                 ton::UnixTime expire_at);
  td::Result<bool> config_add_validator_adnl_id(ton::PublicKeyHash perm_key, ton::PublicKeyHash adnl_id,
                                                ton::UnixTime expire_at);
  td::Result<bool> config_add_full_node_adnl_id(ton::PublicKeyHash id);
  td::Result<bool> config_add_full_node_slave(td::IPAddress addr, ton::PublicKey id);
  td::Result<bool> config_add_full_node_master(td::int32 port, ton::PublicKeyHash id);
  td::Result<bool> config_add_lite_server(ton::PublicKeyHash key, td::int32 port);
  td::Result<bool> config_add_control_interface(ton::PublicKeyHash key, td::int32 port);
  td::Result<bool> config_add_control_process(ton::PublicKeyHash key, td::int32 port, ton::PublicKeyHash id,
                                              td::uint32 permissions);
  td::Result<bool> config_add_gc(ton::PublicKeyHash key);
  td::Result<bool> config_del_network_addr(td::IPAddress addr, std::vector<AdnlCategory> cats,
                                           std::vector<AdnlCategory> prio_cats);
  td::Result<bool> config_del_adnl_addr(ton::PublicKeyHash addr);
  td::Result<bool> config_del_dht_node(ton::PublicKeyHash id);
  td::Result<bool> config_del_validator_permanent_key(ton::PublicKeyHash id);
  td::Result<bool> config_del_validator_temp_key(ton::PublicKeyHash perm_id, ton::PublicKeyHash id);
  td::Result<bool> config_del_validator_adnl_id(ton::PublicKeyHash perm_id, ton::PublicKeyHash adnl_id);
  td::Result<bool> config_del_full_node_adnl_id();
  td::Result<bool> config_del_lite_server(td::int32 port);
  td::Result<bool> config_del_control_interface(td::int32 port);
  td::Result<bool> config_del_control_process(td::int32 port, ton::PublicKeyHash id);
  td::Result<bool> config_del_gc(ton::PublicKeyHash key);

  ton::tl_object_ptr<ton::ton_api::engine_validator_config> tl() const;

  Config();
  Config(ton::ton_api::engine_validator_config &config);
};

class ValidatorEngine : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> adnl_network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp_;
  std::map<ton::PublicKeyHash, td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  ton::PublicKeyHash default_dht_node_ = ton::PublicKeyHash::zero();
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager_;
  td::actor::ActorOwn<ton::validator::ValidatorManagerInterface> validator_manager_;
  td::actor::ActorOwn<ton::adnl::AdnlExtClient> full_node_client_;
  td::actor::ActorOwn<ton::validator::fullnode::FullNode> full_node_;
  std::map<td::uint16, td::actor::ActorOwn<ton::validator::fullnode::FullNodeMaster>> full_node_masters_;
  td::actor::ActorOwn<ton::adnl::AdnlExtServer> control_ext_server_;

  std::string local_config_ = "";
  std::string global_config_ = "ton-global.config";
  std::string config_file_;

  std::string fift_dir_ = "";

  std::string db_root_ = "/var/ton-work/db/";

  std::vector<td::IPAddress> addrs_;
  std::vector<td::IPAddress> proxy_addrs_;

  ton::adnl::AdnlNodesList adnl_static_nodes_;
  std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config_;
  td::Ref<ton::validator::ValidatorManagerOptions> validator_options_;
  Config config_;

  std::set<ton::PublicKeyHash> running_gc_;

  std::map<ton::PublicKeyHash, ton::PublicKey> keys_;

  td::Ref<ton::validator::MasterchainState> state_;

  td::Promise<ton::PublicKey> get_key_promise(td::MultiPromise::InitGuard &ig);
  void got_key(ton::PublicKey key);
  void deleted_key(ton::PublicKeyHash key);
  void got_state(td::Ref<ton::validator::MasterchainState> state) {
    state_ = std::move(state);
  }

  void write_config(td::Promise<td::Unit> promise);

  std::map<td::uint32, ton::adnl::AdnlAddressList> addr_lists_;
  std::map<td::uint32, ton::adnl::AdnlAddressList> prio_addr_lists_;

  struct CI_key {
    ton::PublicKeyHash id;
    td::uint16 port;
    ton::PublicKeyHash pub;
    bool operator<(const CI_key &with) const {
      return id < with.id || (id == with.id && port < with.port) ||
             (id == with.id && port == with.port && pub < with.pub);
    }
  };
  std::map<CI_key, td::uint32> control_permissions_;

  double state_ttl_ = 0;
  double max_mempool_num_ = 0;
  double block_ttl_ = 0;
  double sync_ttl_ = 0;
  double archive_ttl_ = 0;
  double key_proof_ttl_ = 0;
  bool read_config_ = false;
  bool started_keyring_ = false;
  bool started_ = false;
  ton::BlockSeqno truncate_seqno_{0};

  std::set<ton::CatchainSeqno> unsafe_catchains_;
  std::map<ton::BlockSeqno, std::pair<ton::CatchainSeqno, td::uint32>> unsafe_catchain_rotations_;

 public:
  static constexpr td::uint8 max_cat() {
    return 250;
  }

  void add_unsafe_catchain(ton::CatchainSeqno seq) {
    unsafe_catchains_.insert(seq);
  }
  void add_unsafe_catchain_rotation(ton::BlockSeqno b_seqno, ton::CatchainSeqno cc_seqno, td::uint32 value) {
    unsafe_catchain_rotations_.insert({b_seqno, {cc_seqno, value}});
  }
  void set_local_config(std::string str);
  void set_global_config(std::string str);
  void set_fift_dir(std::string str) {
    fift_dir_ = str;
  }
  void set_db_root(std::string db_root);
  void set_state_ttl(double t) {
    state_ttl_ = t;
  }
  void set_max_mempool_num(double t) {
    max_mempool_num_ = t;
  }
  void set_block_ttl(double t) {
    block_ttl_ = t;
  }
  void set_sync_ttl(double t) {
    sync_ttl_ = t;
  }
  void set_archive_ttl(double t) {
    archive_ttl_ = t;
  }
  void set_key_proof_ttl(double t) {
    key_proof_ttl_ = t;
  }
  void set_truncate_seqno(ton::BlockSeqno seqno) {
    truncate_seqno_ = seqno;
  }
  void add_ip(td::IPAddress addr) {
    addrs_.push_back(addr);
  }
  void add_key_to_set(ton::PublicKey key) {
    keys_[key.compute_short_id()] = key;
  }
  void start_up() override;
  void got_result();
  ValidatorEngine() {
  }

  // load config
  td::Status load_global_config();
  void load_empty_local_config(td::Promise<td::Unit> promise);
  void load_local_config(td::Promise<td::Unit> promise);
  void load_config(td::Promise<td::Unit> promise);

  void start();

  void start_adnl();
  void add_addr(const Config::Addr &addr, const Config::AddrCats &cats);
  void add_adnl(ton::PublicKeyHash id, AdnlCategory cat);
  void started_adnl();

  void start_dht();
  void add_dht(ton::PublicKeyHash id);
  void started_dht();

  void start_rldp();
  void started_rldp();

  void start_overlays();
  void started_overlays();

  void start_validator();
  void started_validator();

  void start_full_node();
  void started_full_node();

  void add_lite_server(ton::PublicKeyHash id, td::uint16 port);
  void start_lite_server();
  void started_lite_server();

  void add_control_interface(ton::PublicKeyHash id, td::uint16 port);
  void add_control_process(ton::PublicKeyHash id, td::uint16 port, ton::PublicKeyHash pub, td::int32 permissions);
  void start_control_interface();
  void started_control_interface(td::actor::ActorOwn<ton::adnl::AdnlExtServer> control_ext_server);

  void start_full_node_masters();
  void started_full_node_masters();

  void started();

  void alarm() override;
  void run();

  void get_current_validator_perm_key(td::Promise<std::pair<ton::PublicKey, size_t>> promise);

  void try_add_adnl_node(ton::PublicKeyHash pub, AdnlCategory cat, td::Promise<td::Unit> promise);
  void try_add_dht_node(ton::PublicKeyHash pub, td::Promise<td::Unit> promise);
  void try_add_validator_permanent_key(ton::PublicKeyHash key_hash, td::uint32 election_date, td::uint32 ttl,
                                       td::Promise<td::Unit> promise);
  void try_add_validator_temp_key(ton::PublicKeyHash perm_key, ton::PublicKeyHash temp_key, td::uint32 ttl,
                                  td::Promise<td::Unit> promise);
  void try_add_validator_adnl_addr(ton::PublicKeyHash perm_key, ton::PublicKeyHash adnl_id, td::uint32 ttl,
                                   td::Promise<td::Unit> promise);
  void try_add_full_node_adnl_addr(ton::PublicKeyHash id, td::Promise<td::Unit> promise);
  void try_add_liteserver(ton::PublicKeyHash id, td::int32 port, td::Promise<td::Unit> promise);
  void try_add_control_interface(ton::PublicKeyHash id, td::int32 port, td::Promise<td::Unit> promise);
  void try_add_control_process(ton::PublicKeyHash id, td::int32 port, ton::PublicKeyHash pub, td::int32 permissions,
                               td::Promise<td::Unit> promise);
  void try_del_adnl_node(ton::PublicKeyHash pub, td::Promise<td::Unit> promise);
  void try_del_dht_node(ton::PublicKeyHash pub, td::Promise<td::Unit> promise);
  void try_del_validator_permanent_key(ton::PublicKeyHash pub, td::Promise<td::Unit> promise);
  void try_del_validator_temp_key(ton::PublicKeyHash perm, ton::PublicKeyHash temp_key, td::Promise<td::Unit> promise);
  void try_del_validator_adnl_addr(ton::PublicKeyHash perm, ton::PublicKeyHash adnl_id, td::Promise<td::Unit> promise);

  void reload_adnl_addrs();
  void try_add_listening_port(td::uint32 ip, td::int32 port, std::vector<AdnlCategory> cats,
                              std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise);
  void try_del_listening_port(td::uint32 ip, td::int32 port, std::vector<AdnlCategory> cats,
                              std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise);
  void try_add_proxy(td::uint32 in_ip, td::int32 in_port, td::uint32 out_ip, td::int32 out_port,
                     std::shared_ptr<ton::adnl::AdnlProxy> proxy, std::vector<AdnlCategory> cats,
                     std::vector<AdnlCategory> prio_cats, td::Promise<td::Unit> promise);
  void try_del_proxy(td::uint32 ip, td::int32 port, std::vector<AdnlCategory> cats, std::vector<AdnlCategory> prio_cats,
                     td::Promise<td::Unit> promise);

  void check_key(ton::PublicKeyHash id, td::Promise<td::Unit> promise);

  static td::BufferSlice create_control_query_error(td::Status error);

  void run_control_query(ton::ton_api::engine_validator_getTime &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_importPrivateKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_exportPrivateKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_exportPublicKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_generateKeyPair &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addAdnlId &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addDhtId &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addValidatorPermanentKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addValidatorTempKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addValidatorAdnlAddress &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_changeFullNodeAdnlAddress &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addLiteserver &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addControlInterface &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delAdnlId &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delDhtId &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delValidatorPermanentKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delValidatorTempKey &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delValidatorAdnlAddress &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addListeningPort &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delListeningPort &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addProxy &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delProxy &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getConfig &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_sign &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_setVerbosity &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getStats &query, td::BufferSlice data, ton::PublicKeyHash src,
                         td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_createElectionBid &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_checkDhtServers &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_createProposalVote &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_createComplaintVote &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_importCertificate &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_signShardOverlayCertificate &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_importShardOverlayCertificate &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getOverlaysStats &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  template <class T>
  void run_control_query(T &query, td::BufferSlice data, ton::PublicKeyHash src, td::uint32 perm,
                         td::Promise<td::BufferSlice> promise) {
    promise.set_value(
        create_control_query_error(td::Status::Error(ton::ErrorCode::protoviolation, "query not supported")));
  }
  void process_control_query(td::uint16 port, ton::adnl::AdnlNodeIdShort src, ton::adnl::AdnlNodeIdShort dst,
                             td::BufferSlice data, td::Promise<td::BufferSlice> promise);
};
