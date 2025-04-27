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
#include "rldp2/rldp.h"
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
  ton::validator::fullnode::FullNodeConfig full_node_config;
  std::map<td::int32, Control> controls;
  std::set<ton::PublicKeyHash> gc;
  std::vector<ton::ShardIdFull> shards_to_monitor;

  bool state_serializer_enabled = true;

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
  td::Result<bool> config_add_shard(ton::ShardIdFull shard);
  td::Result<bool> config_del_shard(ton::ShardIdFull shard);
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
  Config(const ton::ton_api::engine_validator_config &config);
};

class ValidatorEngine : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::keyring::Keyring> keyring_;
  td::actor::ActorOwn<ton::adnl::AdnlNetworkManager> adnl_network_manager_;
  td::actor::ActorOwn<ton::adnl::Adnl> adnl_;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp_;
  td::actor::ActorOwn<ton::rldp2::Rldp> rldp2_;
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
  std::string temp_config_file() const {
    return config_file_ + ".tmp";
  }

  std::string fift_dir_ = "";

  std::string db_root_ = "/var/ton-work/db/";

  std::vector<td::IPAddress> addrs_;
  std::vector<td::IPAddress> proxy_addrs_;

  ton::adnl::AdnlNodesList adnl_static_nodes_;
  std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config_;
  td::Ref<ton::validator::ValidatorManagerOptions> validator_options_;
  Config config_;
  ton::tl_object_ptr<ton::ton_api::engine_validator_customOverlaysConfig> custom_overlays_config_;

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
  td::uint32 celldb_compress_depth_ = 0;
  size_t max_open_archive_files_ = 0;
  double archive_preload_period_ = 0.0;
  bool disable_rocksdb_stats_ = false;
  bool nonfinal_ls_queries_enabled_ = false;
  td::optional<td::uint64> celldb_cache_size_ = 1LL << 30;
  bool celldb_direct_io_ = false;
  bool celldb_preload_all_ = false;
  bool celldb_in_memory_ = false;
  bool celldb_v2_ = false;
  bool celldb_disable_bloom_filter_ = false;
  td::optional<double> catchain_max_block_delay_, catchain_max_block_delay_slow_;
  bool read_config_ = false;
  bool started_keyring_ = false;
  bool started_ = false;
  ton::BlockSeqno truncate_seqno_{0};
  std::string session_logs_file_;
  bool fast_state_serializer_enabled_ = false;
  std::string validator_telemetry_filename_;
  bool not_all_shards_ = false;
  std::vector<ton::ShardIdFull> add_shard_cmds_;
  bool state_serializer_disabled_flag_ = false;
  double broadcast_speed_multiplier_catchain_ = 3.33;
  double broadcast_speed_multiplier_public_ = 3.33;
  double broadcast_speed_multiplier_private_ = 3.33;

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
  void set_session_logs_file(std::string f) {
    session_logs_file_ = std::move(f);
  }
  void add_ip(td::IPAddress addr) {
    addrs_.push_back(addr);
  }
  void add_key_to_set(ton::PublicKey key) {
    keys_[key.compute_short_id()] = key;
  }
  void schedule_shutdown(double at);
  void set_celldb_compress_depth(td::uint32 value) {
    celldb_compress_depth_ = value;
  }
  void set_max_open_archive_files(size_t value) {
    max_open_archive_files_ = value;
  }
  void set_archive_preload_period(double value) {
    archive_preload_period_ = value;
  }
  void set_disable_rocksdb_stats(bool value) {
    disable_rocksdb_stats_ = value;
  }
  void set_nonfinal_ls_queries_enabled() {
    nonfinal_ls_queries_enabled_ = true;
  }
  void set_celldb_cache_size(td::uint64 value) {
    celldb_cache_size_ = value;
  }
  void set_celldb_direct_io(bool value) {
    celldb_direct_io_ = value;
  }
  void set_celldb_preload_all(bool value) {
    celldb_preload_all_ = value;
  }
  void set_celldb_in_memory(bool value) {
    celldb_in_memory_ = value;
  }
  void set_celldb_v2(bool value) {
    celldb_v2_ = value;
  }
  void set_celldb_disable_bloom_filter(bool value) {
    celldb_disable_bloom_filter_ = value;
  }
  void set_catchain_max_block_delay(double value) {
    catchain_max_block_delay_ = value;
  }
  void set_catchain_max_block_delay_slow(double value) {
    catchain_max_block_delay_slow_ = value;
  }
  void set_fast_state_serializer_enabled(bool value) {
    fast_state_serializer_enabled_ = value;
  }
  void set_validator_telemetry_filename(std::string value) {
    validator_telemetry_filename_ = std::move(value);
  }
  void set_not_all_shards() {
    not_all_shards_ = true;
  }
  void add_shard_cmd(ton::ShardIdFull shard) {
    add_shard_cmds_.push_back(shard);
  }
  void set_state_serializer_disabled_flag() {
    state_serializer_disabled_flag_ = true;
  }
  void set_broadcast_speed_multiplier_catchain(double value) {
    broadcast_speed_multiplier_catchain_ = value;
  }
  void set_broadcast_speed_multiplier_public(double value) {
    broadcast_speed_multiplier_public_ = value;
  }
  void set_broadcast_speed_multiplier_private(double value) {
    broadcast_speed_multiplier_private_ = value;
  }

  void start_up() override;
  ValidatorEngine() {
  }

  // load config
  td::Status load_global_config();
  void load_empty_local_config(td::Promise<td::Unit> promise);
  void load_local_config(td::Promise<td::Unit> promise);
  void load_config(td::Promise<td::Unit> promise);
  void set_shard_check_function();

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

  std::string custom_overlays_config_file() const {
    return db_root_ + "/custom-overlays.json";
  }
  std::string collator_options_file() const {
    return db_root_ + "/collator-options.json";
  }

  void load_custom_overlays_config();
  td::Status write_custom_overlays_config();
  void add_custom_overlay_to_config(
      ton::tl_object_ptr<ton::ton_api::engine_validator_customOverlay> overlay, td::Promise<td::Unit> promise);
  void del_custom_overlay_from_config(std::string name, td::Promise<td::Unit> promise);
  void load_collator_options();

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
  void run_control_query(ton::ton_api::engine_validator_exportAllPrivateKeys &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
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
  void run_control_query(ton::ton_api::engine_validator_getActorTextStats &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addShard &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delShard &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getPerfTimerStats &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getShardOutQueueSize &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_setExtMessagesBroadcastDisabled &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_addCustomOverlay &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_delCustomOverlay &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_showCustomOverlays &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_setStateSerializerEnabled &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_setCollatorOptionsJson &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getCollatorOptionsJson &query, td::BufferSlice data,
                         ton::PublicKeyHash src, td::uint32 perm, td::Promise<td::BufferSlice> promise);
  void run_control_query(ton::ton_api::engine_validator_getAdnlStats &query, td::BufferSlice data,
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
