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
*/
#include <iostream>
#include <ton/ton-tl.hpp>

#include "adnl/adnl.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api_json.h"
#include "block/block-auto.h"
#include "block/mc-config.h"
#include "lite-client/ext-client.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/actor.h"
#include "td/utils/FileLog.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/filesystem.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/tl_storers.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"

#include "git.h"

using namespace ton;

std::string global_config_file;
td::optional<BlockSeqno> start_mc_seqno, end_mc_seqno;
std::vector<ShardIdFull> shards;

class PrepareLsSliceConfig : public td::actor::Actor {
 public:
  void start_up() override {
    if (start_mc_seqno && end_mc_seqno && start_mc_seqno.value() > end_mc_seqno.value()) {
      LOG(FATAL) << "from-seqno is greater than to-seqno";
    }

    if (!start_mc_seqno && !end_mc_seqno) {
      auto slice = create_tl_object<ton_api::liteserver_descV2_sliceSimple>();
      if (shards.empty()) {
        slice->shards_.push_back(create_tl_shard_id(ShardIdFull{basechainId, shardIdAll}));
      } else {
        for (const ShardIdFull& shard : shards) {
          if (!shard.is_masterchain()) {
            slice->shards_.push_back(create_tl_shard_id(shard));
          }
        }
      }
      print_result(*slice);
      return;
    }

    auto gc_s = td::read_file(global_config_file).move_as_ok();
    auto gc_j = td::json_decode(gc_s.as_slice()).move_as_ok();
    ton_api::liteclient_config_global gc;
    ton_api::from_json(gc, gc_j.get_object()).ensure();
    auto r_servers = liteclient::LiteServerConfig::parse_global_config(gc);
    r_servers.ensure();
    client_ = liteclient::ExtClient::create(r_servers.move_as_ok(), nullptr);

    slice_timed_ = create_tl_object<ton_api::liteserver_descV2_sliceTimed>();
    ++pending_;
    request_shards_info(start_mc_seqno, true);
    request_shards_info(end_mc_seqno, false);
    dec_pending();
  }

  template <class Type, class... Args>
  static td::BufferSlice create_query(Args&&... args) {
    Type object(std::forward<Args>(args)...);
    return create_serialize_tl_object<lite_api::liteServer_query>(serialize_tl_object(&object, true));
  }

  template <class Type>
  static tl_object_ptr<Type> parse_response(const td::Result<td::BufferSlice>& R) {
    R.ensure();
    auto err = fetch_tl_object<lite_api::liteServer_error>(R.ok(), true);
    if (err.is_ok()) {
      LOG(FATAL) << "liteserver error: " << err.ok()->message_;
    }
    auto res = fetch_tl_object<Type>(R.ok(), true);
    res.ensure();
    return res.move_as_ok();
  }

  void request_shards_info(td::optional<BlockSeqno> seqno, bool is_start) {
    if (!seqno) {
      return;
    }
    ++pending_;
    td::actor::send_closure(
        client_, &liteclient::ExtClient::send_query, "q",
        create_query<lite_api::liteServer_lookupBlock>(
            1, create_tl_object<lite_api::tonNode_blockId>(masterchainId, shardIdAll, seqno.value()), 0, 0),
        td::Timestamp::in(5.0), [=, client = client_.get(), SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
          auto mc_header = parse_response<lite_api::liteServer_blockHeader>(std::move(R));
          auto block_id = create_block_id(mc_header->id_);
          td::actor::send_closure(
              client, &liteclient::ExtClient::send_query, "q",
              create_query<lite_api::liteServer_getAllShardsInfo>(create_tl_lite_block_id(block_id)),
              td::Timestamp::in(5.0), [=, mc_header = std::move(mc_header)](td::Result<td::BufferSlice> R) mutable {
                auto shards_info = parse_response<lite_api::liteServer_allShardsInfo>(std::move(R));
                td::actor::send_closure(SelfId, &PrepareLsSliceConfig::got_shards_info, std::move(mc_header),
                                        std::move(shards_info), is_start);
              });
        });
  }

  static tl_object_ptr<ton_api::liteserver_descV2_shardInfo> parse_header(const lite_api::liteServer_blockHeader& obj,
                                                                          bool is_start) {
    auto res = create_tl_object<ton_api::liteserver_descV2_shardInfo>();

    BlockIdExt block_id = create_block_id(obj.id_);
    res->shard_id_ = create_tl_shard_id(block_id.shard_full());
    res->seqno_ = block_id.seqno();

    auto root = vm::std_boc_deserialize(obj.header_proof_).move_as_ok();
    root = vm::MerkleProof::virtualize(root);
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    CHECK(tlb::unpack_cell(root, blk) && tlb::unpack_cell(blk.info, info));
    res->utime_ = info.gen_utime;
    res->lt_ = (is_start ? info.start_lt : info.end_lt);

    return res;
  }

  void got_shards_info(tl_object_ptr<lite_api::liteServer_blockHeader> mc_header,
                       tl_object_ptr<lite_api::liteServer_allShardsInfo> shards_info, bool is_start) {
    (is_start ? slice_timed_->shards_from_ : slice_timed_->shards_to_).push_back(parse_header(*mc_header, is_start));

    auto root = vm::std_boc_deserialize(shards_info->data_).move_as_ok();
    block::ShardConfig sh_conf;
    CHECK(sh_conf.unpack(vm::load_cell_slice_ref(root)));
    auto ids = sh_conf.get_shard_hash_ids(true);
    for (auto id : ids) {
      BlockIdExt block_id = sh_conf.get_shard_hash(ton::ShardIdFull(id))->top_block_id();
      bool ok = shards.empty();
      for (const auto& our_shard : shards) {
        if (shard_intersects(our_shard, block_id.shard_full())) {
          ok = true;
          break;
        }
      }
      if (ok) {
        ++pending_;
        td::actor::send_closure(
            client_, &liteclient::ExtClient::send_query, "q",
            create_query<lite_api::liteServer_getBlockHeader>(create_tl_lite_block_id(block_id), 0xffff),
            td::Timestamp::in(5.0), [=, SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable {
              auto header = parse_response<lite_api::liteServer_blockHeader>(std::move(R));
              td::actor::send_closure(SelfId, &PrepareLsSliceConfig::got_block_header, std::move(header), is_start);
            });
      }
    }

    dec_pending();
  }

  void got_block_header(tl_object_ptr<lite_api::liteServer_blockHeader> header, bool is_start) {
    (is_start ? slice_timed_->shards_from_ : slice_timed_->shards_to_).push_back(parse_header(*header, is_start));
    dec_pending();
  }

  void print_result(const ton_api::liteserver_descV2_Slice& result) {
    auto s = td::json_encode<std::string>(td::ToJson(result), true);
    std::cout << s << "\n";
    std::cout.flush();
    exit(0);
  }

 private:
  td::actor::ActorOwn<liteclient::ExtClient> client_;
  tl_object_ptr<ton_api::liteserver_descV2_sliceTimed> slice_timed_;
  size_t pending_ = 0;

  void dec_pending() {
    --pending_;
    if (pending_ == 0) {
      auto cmp = [](const tl_object_ptr<ton_api::liteserver_descV2_shardInfo>& a,
                    const tl_object_ptr<ton_api::liteserver_descV2_shardInfo>& b) {
        return create_shard_id(a->shard_id_) < create_shard_id(b->shard_id_);
      };
      std::sort(slice_timed_->shards_from_.begin(), slice_timed_->shards_from_.end(), cmp);
      std::sort(slice_timed_->shards_to_.begin(), slice_timed_->shards_to_.end(), cmp);
      print_result(*slice_timed_);
    }
  }
};

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::OptionParser p;
  p.set_description(
      "Generate liteserver.descV2.Slice for global-config.json from given shards and masterchain seqnos\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('V', "version", "show build information", [&]() {
    std::cout << "prepare-ls-slice-config build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('C', "global-config", "global TON configuration file (used to fetch shard configuration)",
               [&](td::Slice arg) { global_config_file = arg.str(); });
  p.add_checked_option('f', "from-seqno", "starting masterchain seqno (default: none)",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT_ASSIGN(start_mc_seqno, td::to_integer_safe<BlockSeqno>(arg))
                         return td::Status::OK();
                       });
  p.add_checked_option('t', "to-seqno", "ending masterchain seqno (default: none)", [&](td::Slice arg) -> td::Status {
    TRY_RESULT_ASSIGN(end_mc_seqno, td::to_integer_safe<BlockSeqno>(arg))
    return td::Status::OK();
  });
  p.add_checked_option('s', "shard", "shard in format 0:8000000000000000 (default: all shards)",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT(shard, ShardIdFull::parse(arg));
                         if (!shard.is_valid_ext()) {
                           return td::Status::Error(PSTRING() << "invalid shard " << arg);
                         }
                         shards.push_back(shard);
                         return td::Status::OK();
                       });

  p.run(argc, argv).ensure();
  td::actor::Scheduler scheduler({3});

  scheduler.run_in_context([&] { td::actor::create_actor<PrepareLsSliceConfig>("main").release(); });
  while (scheduler.run(1)) {
  }
}
