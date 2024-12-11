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
#include "adnl/adnl.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "dht/dht.h"
#include "overlay/overlays.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/format.h"
#include "td/utils/Random.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "catchain/catchain.h"
#include "validator-session/validator-session.h"
#include "validator/manager-hardfork.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"

#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"

#include "validator/fabric.h"
#include "validator/impl/collator.h"
#include "crypto/vm/vm.h"
#include "crypto/block/block-db.h"

#include "common/errorlog.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>
#include <sstream>
#include "git.h"

int verbosity;

struct IntError {
  std::string err_msg;
  IntError(std::string _msg) : err_msg(_msg) {
  }
  IntError(const char *_msg) : err_msg(_msg) {
  }
  IntError(td::Status _err) : err_msg(_err.to_string()) {
  }
  void show() const {
    std::cerr << "fatal: " << err_msg << std::endl;
  }
};

class HardforkCreator : public td::actor::Actor {
 private:
  td::actor::ActorOwn<ton::validator::ValidatorManagerInterface> validator_manager_;

  std::string db_root_ = "/var/ton-work/db/";
  std::string global_config_;
  td::Ref<ton::validator::ValidatorManagerOptions> opts_;
  td::BufferSlice bs_;
  std::vector<td::BufferSlice> ext_msgs_;
  std::vector<td::BufferSlice> top_shard_descrs_;
  bool need_save_file_{false};
  bool tdescr_save_{false};
  std::string tdescr_pfx_;
  ton::BlockIdExt shard_top_block_id_;

  ton::ShardIdFull shard_{ton::masterchainId};

 public:
  void set_db_root(std::string db_root) {
    db_root_ = db_root;
  }
  void set_global_config_path(std::string path) {
    global_config_ = path;
  }
  void set_shard(ton::ShardIdFull shard) {
    LOG(DEBUG) << "setting shard to " << shard.to_str();
    shard_ = shard;
  }
  void set_shard_top_block(ton::BlockIdExt block_id) {
    shard_top_block_id_ = block_id;
  }
  void set_top_descr_prefix(std::string tdescr_pfx) {
    tdescr_pfx_ = tdescr_pfx;
    tdescr_save_ = true;
  }
  void set_collator_flags(int flags) {
    ton::collator_settings |= flags;
  }
  void start_up() override {
  }
  void alarm() override {
  }
  HardforkCreator() {
  }

  void load_ext_message(std::string filename) {
    try {
      auto res1 = block::load_binary_file(filename);
      if (res1.is_error()) {
        throw IntError{res1.move_as_error()};
      }
      ext_msgs_.emplace_back(res1.move_as_ok());
    } catch (IntError err) {
      err.show();
      std::exit(7);
    }
  }

  void load_shard_block_message(std::string filename) {
    try {
      auto res1 = block::load_binary_file(filename);
      if (res1.is_error()) {
        throw IntError{res1.move_as_error()};
      }
      top_shard_descrs_.emplace_back(res1.move_as_ok());
    } catch (IntError err) {
      err.show();
      std::exit(7);
    }
  }

  void do_save_file() {
  }

  td::Status create_validator_options() {
    if(!global_config_.length()) {
      opts_ = ton::validator::ValidatorManagerOptions::create(
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()},
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()});
     return td::Status::OK();
    }
    TRY_RESULT_PREFIX(conf_data, td::read_file(global_config_), "failed to read: ");
    TRY_RESULT_PREFIX(conf_json, td::json_decode(conf_data.as_slice()), "failed to parse json: ");

    ton::ton_api::config_global conf;
    TRY_STATUS_PREFIX(ton::ton_api::from_json(conf, conf_json.get_object()), "json does not fit TL scheme: ");

    auto zero_state = ton::create_block_id(conf.validator_->zero_state_);
    ton::BlockIdExt init_block;
    if (!conf.validator_->init_block_) {
      LOG(INFO) << "no init block in config. using zero state";
      init_block = zero_state;
    } else {
      init_block = ton::create_block_id(conf.validator_->init_block_);
    }
    opts_ = ton::validator::ValidatorManagerOptions::create(zero_state, init_block);
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
    opts_.write().set_hardforks(std::move(h));
    return td::Status::OK();
  }

  void run() {
    td::mkdir(db_root_).ensure();
    ton::errorlog::ErrorLog::create(db_root_);
    if (!shard_.is_masterchain() && need_save_file_) {
      td::mkdir(db_root_ + "/static").ensure();
      do_save_file();
    }

    auto Sr = create_validator_options();
    if (Sr.is_error()) {
      LOG(ERROR) << "failed to load global config'" << global_config_ << "': " << Sr;
      std::_Exit(2);
    }

    auto opts = opts_;
    opts.write().set_initial_sync_disabled(true);
    validator_manager_ =
        ton::validator::ValidatorManagerHardforkFactory::create(opts, shard_, shard_top_block_id_, db_root_);
    for (auto &msg : ext_msgs_) {
      td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManager::new_external_message,
                              std::move(msg), 0);
    }
    for (auto &topmsg : top_shard_descrs_) {
      td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManager::new_shard_block, ton::BlockIdExt{},
                              0, std::move(topmsg));
    }
    class Callback : public ton::validator::ValidatorManagerInterface::Callback {
     private:
      td::actor::ActorId<ton::validator::ValidatorManagerInterface> id_;
      bool tdescr_save_;
      std::string tdescr_pfx_;
      int tdescr_cnt_ = 0;

     public:
      Callback(td::actor::ActorId<ton::validator::ValidatorManagerInterface> id, bool tdescr_save = false,
               std::string tdescr_pfx = "")
          : id_(id), tdescr_save_(tdescr_save), tdescr_pfx_(tdescr_pfx) {
      }

      void initial_read_complete(ton::validator::BlockHandle handle) override {
        td::actor::send_closure(id_, &ton::validator::ValidatorManager::sync_complete,
                                td::PromiseCreator::lambda([](td::Unit) {}));
      }
      void on_new_masterchain_block(td::Ref<ton::validator::MasterchainState> state,
                                    std::set<ton::ShardIdFull> shards_to_monitor) override {
      }
      void send_ihr_message(ton::AccountIdPrefixFull dst, td::BufferSlice data) override {
      }
      void send_ext_message(ton::AccountIdPrefixFull dst, td::BufferSlice data) override {
      }
      void send_shard_block_info(ton::BlockIdExt block_id, ton::CatchainSeqno cc_seqno, td::BufferSlice data) override {
      }
      void send_block_candidate(ton::BlockIdExt block_id, ton::CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                td::BufferSlice data) override {
      }
      void send_broadcast(ton::BlockBroadcast broadcast, int mode) override {
      }
      void download_block(ton::BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                          td::Promise<ton::ReceivedBlock> promise) override {
      }
      void download_zero_state(ton::BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                               td::Promise<td::BufferSlice> promise) override {
      }
      void download_persistent_state(ton::BlockIdExt block_id, ton::BlockIdExt masterchain_block_id,
                                     td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::BufferSlice> promise) override {
      }
      void download_block_proof(ton::BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                td::Promise<td::BufferSlice> promise) override {
      }
      void download_block_proof_link(ton::BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                     td::Promise<td::BufferSlice> promise) override {
      }
      void get_next_key_blocks(ton::BlockIdExt block_id, td::Timestamp timeout,
                               td::Promise<std::vector<ton::BlockIdExt>> promise) override {
      }
      void download_archive(ton::BlockSeqno masterchain_seqno, ton::ShardIdFull shard_prefix, std::string tmp_dir,
                            td::Timestamp timeout, td::Promise<std::string> promise) override {
      }
      void download_out_msg_queue_proof(
          ton::ShardIdFull dst_shard, std::vector<ton::BlockIdExt> blocks, block::ImportedMsgQueueLimits limits,
          td::Timestamp timeout, td::Promise<std::vector<td::Ref<ton::validator::OutMsgQueueProof>>> promise) override {
      }

      void new_key_block(ton::validator::BlockHandle handle) override {
      }
      void send_validator_telemetry(ton::PublicKeyHash key,
                                    ton::tl_object_ptr<ton::ton_api::validator_telemetry> telemetry) override {
      }
    };

    td::actor::send_closure(validator_manager_, &ton::validator::ValidatorManagerInterface::install_callback,
                            std::make_unique<Callback>(validator_manager_.get(), tdescr_save_, tdescr_pfx_),
                            td::PromiseCreator::lambda([](td::Unit) {}));
  }
};

td::Result<td::Bits256> get_uint256(td::Slice str) {
  TRY_RESULT(R, td::base64url_decode(str));
  if (R.length() != 32) {
    return td::Status::Error("uint256 must have 64 bytes");
  }
  td::Bits256 x;
  as_slice(x).copy_from(td::Slice(R));
  return x;
}

int parse_hex_digit(int c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c |= 0x20;
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

int main(int argc, char *argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  vm::init_vm().ensure();

  td::actor::ActorOwn<HardforkCreator> x;

  td::OptionParser p;
  p.set_description("test collate block");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('V', "version", "shows create-hardfork build information", [&]() {
    std::cout << "create-hardfork build information: [ Commit: " << GitMetadata::CommitSHA1() << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('D', "db", "root for dbs",
               [&](td::Slice fname) { td::actor::send_closure(x, &HardforkCreator::set_db_root, fname.str()); });
  p.add_option('C', "config", "global config path",
               [&](td::Slice fname) { td::actor::send_closure(x, &HardforkCreator::set_global_config_path, fname.str()); });
  p.add_option('m', "ext-message", "binary file with serialized inbound external message",
               [&](td::Slice fname) { td::actor::send_closure(x, &HardforkCreator::load_ext_message, fname.str()); });
  p.add_option(
      'M', "top-shard-message", "binary file with serialized shard top block description",
      [&](td::Slice fname) { td::actor::send_closure(x, &HardforkCreator::load_shard_block_message, fname.str()); });
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (verbosity = td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_checked_option('w', "workchain", "<workchain>[:<shard>]\tcollate block in this workchain", [&](td::Slice arg) {
    ton::ShardId shard = 0;
    auto pos = std::min(arg.find(':'), arg.size());
    TRY_RESULT(workchain, td::to_integer_safe<int>(arg.substr(0, pos)));
    int s = 60;
    while (++pos < arg.size()) {
      int x = parse_hex_digit(arg[pos]);
      if (x < 0 || s < 0) {
        return td::Status::Error("cannot parse hexadecimal shard id (prefix)");
      }
      shard |= (ton::ShardId(x) << s);
      s -= 4;
    }
    td::actor::send_closure(x, &HardforkCreator::set_shard,
                            ton::ShardIdFull{workchain, shard ? shard : ton::shardIdAll});
    return td::Status::OK();
  });
  p.add_checked_option('T', "top-block", "BlockIdExt of top block (new block will be generated atop of it)",
                       [&](td::Slice arg) {
                         ton::BlockIdExt block_id;
                         if (block::parse_block_id_ext(arg, block_id)) {
                           LOG(INFO) << "setting previous block to " << block_id.to_str();
                           td::actor::send_closure(x, &HardforkCreator::set_shard_top_block, block_id);

                           return td::Status::OK();
                         } else {
                           return td::Status::Error("cannot parse BlockIdExt");
                         }
                       });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
    td::set_signal_handler(td::SignalType::HangUp, [](int sig) {
#if TD_DARWIN || TD_LINUX
      close(0);
      setsid();
#endif
    }).ensure();
  });

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] { x = td::actor::create_actor<HardforkCreator>("testnode"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &HardforkCreator::run); });
  scheduler.run();

  return 0;
}
