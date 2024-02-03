#include <cstdlib>
#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "auto/tl/ton_api_json.h"
#include "crypto/vm/cp0.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"
#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "tl/tlblib.hpp"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/dict.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "td/utils/Slice.h"
#include "td/utils/common.h"
#include "td/utils/OptionParser.h"
#include "td/utils/port/user.h"
#include <utility>
#include <fstream>
#include "auto/tl/lite_api.h"
#include "adnl/utils.hpp"
#include "tuple"
#include "crypto/block/mc-config.h"
#include "lite-server-config.hpp"
#include <algorithm>
#include <queue>
#include <chrono>
#include <thread>

namespace ton {
namespace liteserver {
class LiteServerDaemon : public td::actor::Actor {
 public:
  LiteServerDaemon(std::string db_root, std::string server_config_path, std::string ipaddr, std::string config_path) {
    db_root_ = std::move(db_root);
    server_config_ = std::move(server_config_path);
    tmp_ipaddr_ = std::move(ipaddr);  // only for first run (generate config)
    global_config_ = std::move(config_path);
  }

  void start_up() override {
    LOG(WARNING) << "Start lite-server daemon";
    keyring_ = ton::keyring::Keyring::create(db_root_ + "/keyring");

    auto Sr = create_validator_options();
    if (Sr.is_error()) {
      LOG(ERROR) << "failed to load global config'" << global_config_ << "': " << Sr;
      std::_Exit(2);
    } else {
      LOG(DEBUG) << "Global config loaded successfully from " << global_config_;
    }

    load_config();
  }

  void sync_complete(const ton::validator::BlockHandle &handle) {
    LOG(WARNING) << "Sync complete: " << handle->id().to_str();

    // Start LightServers
    for (auto &s : config_.liteservers) {
      td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_id, ton::adnl::AdnlNodeIdFull{keys_[s.second]},
                              ton::adnl::AdnlAddressList{}, static_cast<td::uint8>(255));
      td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_ext_server_id,
                              ton::adnl::AdnlNodeIdShort{s.second});
      td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::add_ext_server_port,
                              static_cast<td::uint16>(s.first));
    }
  }

 private:
  std::string db_root_;
  std::string server_config_;
  std::string tmp_ipaddr_;
  std::string global_config_;
  ton::liteserver::Config config_;

  ton::adnl::AdnlNodesList adnl_static_nodes_;
  std::map<ton::PublicKeyHash, td::actor::ActorOwn<ton::dht::Dht>> dht_nodes_;
  std::shared_ptr<ton::dht::DhtGlobalConfig> dht_config_;
  std::map<ton::PublicKeyHash, ton::PublicKey> keys_;
  adnl::AdnlNodeIdShort local_id_;
  td::actor::ActorOwn<keyring::Keyring> keyring_;
  td::actor::ActorOwn<adnl::AdnlNetworkManager> adnl_network_manager_;
  td::actor::ActorOwn<adnl::Adnl> adnl_;
  td::Ref<ton::validator::ValidatorManagerOptions> opts_;
  td::actor::ActorOwn<ton::validator::ValidatorManagerInterface> validator_manager_;
  td::actor::ActorOwn<ton::overlay::Overlays> overlay_manager_;
  td::actor::ActorOwn<ton::rldp::Rldp> rldp_;
  td::actor::ActorOwn<ton::rldp2::Rldp> rldp2_;
  ton::PublicKeyHash default_dht_node_ = ton::PublicKeyHash::zero();

  int to_load_keys = 0;

  void init_validator_engine() {
    auto shard = ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
    auto shard_top =
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()};

    auto id = ton::PublicKeyHash::zero();
    validator_manager_ = ton::validator::ValidatorManagerDiskFactory::create(
        id, opts_, shard, shard_top, db_root_, keyring_.get(), adnl_.get(), rldp_.get(), overlay_manager_.get(), true);

    class Callback : public ton::validator::ValidatorManagerInterface::Callback {
     public:
      void initial_read_complete(ton::validator::BlockHandle handle) override {
        LOG(DEBUG) << "Initial read complete: " << handle->id().to_str();
        td::actor::send_closure(id_, &LiteServerDaemon::sync_complete, handle);
      }
      void add_shard(ShardIdFull shard) override {
      }
      void del_shard(ShardIdFull shard) override {
      }
      void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      }
      void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      }
      void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
      }
      void send_broadcast(BlockBroadcast broadcast) override {
      }
      void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<ReceivedBlock> promise) override {
      }
      void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                               td::Promise<td::BufferSlice> promise) override {
      }
      void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, td::uint32 priority,
                                     td::Timestamp timeout, td::Promise<td::BufferSlice> promise) override {
      }
      void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::BufferSlice> promise) override {
      }
      void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::BufferSlice> promise) override {
      }
      void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                               td::Promise<std::vector<BlockIdExt>> promise) override {
      }
      void download_archive(BlockSeqno masterchain_seqno, std::string tmp_dir, td::Timestamp timeout,
                            td::Promise<std::string> promise) override {
      }

      void new_key_block(ton::validator::BlockHandle handle) override {
      }

      Callback(td::actor::ActorId<LiteServerDaemon> id) : id_(id) {
      }

     private:
      td::actor::ActorId<LiteServerDaemon> id_;
    };

    auto P_cb = td::PromiseCreator::lambda([](td::Unit R) {});
    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::install_callback,
                            std::make_unique<Callback>(actor_id(this)), std::move(P_cb));
  }

  void init_network() {
    adnl_network_manager_ = adnl::AdnlNetworkManager::create(static_cast<td::uint16>(config_.addr_.get_port()));
    adnl_ = adnl::Adnl::create("", keyring_.get());
    td::actor::send_closure(adnl_, &adnl::Adnl::register_network_manager, adnl_network_manager_.get());
    adnl::AdnlCategoryMask cat_mask;
    cat_mask[0] = true;
    td::actor::send_closure(adnl_network_manager_, &adnl::AdnlNetworkManager::add_self_addr, config_.addr_,
                            std::move(cat_mask), 0);

    // Start ADNL
    adnl::AdnlAddressList addr_list;
    addr_list.add_udp_address(config_.addr_).ensure();
    addr_list.set_version(static_cast<td::int32>(td::Clocks::system()));
    addr_list.set_reinit_date(adnl::Adnl::adnl_start_time());

    for (auto &adnl : config_.adnl_ids) {
      adnl::AdnlNodeIdFull local_id_full = adnl::AdnlNodeIdFull::create(keys_[adnl.first].tl()).move_as_ok();
      local_id_ = local_id_full.compute_short_id();
      td::actor::send_closure(adnl_, &adnl::Adnl::add_id, local_id_full, addr_list, static_cast<td::uint8>(0));
    }
    td::actor::send_closure(adnl_, &ton::adnl::Adnl::add_static_nodes_from_config, std::move(adnl_static_nodes_));

    // Start DHT
    for (auto &dht : config_.dht_ids) {
      auto D =
          ton::dht::Dht::create(ton::adnl::AdnlNodeIdShort{dht}, db_root_, dht_config_, keyring_.get(), adnl_.get());
      D.ensure();

      dht_nodes_[dht] = D.move_as_ok();
      if (default_dht_node_.is_zero()) {
        default_dht_node_ = dht;
      }
    }

    // Start RLDP
    rldp_ = ton::rldp::Rldp::create(adnl_.get());
    rldp2_ = ton::rldp2::Rldp::create(adnl_.get());

    if (default_dht_node_.is_zero()) {
      LOG(ERROR) << "Config broken, no DHT";
      std::_Exit(2);
    }
    // Start Overlay
    overlay_manager_ =
        ton::overlay::Overlays::create(db_root_, keyring_.get(), adnl_.get(), dht_nodes_[default_dht_node_].get(), "liteserver");

    init_validator_engine();
  }

  void got_key(ton::PublicKey key) {
    to_load_keys--;
    keys_[key.compute_short_id()] = std::move(key);

    if (to_load_keys == 0) {
      LOG(WARNING) << "ADNL available on: " << config_.addr_;

      for (auto &t : config_.adnl_ids) {
        LOG(WARNING) << "ADNL pub: " << keys_[t.first].ed25519_value().raw().to_hex();
      }

      for (auto &[t, e] : config_.liteservers) {
        LOG(WARNING) << "LiteServer port: " << t << " pub: " << keys_[e].ed25519_value().raw().to_hex();
      }

      td::actor::send_closure(actor_id(this), &LiteServerDaemon::init_network);
    }
  }

  void load_config() {
    if (server_config_.empty()) {
      // Generate new config
      server_config_ = db_root_ + "/liteserver.json";
      LOG(WARNING) << "Generate config file, write to: " << server_config_ << ", double-check and run again";

      auto config = ton::liteserver::Config();

      td::IPAddress addr;
      auto s = addr.init_host_port(tmp_ipaddr_);
      if (s.is_error()) {
        LOG(ERROR) << s.move_as_error();
        std::_Exit(2);
      }

      auto ls_port = addr.get_port() + 1;
      config.set_addr(addr);

      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      auto id = pk.compute_short_id();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(pk), false, [](td::Unit) {});
      config.config_add_adnl_addr(id, 0).ensure();
      config.config_add_dht_node(id).ensure();

      auto ls_pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      auto ls_id = ls_pk.compute_short_id();
      td::actor::send_closure(keyring_, &keyring::Keyring::add_key, std::move(ls_pk), false, [](td::Unit) {});
      config.config_add_lite_server(ls_id, ls_port).ensure();

      auto ss = td::json_encode<std::string>(td::ToJson(*config.tl().get()), true);

      auto S = td::write_file(server_config_, ss);
      if (S.is_ok()) {
        stop();  // todo: wait keyring_ save keys
        return;
      } else {
        LOG(ERROR) << S.move_as_error();
      }
    } else {
      // Use existing config
      auto conf_data_R = td::read_file(server_config_);
      if (conf_data_R.is_error()) {
        LOG(ERROR) << "Can't read file: " << server_config_;
        std::_Exit(2);
      }
      auto conf_data = conf_data_R.move_as_ok();
      auto conf_json_R = td::json_decode(conf_data.as_slice());
      if (conf_json_R.is_error()) {
        LOG(ERROR) << "Failed to parse json";
        std::_Exit(2);
      }
      auto conf_json = conf_json_R.move_as_ok();

      ton::ton_api::engine_liteserver_config conf;
      auto S = ton::ton_api::from_json(conf, conf_json.get_object());
      if (S.is_error()) {
        LOG(ERROR) << "Json does not fit TL scheme";
        std::_Exit(2);
      }

      config_ = ton::liteserver::Config{conf};
      for (auto &key : config_.keys_refcnt) {
        to_load_keys++;

        auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ton::PublicKey> R) mutable {
          if (R.is_error()) {
            LOG(ERROR) << R.move_as_error();
            std::_Exit(2);
          } else {
            td::actor::send_closure(SelfId, &LiteServerDaemon::got_key, R.move_as_ok());
          }
        });

        td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key_short, key.first, std::move(P));
      }
    }
  }

  td::Status create_validator_options() {
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

    if (conf.adnl_) {
      if (conf.adnl_->static_nodes_) {
        TRY_RESULT_PREFIX_ASSIGN(adnl_static_nodes_, ton::adnl::AdnlNodesList::create(conf.adnl_->static_nodes_),
                                 "bad static adnl nodes: ");
      }
    }
    TRY_RESULT_PREFIX(dht, ton::dht::Dht::create_global_config(std::move(conf.dht_)), "bad [dht] section: ");
    dht_config_ = std::move(dht);

    auto zero_state = ton::create_block_id(conf.validator_->zero_state_);
    ton::BlockIdExt init_block;
    if (!conf.validator_->init_block_) {
      LOG(INFO) << "no init block read. using zero state";
      init_block = zero_state;
    } else {
      init_block = ton::create_block_id(conf.validator_->init_block_);
    }

    std::function<bool(ton::ShardIdFull, ton::CatchainSeqno, ton::validator::ValidatorManagerOptions::ShardCheckMode)>
        check_shard = [](ton::ShardIdFull, ton::CatchainSeqno,
                         ton::validator::ValidatorManagerOptions::ShardCheckMode) { return true; };
    bool allow_blockchain_init = false;
    double sync_blocks_before = 86400;
    double block_ttl = 86400 * 7;
    double state_ttl = 3600;
    double archive_ttl = 86400 * 365;
    double key_proof_ttl = 86400 * 3650;
    double max_mempool_num = 999999;
    bool initial_sync_disabled = true;

    opts_ = ton::validator::ValidatorManagerOptions::create(zero_state, init_block, check_shard, allow_blockchain_init,
                                                            sync_blocks_before, block_ttl, state_ttl, archive_ttl,
                                                            key_proof_ttl, max_mempool_num, initial_sync_disabled);

    std::vector<ton::BlockIdExt> h;
    h.reserve(conf.validator_->hardforks_.size());
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
      h.emplace_back(b);
    }
    opts_.write().set_hardforks(std::move(h));
    return td::Status::OK();
  }
};
}  // namespace liteserver
}  // namespace ton

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  td::OptionParser p;
  std::string db_root;
  std::string config_path;
  std::string server_config_path;
  std::string ipaddr;
  td::uint32 threads = 7;
  int verbosity = 0;

  p.set_description("blockchain indexer");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option(
      't', "threads", PSTRING() << "number of threads (default=" << threads << ")", [&](td::Slice arg) {
        td::uint32 v;
        try {
          v = std::stoi(arg.str());
        } catch (...) {
          return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: not a number");
        }
        if (v < 1 || v > 256) {
          return td::Status::Error(ton::ErrorCode::error, "bad value for --threads: should be in range [1..256]");
        }
        threads = v;

        return td::Status::OK();
      });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 9) ? td::Status::OK() : td::Status::Error("verbosity must be 0..9");
  });

  p.add_option('D', "db", "root for dbs", [&](td::Slice fname) { db_root = fname.str(); });
  p.add_option('C', "config", "global config path", [&](td::Slice fname) { config_path = fname.str(); });
  p.add_option('S', "server-config", "server config path", [&](td::Slice fname) { server_config_path = fname.str(); });
  p.add_option('I', "ip", "ip address", [&](td::Slice ipaddr_) { ipaddr = ipaddr_.str(); });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }

  // Start vm
  vm::init_vm().ensure();

  if (db_root.empty()) {
    std::cerr << "You must pass db_root" << std::endl;
    std::_Exit(2);
  }

  if (server_config_path.empty()) {
    if (ipaddr.empty()) {
      std::cerr << "You must ipaddr for generating default config" << std::endl;
      std::_Exit(2);
    }
  } else {
    if (!ipaddr.empty()) {
      std::cerr << "Ipaddr flag ignore due config" << std::endl;
      std::_Exit(2);
    }
  }

  td::actor::set_debug(true);
  td::actor::Scheduler scheduler({threads});
  scheduler.run_in_context([&] {
    td::actor::create_actor<ton::liteserver::LiteServerDaemon>("LiteServerDaemon", std::move(db_root),
                                                               std::move(server_config_path), std::move(ipaddr),
                                                               std::move(config_path))
        .release();

    return td::Status::OK();
  });

  scheduler.run();
  return 0;
}