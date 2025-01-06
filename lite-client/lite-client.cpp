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
#include "lite-client.h"

#include "lite-client-common.h"

#include "tl-utils/lite-utils.hpp"
#include "auto/tl/ton_api_json.h"
#include "auto/tl/lite_api.hpp"
#include "td/utils/OptionParser.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/Random.h"
#include "td/utils/crypto.h"
#include "td/utils/port/signals.h"
#include "td/utils/port/FileFd.h"
#include "ton/lite-tl.hpp"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/mc-config.h"
#include "block/check-proof.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/memo.h"
#include "crypto/vm/utils.h"
#include "crypto/common/util.h"
#include "common/checksum.h"

#if TD_DARWIN || TD_LINUX
#include <unistd.h>
#endif
#include <iostream>
#include "git.h"

using namespace std::literals::string_literals;
using td::Ref;

int verbosity;

void TestNode::run() {
  class Cb : public td::TerminalIO::Callback {
   public:
    void line_cb(td::BufferSlice line) override {
      td::actor::send_closure(id_, &TestNode::parse_line, std::move(line));
    }
    Cb(td::actor::ActorId<TestNode> id) : id_(id) {
    }

   private:
    td::actor::ActorId<TestNode> id_;
  };
  io_ = td::TerminalIO::create("> ", readline_enabled_, ex_mode_, std::make_unique<Cb>(actor_id(this)));
  td::actor::send_closure(io_, &td::TerminalIO::set_log_interface);

  std::vector<liteclient::LiteServerConfig> servers;
  if (!single_remote_public_key_.empty()) {  // Use single provided liteserver
    servers.push_back(
        liteclient::LiteServerConfig{ton::adnl::AdnlNodeIdFull{single_remote_public_key_}, single_remote_addr_});
    td::TerminalIO::out() << "using liteserver " << single_remote_addr_ << "\n";
  } else {
    auto G = td::read_file(global_config_).move_as_ok();
    auto gc_j = td::json_decode(G.as_slice()).move_as_ok();
    ton::ton_api::liteclient_config_global gc;
    ton::ton_api::from_json(gc, gc_j.get_object()).ensure();
    auto r_servers = liteclient::LiteServerConfig::parse_global_config(gc);
    r_servers.ensure();
    servers = r_servers.move_as_ok();

    if (gc.validator_ && gc.validator_->zero_state_) {
      zstate_id_.workchain = gc.validator_->zero_state_->workchain_;
      if (zstate_id_.workchain != ton::workchainInvalid) {
        zstate_id_.root_hash = gc.validator_->zero_state_->root_hash_;
        zstate_id_.file_hash = gc.validator_->zero_state_->file_hash_;
        td::TerminalIO::out() << "zerostate set to " << zstate_id_.to_str() << "\n";
      }
    }

    if (single_liteserver_idx_ != -1) {  // Use single liteserver from config
      CHECK(single_liteserver_idx_ >= 0 && (size_t)single_liteserver_idx_ < servers.size());
      td::TerminalIO::out() << "using liteserver #" << single_liteserver_idx_ << " with addr "
                            << servers[single_liteserver_idx_].addr << "\n";
      servers = {servers[single_liteserver_idx_]};
    }
  }
  CHECK(!servers.empty());
  client_ = liteclient::ExtClient::create(std::move(servers), nullptr);
  ready_ = true;

  run_init_queries();
}

void TestNode::got_result(td::Result<td::BufferSlice> R, td::Promise<td::BufferSlice> promise) {
  if (R.is_error()) {
    auto err = R.move_as_error();
    LOG(ERROR) << "failed query: " << err;
    promise.set_error(std::move(err));
    td::actor::send_closure_later(actor_id(this), &TestNode::after_got_result, false);
    return;
  }
  auto data = R.move_as_ok();
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_error>(data.clone(), true);
  if (F.is_ok()) {
    auto f = F.move_as_ok();
    auto err = td::Status::Error(f->code_, f->message_);
    LOG(ERROR) << "liteserver error: " << err;
    promise.set_error(std::move(err));
    td::actor::send_closure_later(actor_id(this), &TestNode::after_got_result, false);
    return;
  }
  promise.set_result(std::move(data));
  td::actor::send_closure_later(actor_id(this), &TestNode::after_got_result, true);
}

void TestNode::after_got_result(bool ok) {
  running_queries_--;
  if (ex_mode_ && !ok) {
    LOG(ERROR) << "fatal error executing command-line queries, skipping the rest";
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
  }
  if (!running_queries_ && ex_queries_.size() > 0) {
    auto data = std::move(ex_queries_[0]);
    ex_queries_.erase(ex_queries_.begin());
    parse_line(std::move(data));
  }
  if (ex_mode_ && !running_queries_ && ex_queries_.size() == 0) {
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(0);
  }
}

bool TestNode::envelope_send_query(td::BufferSlice query, td::Promise<td::BufferSlice> promise) {
  running_queries_++;
  if (!ready_ || client_.empty()) {
    got_result(td::Status::Error("failed to send query to server: not ready"), std::move(promise));
    return false;
  }
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        td::actor::send_closure(SelfId, &TestNode::got_result, std::move(R), std::move(promise));
      });
  td::BufferSlice b =
      ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_query>(std::move(query)), true);
  td::actor::send_closure(client_, &liteclient::ExtClient::send_query, "query", std::move(b), td::Timestamp::in(10.0),
                          std::move(P));
  return true;
}

td::Promise<td::Unit> TestNode::trivial_promise() {
  return td::PromiseCreator::lambda([Self = actor_id(this)](td::Result<td::Unit> res) {
    if (res.is_error()) {
      LOG(ERROR) << "error: " << res.move_as_error();
    }
  });
}

bool TestNode::register_blkid(const ton::BlockIdExt& blkid) {
  for (const auto& id : known_blk_ids_) {
    if (id == blkid) {
      return false;
    }
  }
  known_blk_ids_.push_back(blkid);
  return true;
}

bool TestNode::show_new_blkids(bool all) {
  if (all) {
    shown_blk_ids_ = 0;
  }
  int cnt = 0;
  while (shown_blk_ids_ < known_blk_ids_.size()) {
    td::TerminalIO::out() << "BLK#" << shown_blk_ids_ + 1 << " = " << known_blk_ids_[shown_blk_ids_].to_str()
                          << std::endl;
    ++shown_blk_ids_;
    ++cnt;
  }
  return cnt;
}

bool TestNode::complete_blkid(ton::BlockId partial_blkid, ton::BlockIdExt& complete_blkid) const {
  auto n = known_blk_ids_.size();
  while (n) {
    --n;
    if (known_blk_ids_[n].id == partial_blkid) {
      complete_blkid = known_blk_ids_[n];
      return true;
    }
  }
  if (partial_blkid.is_masterchain() && partial_blkid.seqno == ~0U) {
    complete_blkid.id = ton::BlockId{ton::masterchainId, ton::shardIdAll, ~0U};
    complete_blkid.root_hash.set_zero();
    complete_blkid.file_hash.set_zero();
    return true;
  }
  return false;
}

const tlb::TypenameLookup& TestNode::get_tlb_dict() {
  static tlb::TypenameLookup tlb_dict = []() {
    tlb::TypenameLookup tlb_dict0;
    tlb_dict0.register_types(block::gen::register_simple_types);
    return tlb_dict0;
  }();
  return tlb_dict;
}

bool TestNode::list_cached_cells() const {
  for (const auto& kv : cell_cache_) {
    td::TerminalIO::out() << kv.first.to_hex() << std::endl;
  }
  return true;
}

bool TestNode::dump_cached_cell(td::Slice hash_pfx, td::Slice type_name) {
  if (hash_pfx.size() > 64) {
    return false;
  }
  td::Bits256 hv_min;
  int len = (int)hv_min.from_hex(hash_pfx, true);
  if (len < 0 || len > 256) {
    return set_error("cannot parse hex cell hash prefix");
  }
  (hv_min.bits() + len).fill(false, 256 - len);
  const tlb::TLB* tptr = nullptr;
  block::gen::ConfigParam tpconf(0);
  if (type_name.size()) {
    td::int32 idx;
    if (type_name.substr(0, 11) == "ConfigParam" && convert_int32(type_name.substr(11), idx) && idx >= 0) {
      tpconf = block::gen::ConfigParam(idx);
      tptr = &tpconf;
    } else {
      tptr = get_tlb_dict().lookup(type_name);
    }
    if (!tptr) {
      return set_error("unknown TL-B type");
    }
    td::TerminalIO::out() << "dumping cells as values of TLB type " << tptr->get_type_name() << std::endl;
  }
  auto it = std::lower_bound(cell_cache_.begin(), cell_cache_.end(), hv_min,
                             [](const auto& x, const auto& y) { return x.first < y; });
  int cnt = 0;
  for (; it != cell_cache_.end() && it->first.bits().equals(hv_min.bits(), len); ++it) {
    std::ostringstream os;
    os << "C{" << it->first.to_hex() << "} =" << std::endl;
    vm::load_cell_slice(it->second).print_rec(print_limit_, os, 2);
    if (tptr) {
      tptr->print_ref(print_limit_, os, it->second, 2);
      os << std::endl;
    }
    td::TerminalIO::out() << os.str();
    ++cnt;
  }
  if (!cnt) {
    LOG(ERROR) << "no known cells with specified hash prefix";
    return false;
  }
  return true;
}

bool TestNode::get_server_time() {
  auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getTime>(), true);
  return envelope_send_query(std::move(b), [&, Self = actor_id(this)](td::Result<td::BufferSlice> res) -> void {
    if (res.is_error()) {
      LOG(ERROR) << "cannot get server time";
      return;
    } else {
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_currentTime>(res.move_as_ok(), true);
      if (F.is_error()) {
        LOG(ERROR) << "cannot parse answer to liteServer.getTime";
      } else {
        mc_server_time_ = F.move_as_ok()->now_;
        mc_server_time_got_at_ = now();
        LOG(INFO) << "server time is " << mc_server_time_ << " (delta " << mc_server_time_ - mc_server_time_got_at_
                  << ")";
      }
    }
  });
}

bool TestNode::get_server_version(int mode) {
  auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getVersion>(), true);
  return envelope_send_query(std::move(b), [Self = actor_id(this), mode](td::Result<td::BufferSlice> res) {
    td::actor::send_closure_later(Self, &TestNode::got_server_version, std::move(res), mode);
  });
};

void TestNode::got_server_version(td::Result<td::BufferSlice> res, int mode) {
  mc_server_ok_ = false;
  if (res.is_error()) {
    LOG(ERROR) << "cannot get server version and time (server too old?)";
  } else {
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_version>(res.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.getVersion";
    } else {
      auto a = F.move_as_ok();
      set_mc_server_version(a->version_, a->capabilities_);
      set_mc_server_time(a->now_);
    }
  }
  if (!mc_server_ok_) {
    LOG(ERROR) << "server version is too old (at least " << (min_ls_version >> 8) << "." << (min_ls_version & 0xff)
               << " with capabilities " << min_ls_capabilities << " required), some queries are unavailable";
  }
  if (mode & 0x100) {
    get_server_mc_block_id();
  }
}

void TestNode::set_mc_server_version(td::int32 version, td::int64 capabilities) {
  if (mc_server_version_ != version || mc_server_capabilities_ != capabilities) {
    mc_server_version_ = version;
    mc_server_capabilities_ = capabilities;
    LOG(WARNING) << "server version is " << (mc_server_version_ >> 8) << "." << (mc_server_version_ & 0xff)
                 << ", capabilities " << mc_server_capabilities_;
  }
  mc_server_ok_ = (mc_server_version_ >= min_ls_version) && !(~mc_server_capabilities_ & min_ls_capabilities);
}

void TestNode::set_mc_server_time(int server_utime) {
  mc_server_time_ = server_utime;
  mc_server_time_got_at_ = now();
  LOG(INFO) << "server time is " << mc_server_time_ << " (delta " << mc_server_time_ - mc_server_time_got_at_ << ")";
}

bool TestNode::get_server_mc_block_id() {
  int mode = (mc_server_capabilities_ & 2) ? 0 : -1;
  if (mode < 0) {
    auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
    return envelope_send_query(std::move(b), [Self = actor_id(this)](td::Result<td::BufferSlice> res) -> void {
      if (res.is_error()) {
        LOG(ERROR) << "cannot get masterchain info from server";
        return;
      } else {
        auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(res.move_as_ok(), true);
        if (F.is_error()) {
          LOG(ERROR) << "cannot parse answer to liteServer.getMasterchainInfo";
        } else {
          auto f = F.move_as_ok();
          auto blk_id = create_block_id(f->last_);
          auto zstate_id = create_zero_state_id(f->init_);
          LOG(INFO) << "last masterchain block is " << blk_id.to_str();
          td::actor::send_closure_later(Self, &TestNode::got_server_mc_block_id, blk_id, zstate_id, 0);
        }
      }
    });
  } else {
    auto b =
        ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfoExt>(mode), true);
    return envelope_send_query(std::move(b), [Self = actor_id(this), mode](td::Result<td::BufferSlice> res) -> void {
      if (res.is_error()) {
        LOG(ERROR) << "cannot get extended masterchain info from server";
        return;
      } else {
        auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfoExt>(res.move_as_ok(), true);
        if (F.is_error()) {
          LOG(ERROR) << "cannot parse answer to liteServer.getMasterchainInfoExt";
        } else {
          auto f = F.move_as_ok();
          auto blk_id = create_block_id(f->last_);
          auto zstate_id = create_zero_state_id(f->init_);
          LOG(INFO) << "last masterchain block is " << blk_id.to_str();
          td::actor::send_closure_later(Self, &TestNode::got_server_mc_block_id_ext, blk_id, zstate_id, mode,
                                        f->version_, f->capabilities_, f->last_utime_, f->now_);
        }
      }
    });
  }
}

void TestNode::got_server_mc_block_id(ton::BlockIdExt blkid, ton::ZeroStateIdExt zstateid, int created) {
  if (!zstate_id_.is_valid()) {
    zstate_id_ = zstateid;
    LOG(INFO) << "zerostate id set to " << zstate_id_.to_str();
  } else if (zstate_id_ != zstateid) {
    LOG(FATAL) << "fatal: masterchain zero state id suddenly changed: expected " << zstate_id_.to_str() << ", found "
               << zstateid.to_str();
    _exit(3);
    return;
  }
  register_blkid(blkid);
  register_blkid(ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, zstateid.root_hash, zstateid.file_hash});
  if (!mc_last_id_.is_valid()) {
    mc_last_id_ = blkid;
    request_block(blkid);
    // request_state(blkid);
  } else if (mc_last_id_.id.seqno < blkid.id.seqno) {
    mc_last_id_ = blkid;
  }
  td::TerminalIO::out() << "latest masterchain block known to server is " << blkid.to_str();
  if (created > 0) {
    td::TerminalIO::out() << " created at " << created << " (" << now() - created << " seconds ago)\n";
  } else {
    td::TerminalIO::out() << "\n";
  }
  show_new_blkids();
}

void TestNode::got_server_mc_block_id_ext(ton::BlockIdExt blkid, ton::ZeroStateIdExt zstateid, int mode, int version,
                                          long long capabilities, int last_utime, int server_now) {
  set_mc_server_version(version, capabilities);
  set_mc_server_time(server_now);
  if (last_utime > server_now) {
    LOG(WARNING) << "server claims to have a masterchain block " << blkid.to_str() << " created at " << last_utime
                 << " (" << last_utime - server_now << " seconds in the future)";
  } else if (last_utime < server_now - 60) {
    LOG(WARNING) << "server appears to be out of sync: its newest masterchain block is " << blkid.to_str()
                 << " created at " << last_utime << " (" << server_now - last_utime
                 << " seconds ago according to the server's clock)";
  } else if (last_utime < mc_server_time_got_at_ - 60) {
    LOG(WARNING) << "either the server is out of sync, or the local clock is set incorrectly: the newest masterchain "
                    "block known to server is "
                 << blkid.to_str() << " created at " << last_utime << " (" << server_now - mc_server_time_got_at_
                 << " seconds ago according to the local clock)";
  }
  got_server_mc_block_id(blkid, zstateid, last_utime);
}

bool TestNode::request_block(ton::BlockIdExt blkid) {
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(blkid)), true);
  return envelope_send_query(std::move(b), [Self = actor_id(this), blkid](td::Result<td::BufferSlice> res) -> void {
    if (res.is_error()) {
      LOG(ERROR) << "cannot obtain block " << blkid.to_str() << " from server";
      return;
    } else {
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(res.move_as_ok(), true);
      if (F.is_error()) {
        LOG(ERROR) << "cannot parse answer to liteServer.getBlock";
      } else {
        auto f = F.move_as_ok();
        auto blk_id = ton::create_block_id(f->id_);
        LOG(INFO) << "obtained block " << blk_id.to_str() << " from server";
        if (blk_id != blkid) {
          LOG(ERROR) << "block id mismatch: expected data for block " << blkid.to_str() << ", obtained for "
                     << blk_id.to_str();
        }
        td::actor::send_closure_later(Self, &TestNode::got_mc_block, blk_id, std::move(f->data_));
      }
    }
  });
}

bool TestNode::request_state(ton::BlockIdExt blkid) {
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getState>(ton::create_tl_lite_block_id(blkid)), true);
  return envelope_send_query(std::move(b), [Self = actor_id(this), blkid](td::Result<td::BufferSlice> res) -> void {
    if (res.is_error()) {
      LOG(ERROR) << "cannot obtain state " << blkid.to_str() << " from server";
      return;
    } else {
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockState>(res.move_as_ok(), true);
      if (F.is_error()) {
        LOG(ERROR) << "cannot parse answer to liteServer.getState";
      } else {
        auto f = F.move_as_ok();
        auto blk_id = ton::create_block_id(f->id_);
        LOG(INFO) << "obtained state " << blk_id.to_str() << " from server";
        if (blk_id != blkid) {
          LOG(ERROR) << "block id mismatch: expected state for block " << blkid.to_str() << ", obtained for "
                     << blk_id.to_str();
        }
        td::actor::send_closure_later(Self, &TestNode::got_mc_state, blk_id, f->root_hash_, f->file_hash_,
                                      std::move(f->data_));
      }
    }
  });
}

void TestNode::got_mc_block(ton::BlockIdExt blkid, td::BufferSlice data) {
  LOG(INFO) << "obtained " << data.size() << " data bytes for block " << blkid.to_str();
  ton::FileHash fhash;
  td::sha256(data.as_slice(), fhash.as_slice());
  if (fhash != blkid.file_hash) {
    LOG(ERROR) << "file hash mismatch for block " << blkid.to_str() << ": expected " << blkid.file_hash.to_hex()
               << ", computed " << fhash.to_hex();
    return;
  }
  register_blkid(blkid);
  last_block_id_ = blkid;
  last_block_data_ = data.clone();
  if (!db_root_.empty()) {
    auto res = save_db_file(fhash, std::move(data));
    if (res.is_error()) {
      LOG(ERROR) << "error saving block file: " << res.to_string();
    }
  }
  show_new_blkids();
}

void TestNode::got_mc_state(ton::BlockIdExt blkid, ton::RootHash root_hash, ton::FileHash file_hash,
                            td::BufferSlice data) {
  LOG(INFO) << "obtained " << data.size() << " state bytes for block " << blkid.to_str();
  ton::FileHash fhash;
  td::sha256(data.as_slice(), fhash.as_slice());
  if (fhash != file_hash) {
    LOG(ERROR) << "file hash mismatch for state " << blkid.to_str() << ": expected " << file_hash.to_hex()
               << ", computed " << fhash.to_hex();
    return;
  }
  register_blkid(blkid);
  last_state_id_ = blkid;
  last_state_data_ = data.clone();
  if (!db_root_.empty()) {
    auto res = save_db_file(fhash, std::move(data));
    if (res.is_error()) {
      LOG(ERROR) << "error saving state file: " << res.to_string();
    }
  }
  show_new_blkids();
}

td::Status TestNode::save_db_file(ton::FileHash file_hash, td::BufferSlice data) {
  std::string fname = block::compute_db_filename(db_root_ + '/', file_hash);
  for (int i = 0; i < 10; i++) {
    std::string tmp_fname = block::compute_db_tmp_filename(db_root_ + '/', file_hash, i);
    auto res = block::save_binary_file(tmp_fname, data);
    if (res.is_ok()) {
      if (std::rename(tmp_fname.c_str(), fname.c_str()) < 0) {
        int err = errno;
        LOG(ERROR) << "cannot rename " << tmp_fname << " to " << fname << " : " << std::strerror(err);
        return td::Status::Error(std::string{"cannot rename file: "} + std::strerror(err));
      } else {
        LOG(INFO) << data.size() << " bytes saved into file " << fname;
        return td::Status::OK();
      }
    } else if (i == 9) {
      return res;
    }
  }
  return td::Status::Error("cannot save data file");
}

void TestNode::run_init_queries() {
  get_server_version(0x100);
}

td::Slice TestNode::get_word(char delim) {
  if (delim == ' ' || !delim) {
    skipspc();
  }
  const char* ptr = parse_ptr_;
  while (ptr < parse_end_ && *ptr != delim && (*ptr != '\t' || delim != ' ')) {
    ptr++;
  }
  std::swap(ptr, parse_ptr_);
  return td::Slice{ptr, parse_ptr_};
}

td::Slice TestNode::get_word_ext(const char* delims, const char* specials) {
  if (delims[0] == ' ') {
    skipspc();
  }
  const char* ptr = parse_ptr_;
  while (ptr < parse_end_ && !strchr(delims, *ptr)) {
    if (specials && strchr(specials, *ptr)) {
      if (ptr == parse_ptr_) {
        ptr++;
      }
      break;
    }
    ptr++;
  }
  std::swap(ptr, parse_ptr_);
  return td::Slice{ptr, parse_ptr_};
}

bool TestNode::get_word_to(std::string& str, char delim) {
  str = get_word(delim).str();
  return !str.empty();
}

bool TestNode::get_word_to(td::Slice& str, char delim) {
  str = get_word(delim);
  return !str.empty();
}

int TestNode::skipspc() {
  int i = 0;
  while (parse_ptr_ < parse_end_ && (*parse_ptr_ == ' ' || *parse_ptr_ == '\t')) {
    i++;
    parse_ptr_++;
  }
  return i;
}

std::string TestNode::get_line_tail(bool remove_spaces) const {
  const char *ptr = parse_ptr_, *end = parse_end_;
  if (remove_spaces) {
    while (ptr < end && (*ptr == ' ' || *ptr == '\t')) {
      ptr++;
    }
    while (ptr < end && (end[-1] == ' ' || end[-1] == '\t')) {
      --end;
    }
  }
  return std::string{ptr, end};
}

bool TestNode::eoln() const {
  return parse_ptr_ == parse_end_;
}

bool TestNode::seekeoln() {
  skipspc();
  return eoln();
}

bool TestNode::parse_account_addr(ton::WorkchainId& wc, ton::StdSmcAddress& addr, bool allow_none) {
  auto word = get_word();
  if (allow_none && (word == "none" || word == "root")) {
    wc = ton::workchainInvalid;
    return true;
  }
  return block::parse_std_account_addr(word, wc, addr) || set_error("cannot parse account address");
}

bool TestNode::parse_account_addr_ext(ton::WorkchainId& wc, ton::StdSmcAddress& addr, int& addr_ext, bool allow_none) {
  addr_ext = 0;
  auto word = get_word();
  if (allow_none && (word == "none" || word == "root")) {
    wc = ton::workchainInvalid;
    return true;
  }
  if (word == "config" || word == "elector" || word == "dnsroot") {
    wc = ton::masterchainId;
    addr.set_zero();
    addr_ext = 1 + (word == "elector") + (word == "dnsroot") * 2;
    if (addr_ext == 1 && config_addr_queried_) {
      addr = config_addr_;
      addr_ext = 0;
    } else if (addr_ext == 2 && elect_addr_queried_) {
      addr = elect_addr_;
      addr_ext = 0;
    } else if (addr_ext == 3 && dns_root_queried_) {
      addr = dns_root_;
      addr_ext = 0;
    }
    return true;
  }
  return block::parse_std_account_addr(word, wc, addr) || set_error("cannot parse account address");
}

bool TestNode::convert_uint64(td::Slice word, td::uint64& val) {
  val = ~0ULL;
  if (word.empty()) {
    return false;
  }
  const char* ptr = word.data();
  char* end = nullptr;
  val = std::strtoull(ptr, &end, 10);
  if (end == ptr + word.size()) {
    return true;
  } else {
    val = ~0ULL;
    return false;
  }
}

bool TestNode::convert_int64(td::Slice word, td::int64& val) {
  val = (~0ULL << 63);
  if (word.empty()) {
    return false;
  }
  const char* ptr = word.data();
  char* end = nullptr;
  val = std::strtoll(ptr, &end, 10);
  if (end == ptr + word.size()) {
    return true;
  } else {
    val = (~0ULL << 63);
    return false;
  }
}

bool TestNode::convert_uint32(td::Slice word, td::uint32& val) {
  td::uint64 tmp;
  if (convert_uint64(word, tmp) && (td::uint32)tmp == tmp) {
    val = (td::uint32)tmp;
    return true;
  } else {
    return false;
  }
}

bool TestNode::convert_int32(td::Slice word, td::int32& val) {
  td::int64 tmp;
  if (convert_int64(word, tmp) && (td::int32)tmp == tmp) {
    val = (td::int32)tmp;
    return true;
  } else {
    return false;
  }
}

bool TestNode::parse_lt(ton::LogicalTime& lt) {
  return convert_uint64(get_word(), lt) || set_error("cannot parse logical time");
}

bool TestNode::parse_uint32(td::uint32& val) {
  return convert_uint32(get_word(), val) || set_error("cannot parse 32-bit unsigned integer");
}

bool TestNode::parse_int32(td::int32& val) {
  return convert_int32(get_word(), val) || set_error("cannot parse 32-bit integer");
}

bool TestNode::parse_int16(int& val) {
  return (convert_int32(get_word(), val) && val == (td::int16)val) || set_error("cannot parse 16-bit integer");
}

bool TestNode::set_error(td::Status error) {
  if (error.is_ok()) {
    return true;
  }
  LOG(ERROR) << "error: " << error.to_string();
  if (error_.is_ok()) {
    error_ = std::move(error);
  }
  return false;
}

int TestNode::parse_hex_digit(int c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c |= 0x20;
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 10;
  }
  return -1;
}

bool TestNode::parse_hash(td::Slice str, ton::Bits256& hash) {
  return str.size() == 64 && parse_hash(str.data(), hash);
}

bool TestNode::parse_hash(const char* str, ton::Bits256& hash) {
  unsigned char* data = hash.data();
  for (int i = 0; i < 32; i++) {
    int a = parse_hex_digit(str[2 * i]);
    if (a < 0) {
      return false;
    }
    int b = parse_hex_digit(str[2 * i + 1]);
    if (b < 0) {
      return false;
    }
    data[i] = (unsigned char)((a << 4) + b);
  }
  return true;
}

bool TestNode::parse_block_id_ext(std::string blkid_str, ton::BlockIdExt& blkid, bool allow_incomplete) const {
  if (blkid_str.empty()) {
    return false;
  }
  auto fc = blkid_str[0];
  if (fc == 'B' || fc == '#') {
    unsigned n = 0;
    if (sscanf(blkid_str.c_str(), fc == 'B' ? "BLK#%u" : "#%u", &n) != 1 || !n || n > known_blk_ids_.size()) {
      return false;
    }
    blkid = known_blk_ids_.at(n - 1);
    return true;
  }
  if (blkid_str[0] != '(') {
    return false;
  }
  auto pos = blkid_str.find(')');
  if (pos == std::string::npos || pos >= 38) {
    return false;
  }
  char buffer[40];
  std::memcpy(buffer, blkid_str.c_str(), pos + 1);
  buffer[pos + 1] = 0;
  unsigned long long shard;
  if (sscanf(buffer, "(%d,%016llx,%u)", &blkid.id.workchain, &shard, &blkid.id.seqno) != 3) {
    return false;
  }
  blkid.id.shard = shard;
  if (!blkid.id.is_valid_full()) {
    return false;
  }
  pos++;
  if (pos == blkid_str.size()) {
    blkid.root_hash.set_zero();
    blkid.file_hash.set_zero();
    return complete_blkid(blkid.id, blkid) || allow_incomplete;
  }
  return pos + 2 * 65 == blkid_str.size() && blkid_str[pos] == ':' && blkid_str[pos + 65] == ':' &&
         parse_hash(blkid_str.c_str() + pos + 1, blkid.root_hash) &&
         parse_hash(blkid_str.c_str() + pos + 66, blkid.file_hash) && blkid.is_valid_full();
}

bool TestNode::parse_block_id_ext(ton::BlockIdExt& blk, bool allow_incomplete) {
  return parse_block_id_ext(get_word().str(), blk, allow_incomplete) || set_error("cannot parse BlockIdExt");
}

bool TestNode::parse_hash(ton::Bits256& hash) {
  auto word = get_word();
  return parse_hash(word, hash) || set_error("cannot parse hash");
}

bool TestNode::convert_shard_id(td::Slice str, ton::ShardIdFull& shard) {
  shard.workchain = ton::workchainInvalid;
  shard.shard = 0;
  auto pos = str.find(':');
  if (pos == std::string::npos || pos > 10) {
    return false;
  }
  if (!convert_int32(str.substr(0, pos), shard.workchain)) {
    return false;
  }
  int t = 64;
  while (++pos < str.size()) {
    int z = parse_hex_digit(str[pos]);
    if (z < 0) {
      if (t == 64) {
        shard.shard = ton::shardIdAll;
      }
      return pos == str.size() - 1 && str[pos] == '_';
    }
    t -= 4;
    if (t >= 0) {
      shard.shard |= ((td::uint64)z << t);
    }
  }
  return true;
}

bool TestNode::parse_shard_id(ton::ShardIdFull& shard) {
  return convert_shard_id(get_word(), shard) || set_error("cannot parse full shard identifier or prefix");
}

bool TestNode::set_error(std::string err_msg) {
  return set_error(td::Status::Error(-1, err_msg));
}

void TestNode::parse_line(td::BufferSlice data) {
  line_ = data.as_slice().str();
  parse_ptr_ = line_.c_str();
  parse_end_ = parse_ptr_ + line_.size();
  error_ = td::Status::OK();
  if (seekeoln()) {
    return;
  }
  if (!do_parse_line() || error_.is_error()) {
    show_context();
    LOG(ERROR) << (error_.is_ok() ? "Syntax error" : error_.to_string());
    error_ = td::Status::OK();
  }
  show_new_blkids();
}

void TestNode::show_context() const {
  const char* ptr = line_.c_str();
  CHECK(parse_ptr_ >= ptr && parse_ptr_ <= parse_end_);
  auto out = td::TerminalIO::out();
  for (; ptr < parse_ptr_; ptr++) {
    out << (char)(*ptr == '\t' ? *ptr : ' ');
  }
  out << "^" << '\n';
}

bool TestNode::show_help(std::string command) {
  td::TerminalIO::out()
      << "list of available commands:\n"
         "time\tGet server time\n"
         "remote-version\tShows server time, version and capabilities\n"
         "last\tGet last block and state info from server\n"
         "sendfile <filename>\tLoad a serialized message from <filename> and send it to server\n"
         "status\tShow connection and local database status\n"
         "getaccount <addr> [<block-id-ext>]\tLoads the most recent state of specified account; <addr> is in "
         "[<workchain>:]<hex-or-base64-addr> format\n"
         "saveaccount[code|data] <filename> <addr> [<block-id-ext>]\tSaves into specified file the most recent state "
         "(StateInit) or just the code or data of specified account; <addr> is in "
         "[<workchain>:]<hex-or-base64-addr> format\n"
         "runmethod[full] <addr> [<block-id-ext>] <name> <params>...\tRuns GET method <name> of account "
         "<addr> "
         "with specified parameters\n"
         "dnsresolve [<block-id-ext>] <domain> [<category>]\tResolves a domain starting from root dns smart contract\n"
         "dnsresolvestep <addr> [<block-id-ext>] <domain> [<category>]\tResolves a subdomain using dns smart contract "
         "<addr>\n"
         "allshards [<block-id-ext>]\tShows shard configuration from the most recent masterchain "
         "state or from masterchain state corresponding to <block-id-ext>\n"
         "getconfig [<param>...]\tShows specified or all configuration parameters from the latest masterchain state\n"
         "getconfigfrom <block-id-ext> [<param>...]\tShows specified or all configuration parameters from the "
         "masterchain state of <block-id-ext>\n"
         "getkeyconfig <block-id-ext> [<param>...]\tShows specified or all configuration parameters from the "
         "previous key block with respect to <block-id-ext>\n"
         "saveconfig <filename> [<block-id-ext>]\tSaves all configuration parameters into specified file\n"
         "gethead <block-id-ext>\tShows block header for <block-id-ext>\n"
         "getblock <block-id-ext>\tDownloads block\n"
         "dumpblock <block-id-ext>\tDownloads and dumps specified block\n"
         "getstate <block-id-ext>\tDownloads state corresponding to specified block\n"
         "dumpstate <block-id-ext>\tDownloads and dumps state corresponding to specified block\n"
         "dumptrans <block-id-ext> <account-id> <trans-lt>\tDumps one transaction of specified account\n"
         "lasttrans[dump] <account-id> <trans-lt> <trans-hash> [<count>]\tShows or dumps specified transaction and "
         "several preceding "
         "ones\n"
         "listblocktrans[rev][meta] <block-id-ext> <count> [<start-account-id> <start-trans-lt>]\tLists block "
         "transactions, starting immediately after or before the specified one\n"
         "blkproofchain[step] <from-block-id-ext> [<to-block-id-ext>]\tDownloads and checks proof of validity of the "
         "second "
         "indicated block (or the last known masterchain block) starting from given block\n"
         "byseqno <workchain> <shard-prefix> <seqno>\tLooks up a block by workchain, shard and seqno, and shows its "
         "header\n"
         "bylt <workchain> <shard-prefix> <lt>\tLooks up a block by workchain, shard and logical time, and shows its "
         "header\n"
         "byutime <workchain> <shard-prefix> <utime>\tLooks up a block by workchain, shard and creation time, and "
         "shows its header\n"
         "creatorstats <block-id-ext> [<count> [<start-pubkey>]]\tLists block creator statistics by validator public "
         "key\n"
         "recentcreatorstats <block-id-ext> <start-utime> [<count> [<start-pubkey>]]\tLists block creator statistics "
         "updated after <start-utime> by validator public "
         "key\n"
         "checkload[all|severe][-v2] <start-utime> <end-utime> [<savefile-prefix>]\tChecks whether all validators "
         "worked properly during specified time "
         "interval, and optionally saves proofs into <savefile-prefix>-<n>.boc\n"
         "loadproofcheck <filename>\tChecks a validator misbehavior proof previously created by checkload\n"
         "pastvalsets\tLists known past validator set ids and their hashes\n"
         "savecomplaints <election-id> <filename-pfx>\tSaves all complaints registered for specified validator set id "
         "into files <filename-pfx><complaint-hash>.boc\n"
         "complaintprice <expires-in> <complaint-boc>\tComputes the price (in nanograms) for creating a complaint\n"
         "msgqueuesizes\tShows current sizes of outbound message queues in all shards\n"
         "dispatchqueueinfo <block-id>\tShows list of account dispatch queue of a block\n"
         "dispatchqueuemessages <block-id> <addr> [<after-lt>]\tShows deferred messages from account <addr>, lt > "
         "<after_lt>\n"
         "dispatchqueuemessagesall <block-id> [<after-addr> [<after-lt>]]\tShows messages from dispatch queue of a "
         "block, starting after <after_addr>, <after-lt>\n"
         "known\tShows the list of all known block ids\n"
         "knowncells\tShows the list of hashes of all known (cached) cells\n"
         "dumpcell <hex-hash-pfx>\nDumps a cached cell by a prefix of its hash\n"
         "dumpcellas <tlb-type> <hex-hash-pfx>\nFinds a cached cell by a prefix of its hash and prints it as a value "
         "of <tlb-type>\n"
         "privkey <filename>\tLoads a private key from file\n"
         "help [<command>]\tThis help\n"
         "quit\tExit\n";
  return true;
}

bool TestNode::do_parse_line() {
  ton::WorkchainId workchain = ton::masterchainId;  // change to basechain later
  int addr_ext = 0;
  ton::StdSmcAddress addr = ton::StdSmcAddress::zero();
  ton::BlockIdExt blkid{};
  ton::LogicalTime lt = 0;
  ton::Bits256 hash{};
  ton::ShardIdFull shard{};
  ton::BlockSeqno seqno{};
  ton::UnixTime utime{};
  unsigned count{};
  std::string word = get_word().str();
  skipspc();
  if (word == "time") {
    return eoln() && get_server_time();
  } else if (word == "remote-version") {
    return eoln() && get_server_version();
  } else if (word == "last") {
    return eoln() && get_server_mc_block_id();
  } else if (word == "sendfile") {
    return !eoln() && set_error(send_ext_msg_from_filename(get_line_tail()));
  } else if (word == "getaccount" || word == "getaccountprunned") {
    bool prunned = word == "getaccountprunned";
    return parse_account_addr_ext(workchain, addr, addr_ext) &&
           (seekeoln() ? get_account_state(workchain, addr, mc_last_id_, addr_ext, "", -1, prunned)
                       : parse_block_id_ext(blkid) && seekeoln() &&
                             get_account_state(workchain, addr, blkid, addr_ext, "", -1, prunned));
  } else if (word == "saveaccount" || word == "saveaccountcode" || word == "saveaccountdata") {
    std::string filename;
    int mode = ((word.c_str()[11] >> 1) & 3);
    return get_word_to(filename) && parse_account_addr_ext(workchain, addr, addr_ext) &&
           (seekeoln() ? get_account_state(workchain, addr, mc_last_id_, addr_ext, filename, mode)
                       : parse_block_id_ext(blkid) && seekeoln() &&
                             get_account_state(workchain, addr, blkid, addr_ext, filename, mode));
  } else if (word == "runmethod" || word == "runmethodx" || word == "runmethodfull") {
    std::string method;
    return parse_account_addr_ext(workchain, addr, addr_ext) && get_word_to(method) &&
           (parse_block_id_ext(method, blkid) ? get_word_to(method) : (blkid = mc_last_id_).is_valid()) &&
           parse_run_method(workchain, addr, blkid, addr_ext, method, word.size() <= 10);
  } else if (word == "dnsresolve" || word == "dnsresolvestep") {
    workchain = ton::workchainInvalid;
    bool step = (word.size() > 10);
    std::string domain;
    std::string cat_str;
    return (!step || parse_account_addr(workchain, addr)) && get_word_to(domain) &&
           (parse_block_id_ext(domain, blkid) ? get_word_to(domain) : (blkid = mc_last_id_).is_valid()) &&
           (seekeoln() || get_word_to(cat_str)) && seekeoln() &&
           dns_resolve_start(workchain, addr, blkid, domain,
                             cat_str.empty() ? td::Bits256::zero() : td::sha256_bits256(td::as_slice(cat_str)),
                             step ? 3 : 0);
  } else if (word == "allshards" || word == "allshardssave") {
    std::string filename;
    return (word.size() <= 9 || get_word_to(filename)) &&
           (seekeoln() ? get_all_shards(filename)
                       : (parse_block_id_ext(blkid) && seekeoln() && get_all_shards(filename, false, blkid)));
  } else if (word == "saveconfig") {
    blkid = mc_last_id_;
    std::string filename;
    return get_word_to(filename) && (seekeoln() || parse_block_id_ext(blkid)) && seekeoln() &&
           parse_get_config_params(blkid, -1, filename);
  } else if (word == "getconfig" || word == "getconfigfrom") {
    blkid = mc_last_id_;
    return (word == "getconfig" || parse_block_id_ext(blkid)) && parse_get_config_params(blkid, 0);
  } else if (word == "getkeyconfig") {
    return parse_block_id_ext(blkid) && parse_get_config_params(blkid, 0x8000);
  } else if (word == "getblock") {
    return parse_block_id_ext(blkid) && seekeoln() && get_block(blkid, false);
  } else if (word == "dumpblock") {
    return parse_block_id_ext(blkid) && seekeoln() && get_block(blkid, true);
  } else if (word == "getstate") {
    return parse_block_id_ext(blkid) && seekeoln() && get_state(blkid, false);
  } else if (word == "dumpstate") {
    return parse_block_id_ext(blkid) && seekeoln() && get_state(blkid, true);
  } else if (word == "gethead") {
    return parse_block_id_ext(blkid) && seekeoln() && get_show_block_header(blkid, 0xffff);
  } else if (word == "dumptrans") {
    return parse_block_id_ext(blkid) && parse_account_addr(workchain, addr) && parse_lt(lt) && seekeoln() &&
           get_one_transaction(blkid, workchain, addr, lt, true);
  } else if (word == "lasttrans" || word == "lasttransdump") {
    count = 10;
    return parse_account_addr(workchain, addr) && parse_lt(lt) && parse_hash(hash) &&
           (seekeoln() || parse_uint32(count)) && seekeoln() &&
           get_last_transactions(workchain, addr, lt, hash, count, word == "lasttransdump");
  } else if (word == "listblocktrans" || word == "listblocktransrev") {
    lt = 0;
    int mode = (word == "listblocktrans" ? 7 : 0x47);
    return parse_block_id_ext(blkid) && parse_uint32(count) &&
           (seekeoln() || (parse_hash(hash) && parse_lt(lt) && (mode |= 128) && seekeoln())) &&
           get_block_transactions(blkid, mode, count, hash, lt);
  } else if (word == "listblocktransmeta" || word == "listblocktransrevmeta") {
    lt = 0;
    int mode = (word == "listblocktransmeta" ? 7 : 0x47);
    mode |= 256;
    return parse_block_id_ext(blkid) && parse_uint32(count) &&
           (seekeoln() || (parse_hash(hash) && parse_lt(lt) && (mode |= 128) && seekeoln())) &&
           get_block_transactions(blkid, mode, count, hash, lt);
  } else if (word == "blkproofchain" || word == "blkproofchainstep") {
    ton::BlockIdExt blkid2{};
    return parse_block_id_ext(blkid) && (seekeoln() || parse_block_id_ext(blkid2)) && seekeoln() &&
           get_block_proof(blkid, blkid2, blkid2.is_valid() + (word == "blkproofchain") * 0x1000);
  } else if (word == "byseqno") {
    return parse_shard_id(shard) && parse_uint32(seqno) && seekeoln() && lookup_show_block(shard, 1, seqno);
  } else if (word == "byutime") {
    return parse_shard_id(shard) && parse_uint32(utime) && seekeoln() && lookup_show_block(shard, 4, utime);
  } else if (word == "bylt") {
    return parse_shard_id(shard) && parse_lt(lt) && seekeoln() && lookup_show_block(shard, 2, lt);
  } else if (word == "creatorstats" || word == "recentcreatorstats") {
    count = 1000;
    int mode = (word == "recentcreatorstats" ? 4 : 0);
    return parse_block_id_ext(blkid) && (!mode || parse_uint32(utime)) &&
           (seekeoln() ? (mode |= 0x100) : parse_uint32(count)) && (seekeoln() || (parse_hash(hash) && (mode |= 1))) &&
           seekeoln() && get_creator_stats(blkid, mode, count, hash, utime);
  } else if (word == "checkload" || word == "checkloadall" || word == "checkloadsevere" || word == "checkload-v2" ||
             word == "checkloadall-v2" || word == "checkloadsevere-v2") {
    int time1, time2, mode = 0;
    if (word == "checkloadsevere" || word == "checkloadsevere-v2") {
      mode |= 1;
    }
    if (td::ends_with(word, "-v2")) {
      mode |= 4;
    }
    std::string file_pfx;
    return parse_int32(time1) && parse_int32(time2) && (seekeoln() || ((mode |= 2) && get_word_to(file_pfx))) &&
           seekeoln() && check_validator_load(time1, time2, mode, file_pfx);
  } else if (word == "loadproofcheck") {
    std::string filename;
    return get_word_to(filename) && seekeoln() && set_error(check_validator_load_proof(filename));
  } else if (word == "pastvalsets") {
    return eoln() && get_past_validator_sets();
  } else if (word == "savecomplaints") {
    td::uint32 elect_id;
    std::string file_pfx;
    return parse_uint32(elect_id) && get_word_to(file_pfx) && seekeoln() && get_complaints(elect_id, file_pfx);
  } else if (word == "complaintprice") {
    td::uint32 expire_in;
    std::string filename;
    return parse_uint32(expire_in) && get_word_to(filename) && seekeoln() &&
           set_error(get_complaint_price(expire_in, filename));
  } else if (word == "msgqueuesizes") {
    return get_msg_queue_sizes();
  } else if (word == "dispatchqueueinfo") {
    return parse_block_id_ext(blkid) && seekeoln() && get_dispatch_queue_info(blkid);
  } else if (word == "dispatchqueuemessages" || word == "dispatchqueuemessagesall") {
    bool one_account = word == "dispatchqueuemessages";
    if (!parse_block_id_ext(blkid)) {
      return false;
    }
    workchain = blkid.id.workchain;
    return ((!one_account && seekeoln()) || parse_account_addr(workchain, addr)) && (seekeoln() || parse_lt(lt)) &&
           seekeoln() && get_dispatch_queue_messages(blkid, workchain, addr, lt, one_account);
  } else if (word == "known") {
    return eoln() && show_new_blkids(true);
  } else if (word == "knowncells") {
    return eoln() && list_cached_cells();
  } else if (word == "dumpcell" || word == "dumpcellas") {
    td::Slice chash;
    td::Slice tname;
    return (word == "dumpcell" || get_word_to(tname)) && get_word_to(chash) && seekeoln() &&
           dump_cached_cell(chash, tname);
  } else if (word == "quit" && eoln()) {
    LOG(INFO) << "Exiting";
    stop();
    // std::exit(0);
    return true;
  } else if (word == "help") {
    return show_help(get_line_tail());
  } else {
    td::TerminalIO::out() << "unknown command: " << word << " ; type `help` to get help" << '\n';
    return false;
  }
}

td::Result<std::pair<Ref<vm::Cell>, std::shared_ptr<vm::StaticBagOfCellsDb>>> lazy_boc_deserialize(
    td::BufferSlice data) {
  vm::StaticBagOfCellsDbLazy::Options options;
  options.check_crc32c = true;
  TRY_RESULT(boc, vm::StaticBagOfCellsDbLazy::create(td::BufferSliceBlobView::create(std::move(data)), options));
  TRY_RESULT(rc, boc->get_root_count());
  if (rc != 1) {
    return td::Status::Error(-668, "bag-of-cells is not standard (exactly one root cell expected)");
  }
  TRY_RESULT(root, boc->get_root_cell(0));
  return std::make_pair(std::move(root), std::move(boc));
}

td::Status TestNode::send_ext_msg_from_filename(std::string filename) {
  auto F = td::read_file(filename);
  if (F.is_error()) {
    auto err = F.move_as_error();
    LOG(ERROR) << "failed to read file `" << filename << "`: " << err.to_string();
    return err;
  }
  if (ready_ && !client_.empty()) {
    LOG(ERROR) << "sending query from file " << filename;
    auto P = td::PromiseCreator::lambda([](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        return;
      }
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatus>(R.move_as_ok(), true);
      if (F.is_error()) {
        LOG(ERROR) << "cannot parse answer to liteServer.sendMessage";
      } else {
        int status = F.move_as_ok()->status_;
        LOG(INFO) << "external message status is " << status;
      }
    });
    auto b =
        ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(F.move_as_ok()), true);
    return envelope_send_query(std::move(b), std::move(P)) ? td::Status::OK()
                                                           : td::Status::Error("cannot send query to server");
  } else {
    return td::Status::Error("server connection not ready");
  }
}

bool TestNode::get_account_state(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt ref_blkid,
                                 int addr_ext, std::string filename, int mode, bool prunned) {
  if (!ref_blkid.is_valid()) {
    return set_error("must obtain last block information before making other queries");
  }
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  if (addr_ext) {
    return get_special_smc_addr(
        addr_ext, [this, ref_blkid, filename, mode, prunned](td::Result<ton::StdSmcAddress> res) {
          if (res.is_error()) {
            LOG(ERROR) << "cannot resolve special smart contract address: " << res.move_as_error();
          } else {
            get_account_state(ton::masterchainId, res.move_as_ok(), ref_blkid, 0, filename, mode, prunned);
          }
        });
  }
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(workchain, addr);
  td::BufferSlice b;
  if (prunned) {
    b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountStatePrunned>(
                                     ton::create_tl_lite_block_id(ref_blkid), std::move(a)),
                                 true);
  } else {
    b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
                                     ton::create_tl_lite_block_id(ref_blkid), std::move(a)),
                                 true);
  }
  LOG(INFO) << "requesting " << (prunned ? "prunned " : "") << "account state for " << workchain << ":" << addr.to_hex()
            << " with respect to " << ref_blkid.to_str() << " with savefile `" << filename << "` and mode " << mode;
  return envelope_send_query(std::move(b), [Self = actor_id(this), workchain, addr, ref_blkid, filename, mode,
                                            prunned](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(R.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.getAccountState";
    } else {
      auto f = F.move_as_ok();
      td::actor::send_closure_later(Self, &TestNode::got_account_state, ref_blkid, ton::create_block_id(f->id_),
                                    ton::create_block_id(f->shardblk_), std::move(f->shard_proof_),
                                    std::move(f->proof_), std::move(f->state_), workchain, addr, filename, mode,
                                    prunned);
    }
  });
}

td::int64 TestNode::compute_method_id(std::string method) {
  td::int64 method_id;
  if (!convert_int64(method, method_id)) {
    method_id = (td::crc16(td::Slice{method}) & 0xffff) | 0x10000;
  }
  return method_id;
}

bool TestNode::cache_cell(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return false;
  }
  td::Bits256 hash = cell->get_hash().bits();
  LOG(INFO) << "caching cell " << hash.to_hex();
  auto res = cell_cache_.emplace(hash, std::move(cell));
  return res.second;
}

bool TestNode::parse_run_method(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt ref_blkid,
                                int addr_ext, std::string method_name, bool ext_mode) {
  auto R = vm::parse_stack_entries(td::Slice(parse_ptr_, parse_end_));
  if (R.is_error()) {
    return set_error(R.move_as_error().to_string());
  }
  parse_ptr_ = parse_end_;
  if (addr_ext) {
    return get_special_smc_addr(addr_ext, [this, ref_blkid, method_name, ext_mode,
                                           args = R.move_as_ok()](td::Result<ton::StdSmcAddress> res) mutable {
      if (res.is_error()) {
        LOG(ERROR) << "cannot resolve special smart contract address: " << res.move_as_error();
      } else {
        after_parse_run_method(ton::masterchainId, res.move_as_ok(), ref_blkid, method_name, std::move(args), ext_mode);
      }
    });
  }
  return after_parse_run_method(workchain, addr, ref_blkid, method_name, R.move_as_ok(), ext_mode);
}

bool TestNode::after_parse_run_method(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt ref_blkid,
                                      std::string method_name, std::vector<vm::StackEntry> params, bool ext_mode) {
  auto P = td::PromiseCreator::lambda([this](td::Result<std::vector<vm::StackEntry>> R) {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error();
    } else {
      for (const auto& v : R.move_as_ok()) {
        v.for_each_scalar([this](const vm::StackEntry& val) {
          if (val.is_cell()) {
            cache_cell(val.as_cell());
          }
        });
      }
    }
  });
  return start_run_method(workchain, addr, ref_blkid, method_name, std::move(params), ext_mode ? 0x17 : 0,
                          std::move(P));
}

bool TestNode::start_run_method(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt ref_blkid,
                                std::string method_name, std::vector<vm::StackEntry> params, int mode,
                                td::Promise<std::vector<vm::StackEntry>> promise) {
  if (!ref_blkid.is_valid()) {
    return set_error("must obtain last block information before making other queries");
  }
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(workchain, addr);
  if (!mode) {
    auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
                                          ton::create_tl_lite_block_id(ref_blkid), std::move(a)),
                                      true);
    LOG(INFO) << "requesting account state for " << workchain << ":" << addr.to_hex() << " with respect to "
              << ref_blkid.to_str() << " to run method " << method_name << " with " << params.size() << " parameters";
    return envelope_send_query(
        std::move(b), [Self = actor_id(this), workchain, addr, ref_blkid, method_name, params = std::move(params),
                       promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            promise.set_error(R.move_as_error());
            return;
          }
          auto F = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(R.move_as_ok(), true);
          if (F.is_error()) {
            LOG(ERROR) << "cannot parse answer to liteServer.getAccountState";
            promise.set_error(td::Status::Error("cannot parse answer to liteServer.getAccountState"));
          } else {
            auto f = F.move_as_ok();
            td::actor::send_closure_later(Self, &TestNode::run_smc_method, 0, ref_blkid, ton::create_block_id(f->id_),
                                          ton::create_block_id(f->shardblk_), std::move(f->shard_proof_),
                                          std::move(f->proof_), std::move(f->state_), workchain, addr, method_name,
                                          std::move(params), td::BufferSlice(), td::BufferSlice(), td::BufferSlice(),
                                          -0x10000, std::move(promise));
          }
        });
  } else {
    td::int64 method_id = compute_method_id(method_name);
    // set serialization limits
    vm::FakeVmStateLimits fstate(1000);  // limit recursive (de)serialization calls
    vm::VmStateInterface::Guard guard(&fstate);
    // serialize parameters
    vm::CellBuilder cb;
    Ref<vm::Cell> cell;
    if (!(vm::Stack{params}.serialize(cb) && cb.finalize_to(cell))) {
      return set_error("cannot serialize stack with get-method parameters");
    }
    auto stk = vm::std_boc_serialize(std::move(cell));
    if (stk.is_error()) {
      return set_error("cannot serialize stack with get-method parameters : "s + stk.move_as_error().to_string());
    }
    auto b = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_runSmcMethod>(mode, ton::create_tl_lite_block_id(ref_blkid),
                                                                      std::move(a), method_id, stk.move_as_ok()),
        true);
    LOG(INFO) << "requesting remote get-method execution for " << workchain << ":" << addr.to_hex()
              << " with respect to " << ref_blkid.to_str() << " to run method " << method_name << " with "
              << params.size() << " parameters";
    return envelope_send_query(std::move(b), [Self = actor_id(this), workchain, addr, ref_blkid, method_name, mode,
                                              params = std::move(params),
                                              promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
      if (R.is_error()) {
        promise.set_error(R.move_as_error());
        return;
      }
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_runMethodResult>(R.move_as_ok(), true);
      if (F.is_error()) {
        LOG(ERROR) << "cannot parse answer to liteServer.runSmcMethod";
        promise.set_error(td::Status::Error("cannot parse answer to liteServer.runSmcMethod"));
      } else {
        auto f = F.move_as_ok();
        td::actor::send_closure_later(Self, &TestNode::run_smc_method, mode, ref_blkid, ton::create_block_id(f->id_),
                                      ton::create_block_id(f->shardblk_), std::move(f->shard_proof_),
                                      std::move(f->proof_), std::move(f->state_proof_), workchain, addr, method_name,
                                      std::move(params), std::move(f->init_c7_), std::move(f->lib_extras_),
                                      std::move(f->result_), f->exit_code_, std::move(promise));
      }
    });
  }
}

bool TestNode::get_config_addr(td::Promise<ton::StdSmcAddress> promise) {
  if (config_addr_queried_) {
    promise.set_result(config_addr_);
    return true;
  }
  auto P = td::PromiseCreator::lambda([this, promise = std::move(promise)](
                                          td::Result<std::unique_ptr<block::Config>> R) mutable {
    TRY_RESULT_PROMISE_PREFIX(promise, config, std::move(R), "cannot obtain configurator address from configuration:");
    if (config_addr_queried_) {
      promise.set_result(config_addr_);
    } else {
      promise.set_error(td::Status::Error("cannot obtain configurator address from configuration parameter #0"));
    }
  });
  return get_config_params(mc_last_id_, std::move(P), 0x3000, "", {0});
}

bool TestNode::get_elector_addr(td::Promise<ton::StdSmcAddress> promise) {
  if (elect_addr_queried_) {
    promise.set_result(elect_addr_);
    return true;
  }
  auto P = td::PromiseCreator::lambda(
      [this, promise = std::move(promise)](td::Result<std::unique_ptr<block::Config>> R) mutable {
        TRY_RESULT_PROMISE_PREFIX(promise, config, std::move(R), "cannot obtain elector address from configuration:");
        if (elect_addr_queried_) {
          promise.set_result(elect_addr_);
        } else {
          promise.set_error(td::Status::Error("cannot obtain elector address from configuration parameter #1"));
        }
      });
  return get_config_params(mc_last_id_, std::move(P), 0x3000, "", {1});
}

bool TestNode::get_dns_root(td::Promise<ton::StdSmcAddress> promise) {
  if (dns_root_queried_) {
    promise.set_result(dns_root_);
    return true;
  }
  auto P = td::PromiseCreator::lambda(
      [this, promise = std::move(promise)](td::Result<std::unique_ptr<block::Config>> R) mutable {
        TRY_RESULT_PROMISE_PREFIX(promise, config, std::move(R), "cannot obtain dns root address from configuration:");
        if (dns_root_queried_) {
          promise.set_result(dns_root_);
        } else {
          promise.set_error(td::Status::Error("cannot obtain dns root address from configuration parameter #4"));
        }
      });
  return get_config_params(mc_last_id_, std::move(P), 0x3000, "", {4});
}

bool TestNode::get_special_smc_addr(int addr_ext, td::Promise<ton::StdSmcAddress> promise) {
  switch (addr_ext) {
    case 1:
      return get_config_addr(std::move(promise));
    case 2:
      return get_elector_addr(std::move(promise));
    case 3:
      return get_dns_root(std::move(promise));
    default:
      promise.set_error(td::Status::Error(PSLICE() << "unknown special smart contract address class " << addr_ext));
      return false;
  }
}

bool TestNode::get_past_validator_sets() {
  return get_elector_addr([this](td::Result<ton::StdSmcAddress> res) {
    if (res.is_error()) {
      LOG(ERROR) << res.move_as_error();
    } else {
      send_past_vset_query(res.move_as_ok());
    }
  });
}

bool TestNode::send_past_vset_query(ton::StdSmcAddress elector_addr) {
  std::vector<vm::StackEntry> params;
  auto P = td::PromiseCreator::lambda([this](td::Result<std::vector<vm::StackEntry>> R) {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error();
      return;
    }
    auto S = R.move_as_ok();
    if (S.size() < 1 || !S.back().is_list()) {
      LOG(ERROR) << "past_elections_list did not return a value of type tuple";
      return;
    }
    register_past_vset_info(std::move(S.back()));
  });
  return start_run_method(ton::masterchainId, elector_addr, mc_last_id_, "past_elections_list", std::move(params), 0x17,
                          std::move(P));
}

void TestNode::register_past_vset_info(vm::StackEntry list) {
  try {
    while (!list.empty()) {
      auto tup = std::move(list).as_tuple_range(2, 2);
      if (tup.is_null()) {
        LOG(ERROR) << "invalid list in the result of past_elections_list";
        return;
      }
      list = tup->at(1);
      auto t2 = tup->at(0).as_tuple_range(255, 3);
      if (t2.is_null()) {
        LOG(ERROR) << "invalid list entry in the result of past_elections_list";
        return;
      }
      auto x = t2->at(0).as_int(), y = t2->at(2).as_int();
      if (x.is_null() || y.is_null() || !x->unsigned_fits_bits(32) || !y->unsigned_fits_bits(256)) {
        LOG(ERROR) << "invalid components in a list entry in the result of past_elections_list";
        return;
      }
      td::TerminalIO::out() << "PAST_VSET\t" << td::dec_string(x) << "\t" << td::hex_string(y, true, 64) << std::endl;
    }
  } catch (vm::VmError& err) {
    LOG(ERROR) << "vm error while scanning result: " << err.get_msg();
  }
}

bool TestNode::get_complaints(unsigned elect_id, std::string file_pfx) {
  return get_elector_addr([this, elect_id, file_pfx](td::Result<ton::StdSmcAddress> res) {
    if (res.is_error()) {
      LOG(ERROR) << res.move_as_error();
    } else {
      send_get_complaints_query(elect_id, res.move_as_ok(), file_pfx);
    }
  });
}

void TestNode::send_get_complaints_query(unsigned elect_id, ton::StdSmcAddress elector_addr, std::string file_pfx) {
  std::vector<vm::StackEntry> params;
  params.emplace_back(td::make_refint(elect_id));
  auto P = td::PromiseCreator::lambda([this, elect_id, file_pfx](td::Result<std::vector<vm::StackEntry>> R) {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error();
      return;
    }
    auto S = R.move_as_ok();
    if (S.size() < 1 || !(S.back().empty() || S.back().is_cell())) {
      LOG(ERROR) << "get_past_complaints did not return a value of type cell";
      return;
    }
    try {
      save_complaints(elect_id, std::move(S.back()).as_cell(), file_pfx);
    } catch (vm::VmError& err) {
      LOG(ERROR) << "vm error: " << err.get_msg();
    } catch (vm::VmVirtError& err) {
      LOG(ERROR) << "vm virtualization error: " << err.get_msg();
    }
  });
  start_run_method(ton::masterchainId, elector_addr, mc_last_id_, "get_past_complaints", std::move(params), 0x17,
                   std::move(P));
}

void TestNode::save_complaints(unsigned elect_id, Ref<vm::Cell> complaints, std::string file_pfx) {
  vm::Dictionary dict{std::move(complaints), 256};
  for (auto entry : dict) {
    block::gen::ValidatorComplaintStatus::Record rec;
    block::gen::ValidatorComplaint::Record rec2;
    if (!(tlb::csr_unpack(std::move(entry.second), rec) && tlb::unpack_cell(rec.complaint, rec2))) {
      LOG(ERROR) << "cannot unpack complaint with key " << entry.first.to_hex(256);
      break;
    }
    if (rec.complaint->get_hash().as_bitslice() != entry.first) {
      LOG(ERROR) << "invalid complaint key " << entry.first.to_hex(256) << ": actual complaint hash is "
                 << rec.complaint->get_hash().to_hex();
      break;
    }
    std::string filename = file_pfx + entry.first.to_hex(256) + ".boc";
    auto boc = vm::std_boc_serialize(rec.complaint, 2);
    if (boc.is_error()) {
      LOG(ERROR) << "cannot serialize complaint";
      break;
    }
    auto len = boc.ok().size();
    auto res1 = td::write_file(filename, boc.move_as_ok());
    if (res1.is_error()) {
      LOG(ERROR) << "cannot save serialized complaint to file `" << filename << "` : " << res1.move_as_error();
      return;
    }
    LOG(DEBUG) << "saved " << len << " bytes into file `" << filename << "`";
    td::TerminalIO::out() << "SAVE_COMPLAINT\t" << elect_id << '\t' << entry.first.to_hex(256) << '\t'
                          << rec2.validator_pubkey.to_hex() << '\t' << rec2.created_at << '\t' << filename << std::endl;
  }
}

td::Status TestNode::get_complaint_price(unsigned expires_in, std::string filename) {
  LOG(DEBUG) << "reading complaint file " << filename;
  TRY_RESULT_PREFIX(data, td::read_file(filename), "cannot read complaint file:");
  TRY_RESULT_PREFIX(complaint, vm::std_boc_deserialize(data),
                    PSLICE() << "cannot deserialize bag-of-cells read from complaint file `" << filename << "`:");
  if (complaint.is_null()) {
    return td::Status::Error("complaint is null");
  }
  block::gen::ValidatorComplaint::Record rec;
  if (!tlb::unpack_cell(complaint, rec)) {
    return td::Status::Error("cannot deserialize complaint");
  }
  td::Bits256 chash = complaint->get_hash().bits();
  vm::VmStorageStat stat{1 << 22};
  if (!stat.add_storage(std::move(complaint))) {
    return td::Status::Error("cannot compute storage size for this complaint");
  }
  return get_complaint_price(expires_in, (unsigned)stat.bits, (unsigned)stat.refs, chash, filename);
}

td::Status TestNode::get_complaint_price(unsigned expires_in, unsigned bits, unsigned refs, td::Bits256 chash,
                                         std::string filename) {
  LOG(INFO) << "complaint `" << filename << "`: " << bits << " bits, " << refs << " references";
  return get_elector_addr([this, filename, expires_in, bits, refs, chash](td::Result<ton::StdSmcAddress> res) {
    if (res.is_error()) {
      LOG(ERROR) << res.move_as_error();
    } else {
      send_compute_complaint_price_query(res.move_as_ok(), expires_in, bits, refs, chash, filename);
    }
  })
             ? td::Status::OK()
             : td::Status::Error("cannot obtain elector address");
}

void TestNode::send_compute_complaint_price_query(ton::StdSmcAddress elector_addr, unsigned expires_in, unsigned bits,
                                                  unsigned refs, td::Bits256 chash, std::string filename) {
  std::vector<vm::StackEntry> params;
  params.emplace_back(td::make_refint(bits));
  params.emplace_back(td::make_refint(refs));
  params.emplace_back(td::make_refint(expires_in));
  auto P = td::PromiseCreator::lambda(
      [this, expires_in, bits, refs, chash, filename](td::Result<std::vector<vm::StackEntry>> R) {
        if (R.is_error()) {
          LOG(ERROR) << R.move_as_error();
          return;
        }
        auto S = R.move_as_ok();
        if (S.size() < 1 || !S.back().is_int()) {
          LOG(ERROR) << "complaint_storage_price did not return a value of type cell";
          return;
        }
        try {
          auto price = std::move(S.back()).as_int();
          td::TerminalIO::out() << "COMPLAINT_PRICE\t" << chash.to_hex() << '\t' << td::dec_string(price) << '\t'
                                << bits << '\t' << refs << '\t' << expires_in << '\t' << filename << std::endl;
        } catch (vm::VmError& err) {
          LOG(ERROR) << "vm error: " << err.get_msg();
        } catch (vm::VmVirtError& err) {
          LOG(ERROR) << "vm virtualization error: " << err.get_msg();
        }
      });
  start_run_method(ton::masterchainId, elector_addr, mc_last_id_, "complaint_storage_price", std::move(params), 0x17,
                   std::move(P));
}

bool TestNode::get_msg_queue_sizes() {
  ton::BlockIdExt blkid = mc_last_id_;
  if (!blkid.is_valid_full()) {
    return set_error("must obtain last block information before making other queries");
  }
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  auto b =
      ton::create_serialize_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(blkid));
  LOG(INFO) << "requesting recent shard configuration";
  return envelope_send_query(std::move(b), [Self = actor_id(this), blkid](td::Result<td::BufferSlice> R) -> void {
    if (R.is_error()) {
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(R.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.getAllShardsInfo";
    } else {
      auto f = F.move_as_ok();
      td::actor::send_closure_later(Self, &TestNode::get_msg_queue_sizes_cont, blkid, std::move(f->data_));
    }
  });
}

void TestNode::get_msg_queue_sizes_cont(ton::BlockIdExt mc_blkid, td::BufferSlice data) {
  LOG(INFO) << "got shard configuration with respect to block " << mc_blkid.to_str();
  std::vector<ton::BlockIdExt> blocks;
  blocks.push_back(mc_blkid);
  auto R = vm::std_boc_deserialize(data.clone());
  if (R.is_error()) {
    set_error(R.move_as_error_prefix("cannot deserialize shard configuration: "));
    return;
  }
  auto root = R.move_as_ok();
  block::ShardConfig sh_conf;
  if (!sh_conf.unpack(vm::load_cell_slice_ref(root))) {
    set_error("cannot extract shard block list from shard configuration");
    return;
  }
  auto ids = sh_conf.get_shard_hash_ids(true);
  for (auto id : ids) {
    auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
    if (ref.not_null()) {
      blocks.push_back(ref->top_block_id());
    }
  }

  struct QueryInfo {
    std::vector<ton::BlockIdExt> blocks;
    std::vector<td::uint64> sizes;
    size_t pending;
  };
  auto info = std::make_shared<QueryInfo>();
  info->blocks = std::move(blocks);
  info->sizes.resize(info->blocks.size(), 0);
  info->pending = info->blocks.size();

  for (size_t i = 0; i < info->blocks.size(); ++i) {
    ton::BlockIdExt block_id = info->blocks[i];
    auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_getBlockOutMsgQueueSize>(
        0, ton::create_tl_lite_block_id(block_id), false);
    LOG(DEBUG) << "requesting queue size for block " << block_id.to_str();
    envelope_send_query(std::move(b), [=, this](td::Result<td::BufferSlice> R) -> void {
      if (R.is_error()) {
        return;
      }
      auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockOutMsgQueueSize>(R.move_as_ok(), true);
      if (F.is_error()) {
        set_error(F.move_as_error_prefix("failed to get queue size: "));
        return;
      }
      auto f = F.move_as_ok();
      LOG(DEBUG) << "got queue size for block " << block_id.to_str() << " : " << f->size_;
      info->sizes[i] = f->size_;
      if (--info->pending == 0) {
        get_msg_queue_sizes_finish(std::move(info->blocks), std::move(info->sizes));
      }
    });
  }
}

void TestNode::get_msg_queue_sizes_finish(std::vector<ton::BlockIdExt> blocks, std::vector<td::uint64> sizes) {
  CHECK(blocks.size() == sizes.size());
  td::TerminalIO::out() << "Outbound message queue sizes:" << std::endl;
  for (size_t i = 0; i < blocks.size(); ++i) {
    td::TerminalIO::out() << blocks[i].id.to_str() << "    " << sizes[i] << std::endl;
  }
}

bool TestNode::get_dispatch_queue_info(ton::BlockIdExt block_id) {
  td::TerminalIO::out() << "Dispatch queue in block: " << block_id.id.to_str() << std::endl;
  return get_dispatch_queue_info_cont(block_id, true, td::Bits256::zero());
}

bool TestNode::get_dispatch_queue_info_cont(ton::BlockIdExt block_id, bool first, td::Bits256 after_addr) {
  auto q = ton::create_serialize_tl_object<ton::lite_api::liteServer_getDispatchQueueInfo>(
      first ? 0 : 2, ton::create_tl_lite_block_id(block_id), after_addr, 32, false);
  return envelope_send_query(std::move(q), [=, Self = actor_id(this)](td::Result<td::BufferSlice> res) -> void {
    if (res.is_error()) {
      LOG(ERROR) << "liteServer.getDispatchQueueInfo error: " << res.move_as_error();
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_dispatchQueueInfo>(res.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.getDispatchQueueInfo";
      return;
    }
    td::actor::send_closure_later(Self, &TestNode::got_dispatch_queue_info, block_id, F.move_as_ok());
  });
}

void TestNode::got_dispatch_queue_info(ton::BlockIdExt block_id,
                                       ton::tl_object_ptr<ton::lite_api::liteServer_dispatchQueueInfo> info) {
  for (auto& acc : info->account_dispatch_queues_) {
    td::TerminalIO::out() << block_id.id.workchain << ":" << acc->addr_.to_hex() << " : size=" << acc->size_
                          << " lt=" << acc->min_lt_ << ".." << acc->max_lt_ << std::endl;
  }
  if (info->complete_) {
    td::TerminalIO::out() << "Done" << std::endl;
    return;
  }
  get_dispatch_queue_info_cont(block_id, false, info->account_dispatch_queues_.back()->addr_);
}

bool TestNode::get_dispatch_queue_messages(ton::BlockIdExt block_id, ton::WorkchainId wc, ton::StdSmcAddress addr,
                                           ton::LogicalTime lt, bool one_account) {
  if (wc != block_id.id.workchain) {
    return set_error("workchain mismatch");
  }
  auto q = ton::create_serialize_tl_object<ton::lite_api::liteServer_getDispatchQueueMessages>(
      one_account ? 2 : 0, ton::create_tl_lite_block_id(block_id), addr, lt, 64, false, one_account, false);
  return envelope_send_query(std::move(q), [=, Self = actor_id(this)](td::Result<td::BufferSlice> res) -> void {
    if (res.is_error()) {
      LOG(ERROR) << "liteServer.getDispatchQueueMessages error: " << res.move_as_error();
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_dispatchQueueMessages>(res.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.getDispatchQueueMessages";
      return;
    }
    td::actor::send_closure_later(Self, &TestNode::got_dispatch_queue_messages, F.move_as_ok());
  });
}

void TestNode::got_dispatch_queue_messages(ton::tl_object_ptr<ton::lite_api::liteServer_dispatchQueueMessages> msgs) {
  td::TerminalIO::out() << "Dispatch queue messages (" << msgs->messages_.size() << "):\n";
  int count = 0;
  for (auto& m : msgs->messages_) {
    auto& meta = m->metadata_;
    td::TerminalIO::out() << "Msg #" << ++count << ": " << msgs->id_->workchain_ << ":" << m->addr_.to_hex() << " "
                          << m->lt_ << " : "
                          << (meta->initiator_->workchain_ == ton::workchainInvalid
                                  ? "[ no metadata ]"
                                  : block::MsgMetadata{(td::uint32)meta->depth_, meta->initiator_->workchain_,
                                                       meta->initiator_->id_, (ton::LogicalTime)meta->initiator_lt_}
                                        .to_str())
                          << "\n";
  }
  if (!msgs->complete_) {
    td::TerminalIO::out() << "(incomplete list)\n";
  }
}

bool TestNode::dns_resolve_start(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt blkid,
                                 std::string domain, td::Bits256 cat, int mode) {
  if (domain.size() >= 2 && domain[0] == '"' && domain.back() == '"') {
    domain.erase(0, 1);
    domain.pop_back();
  }
  std::vector<std::string> components;
  if (domain != ".") {
    std::size_t i, p = 0;
    for (i = 0; i < domain.size(); i++) {
      if (!domain[i] || (unsigned char)domain[i] >= 0xfe || (unsigned char)domain[i] <= ' ') {
        return set_error("invalid characters in a domain name");
      }
      if (domain[i] == '.') {
        if (i == p) {
          return set_error("domain name cannot have an empty component");
        }
        components.emplace_back(domain, p, i - p);
        p = i + 1;
      }
    }
    if (i > p) {
      components.emplace_back(domain, p, i - p);
    }
  }
  std::string qdomain;
  if (mode & 2) {
    qdomain += '\0';
  }
  while (!components.empty()) {
    qdomain += components.back();
    qdomain += '\0';
    components.pop_back();
  }
  if (qdomain.size() > 127) {
    return set_error("domain name too long");
  }

  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }

  if (workchain == ton::workchainInvalid) {
    if (dns_root_queried_) {
      workchain = ton::masterchainId;
      addr = dns_root_;
    } else {
      auto P =
          td::PromiseCreator::lambda([this, blkid, domain, cat, mode](td::Result<std::unique_ptr<block::Config>> R) {
            if (R.is_error()) {
              LOG(ERROR) << "cannot obtain root dns address from configuration: " << R.move_as_error();
            } else if (dns_root_queried_) {
              dns_resolve_start(ton::masterchainId, dns_root_, blkid, domain, cat, mode);
            } else {
              LOG(ERROR) << "cannot obtain root dns address from configuration parameter #4";
            }
          });
      return get_config_params(mc_last_id_, std::move(P), 0x3000, "", {4});
    }
  }
  return dns_resolve_send(workchain, addr, blkid, domain, qdomain, cat, mode);
}

bool TestNode::dns_resolve_send(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt blkid,
                                std::string domain, std::string qdomain, td::Bits256 cat, int mode) {
  LOG(INFO) << "dns_resolve for '" << domain << "' category=" << cat << " mode=" << mode
            << " starting from smart contract " << workchain << ":" << addr.to_hex() << " with respect to block "
            << blkid.to_str();
  vm::CellBuilder cb;
  Ref<vm::Cell> cell;
  if (!(cb.store_bytes_bool(td::Slice(qdomain)) && cb.finalize_to(cell))) {
    return set_error("cannot store domain name into slice");
  }
  std::vector<vm::StackEntry> params;
  params.emplace_back(vm::load_cell_slice_ref(cell));
  params.emplace_back(td::bits_to_refint(cat.cbits(), 256, false));
  auto P = td::PromiseCreator::lambda([this, workchain, addr, blkid, domain, qdomain, cat,
                                       mode](td::Result<std::vector<vm::StackEntry>> R) {
    if (R.is_error()) {
      LOG(ERROR) << R.move_as_error();
      return;
    }
    auto S = R.move_as_ok();
    if (S.size() < 2 || !S[S.size() - 2].is_int() || !(S.back().is_cell() || S.back().is_null())) {
      LOG(ERROR) << "dnsresolve did not return a value of type (int,cell)";
      return;
    }
    auto cell = S.back().as_cell();
    S.pop_back();
    auto x = S.back().as_int();
    S.clear();
    if (!x->signed_fits_bits(32)) {
      LOG(ERROR) << "invalid integer result of dnsresolve (" << x << ")";
      return;
    }
    return dns_resolve_finish(workchain, addr, blkid, domain, qdomain, cat, mode, (int)x->to_long(), std::move(cell));
  });
  return start_run_method(workchain, addr, blkid, "dnsresolve", std::move(params), 0x17, std::move(P));
}

bool TestNode::show_dns_record(std::ostream& os, td::Bits256 cat, Ref<vm::CellSlice> value, bool raw_dump) {
  if (raw_dump) {
    bool ok = show_dns_record(os, cat, value, false);
    if (!ok) {
      os << "cannot parse dns record; raw value: ";
      value->print_rec(print_limit_, os);
    }
    return ok;
  }
  if (value.is_null()) {
    os << "(null)";
    return true;
  }
  // block::gen::t_DNSRecord.print_ref(print_limit_, os, value);
  if (!block::gen::t_DNSRecord.validate_csr(value)) {
    return false;
  }
  block::gen::t_DNSRecord.print(os, value, 0, print_limit_);
  auto cs = *value;
  auto tag = block::gen::t_DNSRecord.get_tag(cs);
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  switch (tag) {
    case block::gen::DNSRecord::dns_adnl_address: {
      block::gen::DNSRecord::Record_dns_adnl_address rec;
      if (tlb::unpack_exact(cs, rec)) {
        os << "\n\tadnl address " << rec.adnl_addr.to_hex() << " = " << td::adnl_id_encode(rec.adnl_addr, true);
      }
      break;
    }
    case block::gen::DNSRecord::dns_smc_address: {
      block::gen::DNSRecord::Record_dns_smc_address rec;
      if (tlb::unpack_exact(cs, rec) && block::tlb::t_MsgAddressInt.extract_std_address(rec.smc_addr, wc, addr)) {
        os << "\tsmart contract " << wc << ":" << addr.to_hex() << " = "
           << block::StdAddress{wc, addr}.rserialize(true);
      }
      break;
    }
    case block::gen::DNSRecord::dns_storage_address: {
      block::gen::DNSRecord::Record_dns_storage_address rec;
      if (tlb::unpack_exact(cs, rec)) {
        os << "\tstorage address " << rec.bag_id.to_hex();
      }
      break;
    }
    case block::gen::DNSRecord::dns_next_resolver: {
      block::gen::DNSRecord::Record_dns_next_resolver rec;
      if (tlb::unpack_exact(cs, rec) && block::tlb::t_MsgAddressInt.extract_std_address(rec.resolver, wc, addr)) {
        os << "\tnext resolver " << wc << ":" << addr.to_hex() << " = " << block::StdAddress{wc, addr}.rserialize(true);
      }
      break;
    }
  }
  return true;
}

void TestNode::dns_resolve_finish(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::BlockIdExt blkid,
                                  std::string domain, std::string qdomain, td::Bits256 cat, int mode, int used_bits,
                                  Ref<vm::Cell> value) {
  if (used_bits <= 0) {
    td::TerminalIO::out() << "domain '" << domain << "' not found" << std::endl;
    return;
  }
  if ((used_bits & 7) || (unsigned)used_bits > 8 * std::min<std::size_t>(qdomain.size(), 126)) {
    LOG(ERROR) << "too many bits used (" << used_bits << " out of " << qdomain.size() * 8 << ")";
    return;
  }
  size_t pos = used_bits >> 3;
  bool end = pos == qdomain.size();
  if (!end && qdomain[pos - 1] && qdomain[pos]) {
    LOG(ERROR) << "domain split not at a component boundary";
    return;
  }
  if (!end) {
    LOG(INFO) << "partial information obtained";
    if (value.is_null()) {
      td::TerminalIO::out() << "domain '" << domain << "' not found: no next resolver" << std::endl;
      return;
    }
    Ref<vm::CellSlice> nx_address;
    ton::WorkchainId nx_wc;
    ton::StdSmcAddress nx_addr;
    if (!(block::gen::t_DNSRecord.cell_unpack_dns_next_resolver(value, nx_address) &&
          block::tlb::t_MsgAddressInt.extract_std_address(std::move(nx_address), nx_wc, nx_addr))) {
      LOG(ERROR) << "cannot parse next resolver info for " << domain.substr(qdomain.size() - pos - 1);
      std::ostringstream out;
      vm::load_cell_slice(value).print_rec(print_limit_, out);
      td::TerminalIO::err() << out.str() << std::endl;
      return;
    }
    LOG(INFO) << "next resolver is " << nx_wc << ":" << nx_addr.to_hex();
    if ((mode & 1)) {
      return;  // no recursive resolving
    }
    if (!(dns_resolve_send(nx_wc, nx_addr, blkid, domain, qdomain.substr(pos), cat, mode))) {
      LOG(ERROR) << "cannot send next dns query";
      return;
    }
    LOG(INFO) << "recursive dns query to '" << domain.substr(qdomain.size() - pos - 1) << "' sent";
    return;
  }
  auto out = td::TerminalIO::out();
  if (cat.is_zero()) {
    out << "Result for domain '" << domain << "' (all categories)" << std::endl;
  } else {
    out << "Result for domain '" << domain << "' category " << cat << std::endl;
  }
  try {
    if (value.not_null()) {
      std::ostringstream os0;
      vm::load_cell_slice(value).print_rec(print_limit_, os0);
      out << "raw data: " << os0.str() << std::endl;
    }
    if (cat.is_zero()) {
      vm::Dictionary dict{value, 256};
      if (!dict.check_for_each([this, &out](Ref<vm::CellSlice> cs, td::ConstBitPtr key, int n) {
            CHECK(n == 256);
            td::Bits256 x{key};
            if (cs.is_null() || cs->size_ext() != 0x10000) {
              out << "category " << x << " : value is not a reference" << std::endl;
              return true;
            }
            cs = vm::load_cell_slice_ref(cs->prefetch_ref());
            std::ostringstream os;
            (void)show_dns_record(os, x, cs, true);
            out << "category " << x << " : " << os.str() << std::endl;
            return true;
          })) {
        out << "invalid dns record dictionary" << std::endl;
      }
    } else {
      std::ostringstream os;
      (void)show_dns_record(os, cat, value.is_null() ? Ref<vm::CellSlice>() : vm::load_cell_slice_ref(value), true);
      out << "category " << cat << " : " << os.str() << std::endl;
    }
  } catch (vm::VmError& err) {
    LOG(ERROR) << "vm error while traversing dns resolve result: " << err.get_msg();
  } catch (vm::VmVirtError& err) {
    LOG(ERROR) << "vm virtualization error while traversing dns resolve result: " << err.get_msg();
  }
}

bool TestNode::get_one_transaction(ton::BlockIdExt blkid, ton::WorkchainId workchain, ton::StdSmcAddress addr,
                                   ton::LogicalTime lt, bool dump) {
  if (!blkid.is_valid_full()) {
    return set_error("invalid block id");
  }
  if (!ton::shard_contains(blkid.shard_full(), ton::extract_addr_prefix(workchain, addr))) {
    return set_error("the shard of this block cannot contain this account");
  }
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(workchain, addr);
  auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getOneTransaction>(
                                        ton::create_tl_lite_block_id(blkid), std::move(a), lt),
                                    true);
  LOG(INFO) << "requesting transaction " << lt << " of " << workchain << ":" << addr.to_hex() << " from block "
            << blkid.to_str();
  return envelope_send_query(
      std::move(b), [Self = actor_id(this), workchain, addr, lt, blkid, dump](td::Result<td::BufferSlice> R) -> void {
        if (R.is_error()) {
          return;
        }
        auto F = ton::fetch_tl_object<ton::lite_api::liteServer_transactionInfo>(R.move_as_ok(), true);
        if (F.is_error()) {
          LOG(ERROR) << "cannot parse answer to liteServer.getOneTransaction";
        } else {
          auto f = F.move_as_ok();
          td::actor::send_closure_later(Self, &TestNode::got_one_transaction, blkid, ton::create_block_id(f->id_),
                                        std::move(f->proof_), std::move(f->transaction_), workchain, addr, lt, dump);
        }
      });
}

bool TestNode::get_last_transactions(ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::LogicalTime lt,
                                     ton::Bits256 hash, unsigned count, bool dump) {
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(workchain, addr);
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(count, std::move(a), lt, hash), true);
  LOG(INFO) << "requesting " << count << " last transactions from " << lt << ":" << hash.to_hex() << " of " << workchain
            << ":" << addr.to_hex();
  return envelope_send_query(
      std::move(b), [Self = actor_id(this), workchain, addr, lt, hash, count, dump](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          return;
        }
        auto F = ton::fetch_tl_object<ton::lite_api::liteServer_transactionList>(R.move_as_ok(), true);
        if (F.is_error()) {
          LOG(ERROR) << "cannot parse answer to liteServer.getTransactions";
        } else {
          auto f = F.move_as_ok();
          std::vector<ton::BlockIdExt> blkids;
          for (auto& id : f->ids_) {
            blkids.push_back(ton::create_block_id(std::move(id)));
          }
          td::actor::send_closure_later(Self, &TestNode::got_last_transactions, std::move(blkids),
                                        std::move(f->transactions_), workchain, addr, lt, hash, count, dump);
        }
      });
}

void TestNode::got_account_state(ton::BlockIdExt ref_blk, ton::BlockIdExt blk, ton::BlockIdExt shard_blk,
                                 td::BufferSlice shard_proof, td::BufferSlice proof, td::BufferSlice state,
                                 ton::WorkchainId workchain, ton::StdSmcAddress addr, std::string filename, int mode,
                                 bool prunned) {
  LOG(INFO) << "got " << (prunned ? "prunned " : "") << "account state for " << workchain << ":" << addr.to_hex()
            << " with respect to blocks " << blk.to_str()
            << (shard_blk == blk ? "" : std::string{" and "} + shard_blk.to_str());
  block::AccountState account_state;
  account_state.blk = blk;
  account_state.shard_blk = shard_blk;
  account_state.shard_proof = std::move(shard_proof);
  account_state.proof = std::move(proof);
  account_state.state = std::move(state);
  account_state.is_virtualized = prunned;
  auto r_info = account_state.validate(ref_blk, block::StdAddress(workchain, addr));
  if (r_info.is_error()) {
    LOG(ERROR) << r_info.error().message();
    return;
  }
  auto out = td::TerminalIO::out();
  auto info = r_info.move_as_ok();
  if (mode < 0) {
    if (info.root.not_null()) {
      out << "account state is ";
      std::ostringstream outp;
      block::gen::t_Account.print_ref(print_limit_, outp, info.root);
      vm::load_cell_slice(info.root).print_rec(print_limit_, outp);
      out << outp.str();
      out << "last transaction lt = " << info.last_trans_lt << " hash = " << info.last_trans_hash.to_hex() << std::endl;
      block::gen::Account::Record_account acc;
      block::gen::AccountStorage::Record store;
      block::CurrencyCollection balance;
      if (tlb::unpack_cell(info.root, acc) && tlb::csr_unpack(acc.storage, store) && balance.unpack(store.balance)) {
        out << "account balance is " << balance.to_str() << std::endl;
      }
    } else {
      out << "account state is empty" << std::endl;
    }
  } else if (info.root.not_null()) {
    block::gen::Account::Record_account acc;
    block::gen::AccountStorage::Record store;
    block::CurrencyCollection balance;
    if (!(tlb::unpack_cell(info.root, acc) && tlb::csr_unpack(acc.storage, store) && balance.unpack(store.balance))) {
      LOG(ERROR) << "error unpacking account state";
      return;
    }
    out << "account balance is " << balance.to_str() << std::endl;
    int tag = block::gen::t_AccountState.get_tag(*store.state);
    switch (tag) {
      case block::gen::AccountState::account_uninit:
        out << "account not initialized (no StateInit to save into file)" << std::endl;
        return;
      case block::gen::AccountState::account_frozen:
        out << "account frozen (no StateInit to save into file)" << std::endl;
        return;
    }
    CHECK(store.state.write().fetch_ulong(1) == 1);  // account_init$1 _:StateInit = AccountState;
    block::gen::StateInit::Record state;
    CHECK(tlb::csr_unpack(store.state, state));
    Ref<vm::Cell> cell;
    const char* name = "<unknown-information>";
    if (mode == 0) {
      // save all state
      vm::CellBuilder cb;
      CHECK(cb.append_cellslice_bool(store.state) && cb.finalize_to(cell));
      name = "StateInit";
    } else if (mode == 1) {
      // save code
      cell = state.code->prefetch_ref();
      name = "code";
    } else if (mode == 2) {
      // save data
      cell = state.data->prefetch_ref();
      name = "data";
    }
    if (cell.is_null()) {
      out << "no " << name << " to save to file" << std::endl;
      return;
    }
    auto res = vm::std_boc_serialize(std::move(cell), 2);
    if (res.is_error()) {
      LOG(ERROR) << "cannot serialize extracted information from account state : " << res.move_as_error();
      return;
    }
    auto len = res.ok().size();
    auto res1 = td::write_file(filename, res.move_as_ok());
    if (res1.is_error()) {
      LOG(ERROR) << "cannot write " << name << " of account " << workchain << ":" << addr.to_hex() << " to file `"
                 << filename << "` : " << res1.move_as_error();
      return;
    }
    out << "written " << name << " of account " << workchain << ":" << addr.to_hex() << " to file `" << filename
        << "` (" << len << " bytes)" << std::endl;
  } else {
    out << "account state is empty (nothing saved to file `" << filename << "`)" << std::endl;
  }
}

void TestNode::run_smc_method(int mode, ton::BlockIdExt ref_blk, ton::BlockIdExt blk, ton::BlockIdExt shard_blk,
                              td::BufferSlice shard_proof, td::BufferSlice proof, td::BufferSlice state,
                              ton::WorkchainId workchain, ton::StdSmcAddress addr, std::string method,
                              std::vector<vm::StackEntry> params, td::BufferSlice remote_c7,
                              td::BufferSlice remote_libs, td::BufferSlice remote_result, int remote_exit_code,
                              td::Promise<std::vector<vm::StackEntry>> promise) {
  LOG(INFO) << "got (partial) account state (" << state.size() << " bytes) with mode=" << mode << " for " << workchain
            << ":" << addr.to_hex() << " with respect to blocks " << blk.to_str()
            << (shard_blk == blk ? "" : std::string{" and "} + shard_blk.to_str());
  auto out = td::TerminalIO::out();
  try {
    block::AccountState account_state;
    account_state.blk = blk;
    account_state.shard_blk = shard_blk;
    account_state.shard_proof = std::move(shard_proof);
    account_state.proof = std::move(proof);
    LOG(DEBUG) << "serialized state is " << state.size() << " bytes";
    LOG(DEBUG) << "serialized remote c7 is " << remote_c7.size() << " bytes";
    account_state.state = std::move(state);
    account_state.is_virtualized = (mode > 0);
    auto r_info = account_state.validate(ref_blk, block::StdAddress(workchain, addr));
    if (r_info.is_error()) {
      LOG(ERROR) << r_info.error().message();
      promise.set_error(r_info.move_as_error());
      return;
    }
    auto out = td::TerminalIO::out();
    auto info = r_info.move_as_ok();
    if (info.root.is_null()) {
      LOG(ERROR) << "account state of " << workchain << ":" << addr.to_hex() << " is empty (cannot run method `"
                 << method << "`)";
      promise.set_error(td::Status::Error(PSLICE() << "account state of " << workchain << ":" << addr.to_hex()
                                                   << " is empty (cannot run method `" << method << "`)"));
      return;
    }
    if (false) {
      // DEBUG (dump state)
      std::ostringstream os;
      vm::CellSlice{vm::NoVm(), info.true_root}.print_rec(print_limit_, os);
      out << "dump of account state (proof): " << os.str() << std::endl;
    }
    // set deserialization limits
    vm::FakeVmStateLimits fstate(1000);  // limit recursive (de)serialization calls
    vm::VmStateInterface::Guard guard(&fstate);
    if (false && remote_c7.size()) {
      // DEBUG (dump remote_c7)
      auto r_c7 = vm::std_boc_deserialize(remote_c7).move_as_ok();
      std::ostringstream os;
      vm::StackEntry val;
      bool ok = val.deserialize(r_c7);
      val.dump(os);
      // os << std::endl;
      // block::gen::t_VmStackValue.print_ref(print_limit_, os, r_c7);
      // os << std::endl;
      // vm::CellSlice{vm::NoVmOrd(), r_c7}.print_rec(print_limit_, os);
      out << "remote_c7 (deserialized=" << ok << "): " << os.str() << std::endl;
    }
    block::gen::Account::Record_account acc;
    block::gen::AccountStorage::Record store;
    block::CurrencyCollection balance;
    if (!(tlb::unpack_cell(info.root, acc) && tlb::csr_unpack(acc.storage, store) &&
          balance.validate_unpack(store.balance))) {
      LOG(ERROR) << "error unpacking account state";
      promise.set_error(td::Status::Error("error unpacking account state"));
      return;
    }
    int tag = block::gen::t_AccountState.get_tag(*store.state);
    switch (tag) {
      case block::gen::AccountState::account_uninit:
        LOG(ERROR) << "account " << workchain << ":" << addr.to_hex()
                   << " not initialized yet (cannot run any methods)";
        promise.set_error(td::Status::Error(PSLICE() << "account " << workchain << ":" << addr.to_hex()
                                                     << " not initialized yet (cannot run any methods)"));
        return;
      case block::gen::AccountState::account_frozen:
        LOG(ERROR) << "account " << workchain << ":" << addr.to_hex() << " frozen (cannot run any methods)";
        promise.set_error(td::Status::Error(PSLICE() << "account " << workchain << ":" << addr.to_hex()
                                                     << " frozen (cannot run any methods)"));
        return;
    }
    CHECK(store.state.write().fetch_ulong(1) == 1);  // account_init$1 _:StateInit = AccountState;
    block::gen::StateInit::Record state_init;
    CHECK(tlb::csr_unpack(store.state, state_init));
    auto code = state_init.code->prefetch_ref();
    auto data = state_init.data->prefetch_ref();
    auto stack = td::make_ref<vm::Stack>(std::move(params));
    td::int64 method_id = compute_method_id(method);
    stack.write().push_smallint(method_id);
    {
      std::ostringstream os;
      os << "arguments: ";
      stack->dump(os, 3);
      out << os.str();
    }
    long long gas_limit = /* vm::GasLimits::infty */ 10000000;
    // OstreamLogger ostream_logger(ctx.error_stream);
    // auto log = create_vm_log(ctx.error_stream ? &ostream_logger : nullptr);
    vm::GasLimits gas{gas_limit};
    LOG(DEBUG) << "creating VM";
    vm::VmState vm{code, std::move(stack), gas, 1, data, vm::VmLog()};
    vm.set_c7(liteclient::prepare_vm_c7(info.gen_utime, info.gen_lt, td::make_ref<vm::CellSlice>(acc.addr->clone()),
                                        balance));  // tuple with SmartContractInfo
    // vm.incr_stack_trace(1);    // enable stack dump after each step
    LOG(INFO) << "starting VM to run method `" << method << "` (" << method_id << ") of smart contract " << workchain
              << ":" << addr.to_hex();
    int exit_code;
    try {
      exit_code = ~vm.run();
    } catch (vm::VmVirtError& err) {
      LOG(ERROR) << "virtualization error while running VM to locally compute runSmcMethod result: " << err.get_msg();
      promise.set_error(
          td::Status::Error(PSLICE() << "virtualization error while running VM to locally compute runSmcMethod result: "
                                     << err.get_msg()));
      exit_code = -1001;
    } catch (vm::VmError& err) {
      LOG(ERROR) << "error while running VM to locally compute runSmcMethod result: " << err.get_msg();
      promise.set_error(td::Status::Error(PSLICE() << "error while running VM to locally compute runSmcMethod result: "
                                                   << err.get_msg()));
      exit_code = -1000;
    }
    LOG(DEBUG) << "VM terminated with exit code " << exit_code;
    if (mode > 0) {
      LOG(DEBUG) << "remote VM exit code is " << remote_exit_code;
      if (remote_exit_code == ~(int)vm::Excno::out_of_gas) {
        LOG(WARNING) << "remote server ran out of gas while performing this request; consider using runmethodfull";
      }
    }
    if (exit_code != 0) {
      out << "result: error " << exit_code << std::endl;
    } else {
      stack = vm.get_stack_ref();
      std::ostringstream os;
      os << "result: ";
      stack->dump(os, 3);
      out << os.str();
    }
    if (!(mode & 4)) {
      if (exit_code != 0) {
        LOG(ERROR) << "VM terminated with error code " << exit_code;
        promise.set_error(td::Status::Error(PSLICE() << "VM terminated with non-zero exit code " << exit_code));
      } else {
        promise.set_result(stack->extract_contents());
      }
    } else {
      if (remote_exit_code != 0) {
        out << "remote result: error " << remote_exit_code << std::endl;
        LOG(ERROR) << "VM terminated with error code " << exit_code;
        promise.set_error(td::Status::Error(PSLICE() << "VM terminated with non-zero exit code " << exit_code));
      } else if (remote_result.empty()) {
        out << "remote result: <none>" << std::endl;
        promise.set_value({});
      } else {
        auto res = vm::std_boc_deserialize(std::move(remote_result));
        if (res.is_error()) {
          auto err = res.move_as_error();
          LOG(ERROR) << "cannot deserialize remote VM result boc: " << err;
          promise.set_error(
              td::Status::Error(PSLICE() << "cannot deserialize remote VM result boc: " << std::move(err)));
          return;
        }
        auto cs = vm::load_cell_slice(res.move_as_ok());
        Ref<vm::Stack> remote_stack;
        if (!(vm::Stack::deserialize_to(cs, remote_stack, 0) && cs.empty_ext())) {
          LOG(ERROR) << "remote VM result boc cannot be deserialized as a VmStack";
          promise.set_error(td::Status::Error("remote VM result boc cannot be deserialized as a VmStack"));
          return;
        }
        std::ostringstream os;
        os << "remote result (not to be trusted): ";
        remote_stack->dump(os, 3);
        out << os.str();
        promise.set_value(remote_stack->extract_contents());
      }
    }
    out.flush();
  } catch (vm::VmVirtError& err) {
    out << "virtualization error while parsing runSmcMethod result: " << err.get_msg();
    promise.set_error(
        td::Status::Error(PSLICE() << "virtualization error while parsing runSmcMethod result: " << err.get_msg()));
  } catch (vm::VmError& err) {
    out << "error while parsing runSmcMethod result: " << err.get_msg();
    promise.set_error(td::Status::Error(PSLICE() << "error while parsing runSmcMethod result: " << err.get_msg()));
  }
}

void TestNode::got_one_transaction(ton::BlockIdExt req_blkid, ton::BlockIdExt blkid, td::BufferSlice proof,
                                   td::BufferSlice transaction, ton::WorkchainId workchain, ton::StdSmcAddress addr,
                                   ton::LogicalTime trans_lt, bool dump) {
  LOG(INFO) << "got transaction " << trans_lt << " for " << workchain << ":" << addr.to_hex()
            << " with respect to block " << blkid.to_str();
  if (blkid != req_blkid) {
    LOG(ERROR) << "obtained TransactionInfo for a different block " << blkid.to_str() << " instead of requested "
               << req_blkid.to_str();
    return;
  }
  if (!ton::shard_contains(blkid.shard_full(), ton::extract_addr_prefix(workchain, addr))) {
    LOG(ERROR) << "received data from block " << blkid.to_str() << " that cannot contain requested account "
               << workchain << ":" << addr.to_hex();
    return;
  }
  Ref<vm::Cell> root;
  if (!transaction.empty()) {
    auto R = vm::std_boc_deserialize(std::move(transaction));
    if (R.is_error()) {
      LOG(ERROR) << "cannot deserialize transaction";
      return;
    }
    root = R.move_as_ok();
    CHECK(root.not_null());
  }
  auto P = vm::std_boc_deserialize(std::move(proof));
  if (P.is_error()) {
    LOG(ERROR) << "cannot deserialize block transaction proof";
    return;
  }
  auto proof_root = P.move_as_ok();
  try {
    auto block_root = vm::MerkleProof::virtualize(std::move(proof_root), 1);
    if (block_root.is_null()) {
      LOG(ERROR) << "transaction block proof is invalid";
      return;
    }
    auto res1 = block::check_block_header_proof(block_root, blkid);
    if (res1.is_error()) {
      LOG(ERROR) << "error in transaction block header proof : " << res1.move_as_error().to_string();
      return;
    }
    auto trans_root_res = block::get_block_transaction_try(std::move(block_root), workchain, addr, trans_lt);
    if (trans_root_res.is_error()) {
      LOG(ERROR) << trans_root_res.move_as_error().message();
      return;
    }
    auto trans_root = trans_root_res.move_as_ok();
    if (trans_root.is_null() && root.not_null()) {
      LOG(ERROR) << "error checking transaction proof: proof claims there is no such transaction, but we have got "
                    "transaction data with hash "
                 << root->get_hash().bits().to_hex(256);
      return;
    }
    if (trans_root.not_null() && root.is_null()) {
      LOG(ERROR) << "error checking transaction proof: proof claims there is such a transaction with hash "
                 << trans_root->get_hash().bits().to_hex(256)
                 << ", but we have got no "
                    "transaction data";
      return;
    }
    if (trans_root.not_null() && trans_root->get_hash().bits().compare(root->get_hash().bits(), 256)) {
      LOG(ERROR) << "transaction hash mismatch: Merkle proof expects " << trans_root->get_hash().bits().to_hex(256)
                 << " but received data has " << root->get_hash().bits().to_hex(256);
      return;
    }
  } catch (vm::VmError err) {
    LOG(ERROR) << "error while traversing block transaction proof : " << err.get_msg();
    return;
  } catch (vm::VmVirtError err) {
    LOG(ERROR) << "virtualization error while traversing block transaction proof : " << err.get_msg();
    return;
  }
  auto out = td::TerminalIO::out();
  if (root.is_null()) {
    out << "transaction not found" << std::endl;
  } else {
    out << "transaction is ";
    std::ostringstream outp;
    block::gen::t_Transaction.print_ref(print_limit_, outp, root, 0);
    vm::load_cell_slice(root).print_rec(print_limit_, outp);
    out << outp.str();
  }
}

bool unpack_addr(std::ostream& os, Ref<vm::CellSlice> csr) {
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(std::move(csr), wc, addr)) {
    os << "<cannot unpack address>";
    return false;
  }
  os << wc << ":" << addr.to_hex();
  return true;
}

bool unpack_message(std::ostream& os, Ref<vm::Cell> msg, int mode) {
  if (msg.is_null()) {
    os << "<message not found>";
    return true;
  }
  vm::CellSlice cs{vm::NoVmOrd(), msg};
  switch (block::gen::t_CommonMsgInfo.get_tag(cs)) {
    case block::gen::CommonMsgInfo::ext_in_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(cs, info)) {
        LOG(DEBUG) << "cannot unpack inbound external message";
        return false;
      }
      os << "EXT-IN-MSG";
      if (!(mode & 2)) {
        os << " TO: ";
        if (!unpack_addr(os, std::move(info.dest))) {
          return false;
        }
      }
      return true;
    }
    case block::gen::CommonMsgInfo::ext_out_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info;
      if (!tlb::unpack(cs, info)) {
        LOG(DEBUG) << "cannot unpack outbound external message";
        return false;
      }
      os << "EXT-OUT-MSG";
      if (!(mode & 1)) {
        os << " FROM: ";
        if (!unpack_addr(os, std::move(info.src))) {
          return false;
        }
      }
      os << " LT:" << info.created_lt << " UTIME:" << info.created_at;
      return true;
    }
    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
        LOG(DEBUG) << "cannot unpack internal message";
        return false;
      }
      os << "INT-MSG";
      if (!(mode & 1)) {
        os << " FROM: ";
        if (!unpack_addr(os, std::move(info.src))) {
          return false;
        }
      }
      if (!(mode & 2)) {
        os << " TO: ";
        if (!unpack_addr(os, std::move(info.dest))) {
          return false;
        }
      }
      os << " LT:" << info.created_lt << " UTIME:" << info.created_at;
      td::RefInt256 value;
      Ref<vm::Cell> extra;
      if (!block::unpack_CurrencyCollection(info.value, value, extra)) {
        LOG(ERROR) << "cannot unpack message value";
        return false;
      }
      os << " VALUE:" << value;
      if (extra.not_null()) {
        os << "+extra";
      }
      return true;
    }
    default:
      LOG(ERROR) << "cannot unpack message";
      return false;
  }
}

std::string message_info_str(Ref<vm::Cell> msg, int mode) {
  std::ostringstream os;
  if (!unpack_message(os, msg, mode)) {
    return "<cannot unpack message>";
  } else {
    return os.str();
  }
}

void TestNode::got_last_transactions(std::vector<ton::BlockIdExt> blkids, td::BufferSlice transactions_boc,
                                     ton::WorkchainId workchain, ton::StdSmcAddress addr, ton::LogicalTime lt,
                                     ton::Bits256 hash, unsigned count, bool dump) {
  LOG(INFO) << "got up to " << count << " transactions for " << workchain << ":" << addr.to_hex()
            << " from last transaction " << lt << ":" << hash.to_hex();
  block::TransactionList transaction_list;
  transaction_list.blkids = blkids;
  transaction_list.lt = lt;
  transaction_list.hash = hash;
  transaction_list.transactions_boc = std::move(transactions_boc);
  auto r_account_state_info = transaction_list.validate();
  if (r_account_state_info.is_error()) {
    LOG(ERROR) << "got_last_transactions: " << r_account_state_info.error();
    return;
  }
  auto account_state_info = r_account_state_info.move_as_ok();
  unsigned c = 0;
  auto out = td::TerminalIO::out();
  CHECK(!account_state_info.transactions.empty());
  for (auto& info : account_state_info.transactions) {
    const auto& blkid = info.blkid;
    out << "transaction #" << c << " from block " << blkid.to_str() << (dump ? " is " : "\n");
    if (dump) {
      std::ostringstream outp;
      block::gen::t_Transaction.print_ref(print_limit_, outp, info.transaction);
      vm::load_cell_slice(info.transaction).print_rec(print_limit_, outp);
      out << outp.str();
    }
    block::gen::Transaction::Record trans;
    if (!tlb::unpack_cell(info.transaction, trans)) {
      LOG(ERROR) << "cannot unpack transaction #" << c;
      return;
    }
    out << "  time=" << trans.now << " outmsg_cnt=" << trans.outmsg_cnt << std::endl;
    auto in_msg = trans.r1.in_msg->prefetch_ref();
    if (in_msg.is_null()) {
      out << "  (no inbound message)" << std::endl;
    } else {
      out << "  inbound message: " << message_info_str(in_msg, 2 * 0) << std::endl;
      if (dump) {
        out << "    " << block::gen::t_Message_Any.as_string_ref(in_msg, 4);  // indentation = 4 spaces
      }
    }
    vm::Dictionary dict{trans.r1.out_msgs, 15};
    for (int x = 0; x < trans.outmsg_cnt && x < 100; x++) {
      auto out_msg = dict.lookup_ref(td::BitArray<15>{x});
      out << "  outbound message #" << x << ": " << message_info_str(out_msg, 1 * 0) << std::endl;
      if (dump) {
        out << "    " << block::gen::t_Message_Any.as_string_ref(out_msg, 4);
      }
    }
    register_blkid(blkid);  // unsafe?
  }
  auto& last = account_state_info.transactions.back();
  if (last.prev_trans_lt > 0) {
    out << "previous transaction has lt " << last.prev_trans_lt << " hash " << last.prev_trans_hash.to_hex()
        << std::endl;
    if (account_state_info.transactions.size() < count) {
      LOG(WARNING) << "obtained less transactions than required";
    }
  } else {
    out << "no preceding transactions (list complete)" << std::endl;
  }
}

bool TestNode::get_block_transactions(ton::BlockIdExt blkid, int mode, unsigned count, ton::Bits256 acc_addr,
                                      ton::LogicalTime lt) {
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  auto a = ton::create_tl_object<ton::lite_api::liteServer_transactionId3>(acc_addr, lt);
  auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
                                        ton::create_tl_lite_block_id(blkid), mode, count, std::move(a), false, false),
                                    true);
  LOG(INFO) << "requesting " << count << " transactions from block " << blkid.to_str() << " starting from account "
            << acc_addr.to_hex() << " lt " << lt;
  return envelope_send_query(std::move(b), [Self = actor_id(this), mode](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactions>(R.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.listBlockTransactions";
    } else {
      auto f = F.move_as_ok();
      std::vector<TransId> transactions;
      std::vector<ton::tl_object_ptr<ton::lite_api::liteServer_transactionMetadata>> metadata;
      for (auto& id : f->ids_) {
        transactions.emplace_back(id->account_, id->lt_, id->hash_);
        metadata.push_back(std::move(id->metadata_));
      }
      td::actor::send_closure_later(Self, &TestNode::got_block_transactions, ton::create_block_id(f->id_), mode,
                                    f->req_count_, f->incomplete_, std::move(transactions), std::move(metadata),
                                    std::move(f->proof_));
    }
  });
}

void TestNode::got_block_transactions(
    ton::BlockIdExt blkid, int mode, unsigned req_count, bool incomplete, std::vector<TestNode::TransId> trans,
    std::vector<ton::tl_object_ptr<ton::lite_api::liteServer_transactionMetadata>> metadata, td::BufferSlice proof) {
  LOG(INFO) << "got up to " << req_count << " transactions from block " << blkid.to_str();
  auto out = td::TerminalIO::out();
  int count = 0;
  for (size_t i = 0; i < trans.size(); ++i) {
    auto& t = trans[i];
    out << "transaction #" << ++count << ": account " << t.acc_addr.to_hex() << " lt " << t.trans_lt << " hash "
        << t.trans_hash.to_hex() << std::endl;
    if (mode & 256) {
      auto& meta = metadata.at(i);
      if (meta == nullptr) {
        out << "    metadata: <none>" << std::endl;
      } else {
        out << "    metadata: "
            << block::MsgMetadata{(td::uint32)meta->depth_, meta->initiator_->workchain_, meta->initiator_->id_,
                                  (ton::LogicalTime)meta->initiator_lt_}
                   .to_str()
            << std::endl;
      }
    }
  }
  out << (incomplete ? "(block transaction list incomplete)" : "(end of block transaction list)") << std::endl;
}

bool TestNode::get_all_shards(std::string filename, bool use_last, ton::BlockIdExt blkid) {
  if (use_last) {
    blkid = mc_last_id_;
  }
  if (!blkid.is_valid_full()) {
    return set_error(use_last ? "must obtain last block information before making other queries"
                              : "invalid masterchain block id");
  }
  if (!blkid.is_masterchain()) {
    return set_error("only masterchain blocks contain shard configuration");
  }
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(blkid)), true);
  LOG(INFO) << "requesting recent shard configuration";
  return envelope_send_query(std::move(b), [Self = actor_id(this), filename](td::Result<td::BufferSlice> R) -> void {
    if (R.is_error()) {
      return;
    }
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(R.move_as_ok(), true);
    if (F.is_error()) {
      LOG(ERROR) << "cannot parse answer to liteServer.getAllShardsInfo";
    } else {
      auto f = F.move_as_ok();
      td::actor::send_closure_later(Self, &TestNode::got_all_shards, ton::create_block_id(f->id_), std::move(f->proof_),
                                    std::move(f->data_), filename);
    }
  });
}

void TestNode::got_all_shards(ton::BlockIdExt blk, td::BufferSlice proof, td::BufferSlice data, std::string filename) {
  LOG(INFO) << "got shard configuration with respect to block " << blk.to_str();
  if (data.empty()) {
    td::TerminalIO::out() << "shard configuration is empty" << '\n';
  } else {
    auto R = vm::std_boc_deserialize(data.clone());
    if (R.is_error()) {
      LOG(ERROR) << "cannot deserialize shard configuration";
      return;
    }
    auto root = R.move_as_ok();
    auto out = td::TerminalIO::out();
    out << "shard configuration is ";
    std::ostringstream outp;
    block::gen::t_ShardHashes.print_ref(print_limit_, outp, root);
    vm::load_cell_slice(root).print_rec(print_limit_, outp);
    out << outp.str();
    block::ShardConfig sh_conf;
    if (!sh_conf.unpack(vm::load_cell_slice_ref(root))) {
      out << "cannot extract shard block list from shard configuration\n";
    } else {
      auto ids = sh_conf.get_shard_hash_ids(true);
      int cnt = 0;
      for (auto id : ids) {
        auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
        if (ref.not_null()) {
          register_blkid(ref->top_block_id());
          out << "shard #" << ++cnt << " : " << ref->top_block_id().to_str() << " @ " << ref->created_at() << " lt "
              << ref->start_lt() << " .. " << ref->end_lt() << std::endl;
        } else {
          out << "shard #" << ++cnt << " : " << id.to_str() << " (cannot unpack)\n";
        }
      }
    }
    if (!filename.empty()) {
      auto res1 = td::write_file(filename, data.as_slice());
      if (res1.is_error()) {
        LOG(ERROR) << "cannot write shard configuration to file `" << filename << "` : " << res1.move_as_error();
      } else {
        out << "saved shard configuration (ShardHashes) to file `" << filename << "` (" << data.size() << " bytes)"
            << std::endl;
      }
    }
  }
  show_new_blkids();
}

bool TestNode::parse_get_config_params(ton::BlockIdExt blkid, int mode, std::string filename, std::vector<int> params) {
  if (mode < 0) {
    mode = 0x80000;
  }
  if (!(mode & 0x81000) && !seekeoln()) {
    mode |= 0x1000;
    while (!seekeoln()) {
      int x;
      if (!convert_int32(get_word(), x)) {
        return set_error("integer configuration parameter id expected");
      }
      params.push_back(x);
    }
  }
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  if (!blkid.is_masterchain_ext()) {
    return set_error("only masterchain blocks contain configuration");
  }
  if (blkid == mc_last_id_) {
    mode |= 0x2000;
  }
  return get_config_params(blkid, trivial_promise_of<std::unique_ptr<block::Config>>(), mode, filename,
                           std::move(params));
}

bool TestNode::get_config_params(ton::BlockIdExt blkid, td::Promise<std::unique_ptr<block::Config>> promise, int mode,
                                 std::string filename, std::vector<int> params) {
  return get_config_params_ext(blkid, promise.wrap([](ConfigInfo&& info) { return std::move(info.config); }),
                               mode | 0x10000, filename, params);
}

bool TestNode::get_config_params_ext(ton::BlockIdExt blkid, td::Promise<ConfigInfo> promise, int mode,
                                     std::string filename, std::vector<int> params) {
  if (!(ready_ && !client_.empty())) {
    promise.set_error(td::Status::Error("server connection not ready"));
    return false;
  }
  if (!blkid.is_masterchain_ext()) {
    promise.set_error(td::Status::Error("masterchain reference block expected"));
    return false;
  }
  if (blkid == mc_last_id_) {
    mode |= 0x2000;
  }
  auto params_copy = params;
  auto b = (mode & 0x1000) ? ton::serialize_tl_object(
                                 ton::create_tl_object<ton::lite_api::liteServer_getConfigParams>(
                                     mode & 0x8fff, ton::create_tl_lite_block_id(blkid), std::move(params_copy)),
                                 true)
                           : ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getConfigAll>(
                                                          mode & 0x8fff, ton::create_tl_lite_block_id(blkid)),
                                                      true);
  LOG(INFO) << "requesting " << params.size() << " configuration parameters with respect to masterchain block "
            << blkid.to_str();
  return envelope_send_query(std::move(b), [Self = actor_id(this), mode, filename, blkid, params = std::move(params),
                                            promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
    td::actor::send_closure_later(Self, &TestNode::got_config_params, blkid, mode, filename, std::move(params),
                                  std::move(R), std::move(promise));
  });
}

void TestNode::got_config_params(ton::BlockIdExt req_blkid, int mode, std::string filename, std::vector<int> params,
                                 td::Result<td::BufferSlice> R, td::Promise<ConfigInfo> promise) {
  TRY_RESULT_PROMISE(promise, res, std::move(R));
  TRY_RESULT_PROMISE_PREFIX(promise, f,
                            ton::fetch_tl_object<ton::lite_api::liteServer_configInfo>(std::move(res), true),
                            "cannot parse answer to liteServer.getConfigParams");
  auto blkid = ton::create_block_id(f->id_);
  LOG(INFO) << "got configuration parameters";
  if (!blkid.is_masterchain_ext()) {
    promise.set_error(td::Status::Error("reference block "s + blkid.to_str() +
                                        " for the configuration is not a valid masterchain block"));
    return;
  }
  bool from_key = (mode & 0x8000);
  if (blkid.seqno() > req_blkid.seqno() || (!from_key && blkid != req_blkid)) {
    promise.set_error(td::Status::Error("got configuration parameters with respect to block "s + blkid.to_str() +
                                        " instead of " + req_blkid.to_str()));
    return;
  }
  try {
    Ref<vm::Cell> state, block, state_proof, config_proof;
    if (!(mode & 0x10000) && !from_key) {
      TRY_RESULT_PROMISE_PREFIX_ASSIGN(promise, state_proof, vm::std_boc_deserialize(f->state_proof_.as_slice()),
                                       "cannot deserialize state proof :");
    }
    if (!(mode & 0x10000) || from_key) {
      TRY_RESULT_PROMISE_PREFIX_ASSIGN(promise, config_proof, vm::std_boc_deserialize(f->config_proof_.as_slice()),
                                       "cannot deserialize config proof :");
    }
    if (!from_key) {
      TRY_RESULT_PROMISE_PREFIX_ASSIGN(
          promise, state,
          block::check_extract_state_proof(blkid, f->state_proof_.as_slice(), f->config_proof_.as_slice()),
          PSLICE() << "masterchain state proof for " << blkid.to_str() << " is invalid :");
    } else {
      block = vm::MerkleProof::virtualize(config_proof, 1);
      if (block.is_null()) {
        promise.set_error(
            td::Status::Error("cannot virtualize configuration proof constructed from key block "s + blkid.to_str()));
        return;
      }
      //TRY_STATUS_PROMISE_PREFIX(promise, block::check_block_header_proof(block, blkid),
      //                          PSLICE() << "incorrect header for key block " << blkid.to_str());
    }
    TRY_RESULT_PROMISE_PREFIX(promise, config,
                              from_key ? block::Config::extract_from_key_block(block, mode & 0xfff)
                                       : block::Config::extract_from_state(state, mode & 0xfff),
                              "cannot unpack configuration:");
    ConfigInfo cinfo{std::move(config), std::move(state_proof), std::move(config_proof)};
    if (mode & 0x80000) {
      TRY_RESULT_PROMISE_PREFIX(promise, boc, vm::std_boc_serialize(cinfo.config->get_root_cell(), 2),
                                "cannot serialize configuration:");
      auto size = boc.size();
      TRY_STATUS_PROMISE_PREFIX(promise, td::write_file(filename, std::move(boc)),
                                PSLICE() << "cannot save file `" << filename << "` :");
      td::TerminalIO::out() << "saved configuration dictionary into file `" << filename << "` (" << size
                            << " bytes written)" << std::endl;
      promise.set_result(std::move(cinfo));
      return;
    }
    if (mode & 0x4000) {
      promise.set_result(std::move(cinfo));
      return;
    }
    auto out = td::TerminalIO::out();
    if (mode & 0x1000) {
      for (int i : params) {
        out << "ConfigParam(" << i << ") = ";
        auto value = cinfo.config->get_config_param(i);
        if (value.is_null()) {
          out << "(null)\n";
        } else {
          std::ostringstream os;
          if (i >= 0) {
            block::gen::ConfigParam{i}.print_ref(print_limit_, os, value);
            os << std::endl;
          }
          vm::load_cell_slice(value).print_rec(print_limit_, os);
          out << os.str() << std::endl;
          if (mode & 0x2000) {
            register_config_param(i, value);
          }
        }
      }
    } else {
      cinfo.config->foreach_config_param([this, &out, mode](int i, Ref<vm::Cell> value) {
        out << "ConfigParam(" << i << ") = ";
        if (value.is_null()) {
          out << "(null)\n";
        } else {
          std::ostringstream os;
          if (i >= 0) {
            block::gen::ConfigParam{i}.print_ref(print_limit_, os, value);
            os << std::endl;
          }
          vm::load_cell_slice(value).print_rec(print_limit_, os);
          out << os.str() << std::endl;
          if (mode & 0x2000) {
            register_config_param(i, value);
          }
        }
        return true;
      });
    }
    promise.set_result(std::move(cinfo));
  } catch (vm::VmError& err) {
    promise.set_error(err.as_status("error while traversing configuration: "));
    return;
  } catch (vm::VmVirtError& err) {
    promise.set_error(err.as_status("virtualization error while traversing configuration: "));
    return;
  }
}

bool TestNode::register_config_param(int idx, Ref<vm::Cell> value) {
  switch (idx) {
    case 0:
      return register_config_param0(std::move(value));
    case 1:
      return register_config_param1(std::move(value));
    case 4:
      return register_config_param4(std::move(value));
    default:
      return true;
  }
}

bool TestNode::register_config_param4(Ref<vm::Cell> value) {
  if (value.is_null()) {
    return false;
  }
  vm::CellSlice cs{vm::NoVmOrd(), std::move(value)};
  ton::StdSmcAddress addr;
  if (cs.size_ext() == 256 && cs.fetch_bits_to(addr)) {
    dns_root_queried_ = true;
    if (dns_root_ != addr) {
      dns_root_ = addr;
      LOG(INFO) << "dns root set to -1:" << addr.to_hex();
    }
    return true;
  } else {
    return false;
  }
}

bool TestNode::register_config_param1(Ref<vm::Cell> value) {
  if (value.is_null()) {
    return false;
  }
  vm::CellSlice cs{vm::NoVmOrd(), std::move(value)};
  ton::StdSmcAddress addr;
  if (cs.size_ext() == 256 && cs.fetch_bits_to(addr)) {
    elect_addr_queried_ = true;
    if (elect_addr_ != addr) {
      elect_addr_ = addr;
      LOG(INFO) << "elector smart contract address set to -1:" << addr.to_hex();
    }
    return true;
  } else {
    return false;
  }
}

bool TestNode::register_config_param0(Ref<vm::Cell> value) {
  if (value.is_null()) {
    return false;
  }
  vm::CellSlice cs{vm::NoVmOrd(), std::move(value)};
  ton::StdSmcAddress addr;
  if (cs.size_ext() == 256 && cs.fetch_bits_to(addr)) {
    config_addr_queried_ = true;
    if (config_addr_ != addr) {
      config_addr_ = addr;
      LOG(INFO) << "configuration smart contract address set to -1:" << addr.to_hex();
    }
    return true;
  } else {
    return false;
  }
}

bool TestNode::get_block(ton::BlockIdExt blkid, bool dump) {
  LOG(INFO) << "got block download request for " << blkid.to_str();
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(blkid)), true);
  return envelope_send_query(
      std::move(b), [Self = actor_id(this), blkid, dump](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          LOG(ERROR) << "cannot obtain block " << blkid.to_str()
                     << " from server : " << res.move_as_error().to_string();
          return;
        } else {
          auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(res.move_as_ok(), true);
          if (F.is_error()) {
            LOG(ERROR) << "cannot parse answer to liteServer.getBlock : " << res.move_as_error().to_string();
          } else {
            auto f = F.move_as_ok();
            auto blk_id = ton::create_block_id(f->id_);
            LOG(INFO) << "obtained block " << blk_id.to_str() << " from server";
            if (blk_id != blkid) {
              LOG(ERROR) << "block id mismatch: expected data for block " << blkid.to_str() << ", obtained for "
                         << blk_id.to_str();
              return;
            }
            td::actor::send_closure_later(Self, &TestNode::got_block, blk_id, std::move(f->data_), dump);
          }
        }
      });
}

bool TestNode::get_state(ton::BlockIdExt blkid, bool dump) {
  LOG(INFO) << "got state download request for " << blkid.to_str();
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getState>(ton::create_tl_lite_block_id(blkid)), true);
  return envelope_send_query(
      std::move(b), [Self = actor_id(this), blkid, dump](td::Result<td::BufferSlice> res) -> void {
        if (res.is_error()) {
          LOG(ERROR) << "cannot obtain state " << blkid.to_str()
                     << " from server : " << res.move_as_error().to_string();
          return;
        } else {
          auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockState>(res.move_as_ok(), true);
          if (F.is_error()) {
            LOG(ERROR) << "cannot parse answer to liteServer.getState";
          } else {
            auto f = F.move_as_ok();
            auto blk_id = ton::create_block_id(f->id_);
            LOG(INFO) << "obtained state " << blk_id.to_str() << " from server";
            if (blk_id != blkid) {
              LOG(ERROR) << "block id mismatch: expected state for block " << blkid.to_str() << ", obtained for "
                         << blk_id.to_str();
              return;
            }
            td::actor::send_closure_later(Self, &TestNode::got_state, blk_id, f->root_hash_, f->file_hash_,
                                          std::move(f->data_), dump);
          }
        }
      });
}

void TestNode::got_block(ton::BlockIdExt blkid, td::BufferSlice data, bool dump) {
  LOG(INFO) << "obtained " << data.size() << " data bytes for block " << blkid.to_str();
  ton::FileHash fhash;
  td::sha256(data.as_slice(), fhash.as_slice());
  if (fhash != blkid.file_hash) {
    LOG(ERROR) << "file hash mismatch for block " << blkid.to_str() << ": expected " << blkid.file_hash.to_hex()
               << ", computed " << fhash.to_hex();
    return;
  }
  register_blkid(blkid);
  if (!db_root_.empty()) {
    auto res = save_db_file(fhash, data.clone());
    if (res.is_error()) {
      LOG(ERROR) << "error saving block file: " << res.to_string();
    }
  }
  if (dump) {
    auto res = vm::std_boc_deserialize(std::move(data));
    if (res.is_error()) {
      LOG(ERROR) << "cannot deserialize block data : " << res.move_as_error().to_string();
      return;
    }
    auto root = res.move_as_ok();
    ton::RootHash rhash{root->get_hash().bits()};
    if (rhash != blkid.root_hash) {
      LOG(ERROR) << "block root hash mismatch: data has " << rhash.to_hex() << " , expected "
                 << blkid.root_hash.to_hex();
      return;
    }
    auto out = td::TerminalIO::out();
    out << "block contents is ";
    std::ostringstream outp;
    block::gen::t_Block.print_ref(print_limit_, outp, root);
    vm::load_cell_slice(root).print_rec(print_limit_, outp);
    out << outp.str();
    show_block_header(blkid, std::move(root), 0xffff);
  } else {
    auto res = lazy_boc_deserialize(std::move(data));
    if (res.is_error()) {
      LOG(ERROR) << "cannot lazily deserialize block data : " << res.move_as_error().to_string();
      return;
    }
    auto pair = res.move_as_ok();
    auto root = std::move(pair.first);
    ton::RootHash rhash{root->get_hash().bits()};
    if (rhash != blkid.root_hash) {
      LOG(ERROR) << "block root hash mismatch: data has " << rhash.to_hex() << " , expected "
                 << blkid.root_hash.to_hex();
      return;
    }
    show_block_header(blkid, std::move(root), 0xffff);
  }
  show_new_blkids();
}

void TestNode::got_state(ton::BlockIdExt blkid, ton::RootHash root_hash, ton::FileHash file_hash, td::BufferSlice data,
                         bool dump) {
  LOG(INFO) << "obtained " << data.size() << " state bytes for block " << blkid.to_str();
  ton::FileHash fhash;
  td::sha256(data.as_slice(), fhash.as_slice());
  if (fhash != file_hash) {
    LOG(ERROR) << "file hash mismatch for state " << blkid.to_str() << ": expected " << file_hash.to_hex()
               << ", computed " << fhash.to_hex();
    return;
  }
  register_blkid(blkid);
  if (!db_root_.empty()) {
    auto res = save_db_file(fhash, data.clone());
    if (res.is_error()) {
      LOG(ERROR) << "error saving state file: " << res.to_string();
    }
  }
  if (dump) {
    auto res = vm::std_boc_deserialize(std::move(data));
    if (res.is_error()) {
      LOG(ERROR) << "cannot deserialize block data : " << res.move_as_error().to_string();
      return;
    }
    auto root = res.move_as_ok();
    ton::RootHash rhash{root->get_hash().bits()};
    if (rhash != root_hash) {
      LOG(ERROR) << "block state root hash mismatch: data has " << rhash.to_hex() << " , expected "
                 << root_hash.to_hex();
      return;
    }
    auto out = td::TerminalIO::out();
    out << "shard state contents is ";
    std::ostringstream outp;
    block::gen::t_ShardState.print_ref(print_limit_, outp, root);
    vm::load_cell_slice(root).print_rec(print_limit_, outp);
    out << outp.str();
    show_state_header(blkid, std::move(root), 0xffff);
  } else {
    auto res = lazy_boc_deserialize(std::move(data));
    if (res.is_error()) {
      LOG(ERROR) << "cannot lazily deserialize block data : " << res.move_as_error().to_string();
      return;
    }
    auto pair = res.move_as_ok();
    auto root = std::move(pair.first);
    ton::RootHash rhash{root->get_hash().bits()};
    if (rhash != root_hash) {
      LOG(ERROR) << "block state root hash mismatch: data has " << rhash.to_hex() << " , expected "
                 << root_hash.to_hex();
      return;
    }
    show_state_header(blkid, std::move(root), 0xffff);
  }
  show_new_blkids();
}

bool TestNode::get_show_block_header(ton::BlockIdExt blkid, int mode) {
  return get_block_header(blkid, mode, [this, blkid](td::Result<BlockHdrInfo> R) {
    if (R.is_error()) {
      LOG(ERROR) << "unable to fetch block header: " << R.move_as_error();
    } else {
      auto res = R.move_as_ok();
      show_block_header(res.blk_id, res.virt_blk_root, res.mode);
      show_new_blkids();
    }
  });
}

bool TestNode::get_block_header(ton::BlockIdExt blkid, int mode, td::Promise<TestNode::BlockHdrInfo> promise) {
  LOG(INFO) << "got block header request for " << blkid.to_str() << " with mode " << mode;
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlockHeader>(ton::create_tl_lite_block_id(blkid), mode), true);
  return envelope_send_query(
      std::move(b), [this, blkid, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable -> void {
        TRY_RESULT_PROMISE_PREFIX(promise, res, std::move(R),
                                  PSLICE() << "cannot obtain block header for " << blkid.to_str() << " from server :");
        got_block_header_raw(std::move(res), std::move(promise), blkid);
      });
}

bool TestNode::lookup_show_block(ton::ShardIdFull shard, int mode, td::uint64 arg) {
  return lookup_block(shard, mode, arg, [this](td::Result<BlockHdrInfo> R) {
    if (R.is_error()) {
      LOG(ERROR) << "unable to look up block: " << R.move_as_error();
    } else {
      auto res = R.move_as_ok();
      show_block_header(res.blk_id, res.virt_blk_root, res.mode);
      show_new_blkids();
    }
  });
}

bool TestNode::lookup_block(ton::ShardIdFull shard, int mode, td::uint64 arg,
                            td::Promise<TestNode::BlockHdrInfo> promise) {
  ton::BlockId id{shard, mode & 1 ? (td::uint32)arg : 0};
  LOG(INFO) << "got block lookup request for " << id.to_str() << " with mode " << mode << " and argument " << arg;
  auto b = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_lookupBlock>(
                                        mode, ton::create_tl_lite_block_id_simple(id), arg, (td::uint32)arg),
                                    true);
  return envelope_send_query(
      std::move(b), [this, id, mode, arg, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable -> void {
        TRY_RESULT_PROMISE_PREFIX(promise, res, std::move(R),
                                  PSLICE() << "cannot look up block header for " << id.to_str() << " with mode " << mode
                                           << " and argument " << arg << " from server :");
        got_block_header_raw(std::move(res), std::move(promise));
      });
}

void TestNode::got_block_header_raw(td::BufferSlice res, td::Promise<TestNode::BlockHdrInfo> promise,
                                    ton::BlockIdExt req_blkid) {
  TRY_RESULT_PROMISE_PREFIX(promise, f,
                            ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(res), true),
                            "cannot parse answer to liteServer.lookupBlock :");
  auto blk_id = ton::create_block_id(f->id_);
  LOG(INFO) << "obtained block header for " << blk_id.to_str() << " from server (" << f->header_proof_.size()
            << " data bytes)";
  if (req_blkid.is_valid() && blk_id != req_blkid) {
    promise.set_error(td::Status::Error(PSLICE() << "block id mismatch: expected data for block " << req_blkid.to_str()
                                                 << ", obtained for " << blk_id.to_str()));
    return;
  }
  TRY_RESULT_PROMISE_PREFIX(promise, root, vm::std_boc_deserialize(std::move(f->header_proof_)),
                            "cannot deserialize block header data :");
  bool ok = false;
  td::Status E;
  try {
    auto virt_root = vm::MerkleProof::virtualize(root, 1);
    if (virt_root.is_null()) {
      promise.set_error(td::Status::Error(PSLICE() << "block header proof for block " << blk_id.to_str()
                                                   << " is not a valid Merkle proof"));
      return;
    }
    ok = true;
    promise.set_result(BlockHdrInfo{blk_id, std::move(root), std::move(virt_root), f->mode_});
    return;
  } catch (vm::VmError& err) {
    E = err.as_status(PSLICE() << "error processing header for " << blk_id.to_str() << " :");
  } catch (vm::VmVirtError& err) {
    E = err.as_status(PSLICE() << "error processing header for " << blk_id.to_str() << " :");
  }
  if (ok) {
    LOG(ERROR) << std::move(E);
  } else {
    promise.set_error(std::move(E));
  }
}

bool TestNode::show_block_header(ton::BlockIdExt blkid, Ref<vm::Cell> root, int mode) {
  ton::RootHash vhash{root->get_hash().bits()};
  if (vhash != blkid.root_hash) {
    LOG(ERROR) << " block header for block " << blkid.to_str() << " has incorrect root hash " << vhash.to_hex()
               << " instead of " << blkid.root_hash.to_hex();
    return false;
  }
  std::vector<ton::BlockIdExt> prev;
  ton::BlockIdExt mc_blkid;
  bool after_split;
  auto res = block::unpack_block_prev_blk_ext(root, blkid, prev, mc_blkid, after_split);
  if (res.is_error()) {
    LOG(ERROR) << "cannot unpack header for block " << blkid.to_str() << " : " << res.to_string();
    return false;
  }
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  if (!(tlb::unpack_cell(root, blk) && tlb::unpack_cell(blk.info, info))) {
    LOG(ERROR) << "cannot unpack header for block " << blkid.to_str();
    return false;
  }
  auto out = td::TerminalIO::out();
  out << "block header of " << blkid.to_str() << " @ " << info.gen_utime << " lt " << info.start_lt << " .. "
      << info.end_lt << std::endl;
  out << "global_id=" << blk.global_id << " version=" << info.version << " not_master=" << info.not_master
      << " after_merge=" << info.after_merge << " after_split=" << info.after_split
      << " before_split=" << info.before_split << " want_merge=" << info.want_merge << " want_split=" << info.want_split
      << " validator_list_hash_short=" << info.gen_validator_list_hash_short
      << " catchain_seqno=" << info.gen_catchain_seqno << " min_ref_mc_seqno=" << info.min_ref_mc_seqno;
  if (!info.not_master) {
    out << " is_key_block=" << info.key_block << " prev_key_block_seqno=" << info.prev_key_block_seqno;
  }
  out << std::endl;
  register_blkid(blkid);
  int cnt = 0;
  for (auto id : prev) {
    out << "previous block #" << ++cnt << " : " << id.to_str() << std::endl;
    register_blkid(id);
  }
  out << "reference masterchain block : " << mc_blkid.to_str() << std::endl;
  register_blkid(mc_blkid);
  return true;
}

bool TestNode::show_state_header(ton::BlockIdExt blkid, Ref<vm::Cell> root, int mode) {
  return true;
}

void TestNode::got_block_header(ton::BlockIdExt blkid, td::BufferSlice data, int mode) {
  LOG(INFO) << "obtained " << data.size() << " data bytes as block header for " << blkid.to_str();
  auto res = vm::std_boc_deserialize(data.clone());
  if (res.is_error()) {
    LOG(ERROR) << "cannot deserialize block header data : " << res.move_as_error().to_string();
    return;
  }
  auto root = res.move_as_ok();
  std::ostringstream outp;
  vm::CellSlice cs{vm::NoVm(), root};
  cs.print_rec(print_limit_, outp);
  td::TerminalIO::out() << outp.str();
  try {
    auto virt_root = vm::MerkleProof::virtualize(root, 1);
    if (virt_root.is_null()) {
      LOG(ERROR) << " block header proof for block " << blkid.to_str() << " is not a valid Merkle proof";
      return;
    }
    show_block_header(blkid, std::move(virt_root), mode);
  } catch (vm::VmError err) {
    LOG(ERROR) << "error processing header for " << blkid.to_str() << " : " << err.get_msg();
  } catch (vm::VmVirtError err) {
    LOG(ERROR) << "error processing header for " << blkid.to_str() << " : " << err.get_msg();
  }
  show_new_blkids();
}

bool TestNode::get_block_proof(ton::BlockIdExt from, ton::BlockIdExt to, int mode) {
  if (!(mode & 1)) {
    to.invalidate_clear();
  }
  if (!(mode & 0x2000)) {
    LOG(INFO) << "got block proof request from " << from.to_str() << " to "
              << ((mode & 1) ? to.to_str() : "last masterchain block") << " with mode=" << mode;
  } else {
    LOG(DEBUG) << "got block proof request from " << from.to_str() << " to "
               << ((mode & 1) ? to.to_str() : "last masterchain block") << " with mode=" << mode;
  }
  if (!from.is_masterchain_ext()) {
    LOG(ERROR) << "source block " << from.to_str() << " is not a valid masterchain block id";
    return false;
  }
  if ((mode & 1) && !to.is_masterchain_ext()) {
    LOG(ERROR) << "destination block " << to.to_str() << " is not a valid masterchain block id";
    return false;
  }
  auto b =
      ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getBlockProof>(
                                   mode & 0xfff, ton::create_tl_lite_block_id(from), ton::create_tl_lite_block_id(to)),
                               true);
  return envelope_send_query(std::move(b), [Self = actor_id(this), from, to, mode](td::Result<td::BufferSlice> res) {
    if (res.is_error()) {
      LOG(ERROR) << "cannot obtain block proof for " << ((mode & 1) ? to.to_str() : "last masterchain block")
                 << " starting from " << from.to_str() << " from server : " << res.move_as_error().to_string();
    } else {
      td::actor::send_closure_later(Self, &TestNode::got_block_proof, from, to, mode, res.move_as_ok());
    }
  });
}

void TestNode::got_block_proof(ton::BlockIdExt from, ton::BlockIdExt to, int mode, td::BufferSlice pchain) {
  LOG(INFO) << "got block proof from " << from.to_str() << " to "
            << ((mode & 1) ? to.to_str() : "last masterchain block") << " with mode=" << mode << " (" << pchain.size()
            << " bytes)";
  auto r_f = ton::fetch_tl_object<ton::lite_api::liteServer_partialBlockProof>(std::move(pchain), true);
  if (r_f.is_error()) {
    LOG(ERROR) << "cannot deserialize liteServer.partialBlockProof: " << r_f.move_as_error();
    return;
  }
  auto f = r_f.move_as_ok();
  auto res = liteclient::deserialize_proof_chain(std::move(f));
  if (res.is_error()) {
    LOG(ERROR) << "cannot deserialize liteServer.partialBlockProof: " << res.move_as_error();
    return;
  }
  auto chain = res.move_as_ok();
  if (chain->from != from) {
    LOG(ERROR) << "block proof chain starts from block " << chain->from.to_str() << ", not from requested block "
               << from.to_str();
    return;
  }
  auto err = chain->validate();
  if (err.is_error()) {
    LOG(ERROR) << "block proof chain is invalid: " << err;
    return;
  }
  // TODO: if `from` was a trusted key block, then mark `to` as a trusted key block, and update the known value of latest trusted key block if `to` is newer
  if (!chain->complete && (mode & 0x1000)) {
    LOG(INFO) << "valid " << (chain->complete ? "" : "in") << "complete proof chain: last block is "
              << chain->to.to_str() << ", last key block is "
              << (chain->has_key_block ? chain->key_blkid.to_str() : "(undefined)");
    get_block_proof(chain->to, to, mode | 0x2000);
    return;
  }
  td::TerminalIO::out() << "valid " << (chain->complete ? "" : "in") << "complete proof chain: last block is "
                        << chain->to.to_str() << ", last key block is "
                        << (chain->has_key_block ? chain->key_blkid.to_str() : "(undefined)") << std::endl;
  if (chain->has_key_block) {
    register_blkid(chain->key_blkid);
  }
  register_blkid(chain->to);
  auto time = now();
  if (!(mode & 1) || (chain->last_utime > time - 3600)) {
    td::TerminalIO::out() << "last block in chain was generated at " << chain->last_utime << " ("
                          << time - chain->last_utime << " seconds ago)\n";
  }
  show_new_blkids();
}

bool TestNode::get_creator_stats(ton::BlockIdExt blkid, int mode, unsigned req_count, ton::Bits256 start_after,
                                 ton::UnixTime min_utime) {
  if (!(ready_ && !client_.empty())) {
    return set_error("server connection not ready");
  }
  if (!blkid.is_masterchain_ext()) {
    return set_error("only masterchain blocks contain block creator statistics");
  }
  if (!(mode & 1)) {
    start_after.set_zero();
  }
  auto osp = std::make_unique<std::ostringstream>();
  auto& os = *osp;
  return get_creator_stats(
      blkid, mode, req_count, start_after, min_utime,
      [min_utime, &os](const td::Bits256& key, const block::DiscountedCounter& mc_cnt,
                       const block::DiscountedCounter& shard_cnt) -> bool {
        os << key.to_hex() << " mc_cnt:" << mc_cnt << " shard_cnt:" << shard_cnt << std::endl;
        return true;
      },
      td::PromiseCreator::lambda([os = std::move(osp)](td::Result<td::Bits256> res) {
        if (res.is_error()) {
          LOG(ERROR) << "error obtaining creator stats: " << res.move_as_error();
        } else {
          if (res.ok().is_zero()) {
            *os << "(complete)" << std::endl;
          } else {
            *os << "(incomplete, repeat query from " << res.move_as_ok().to_hex() << " )" << std::endl;
          }
          td::TerminalIO::out() << os->str();
        }
      }));
}

bool TestNode::get_creator_stats(ton::BlockIdExt blkid, int mode, unsigned req_count, ton::Bits256 start_after,
                                 ton::UnixTime min_utime, TestNode::creator_stats_func_t func,
                                 td::Promise<td::Bits256> promise) {
  return get_creator_stats(blkid, req_count, min_utime, std::move(func),
                           std::make_unique<CreatorStatsRes>(mode | 0x10000, start_after),
                           promise.wrap([](auto&& p) { return p->last_key; }));
}

bool TestNode::get_creator_stats(ton::BlockIdExt blkid, unsigned req_count, ton::UnixTime min_utime,
                                 TestNode::creator_stats_func_t func, std::unique_ptr<TestNode::CreatorStatsRes> state,
                                 td::Promise<std::unique_ptr<TestNode::CreatorStatsRes>> promise) {
  if (!(ready_ && !client_.empty())) {
    promise.set_error(td::Status::Error("server connection not ready"));
    return false;
  }
  if (!state) {
    promise.set_error(td::Status::Error("null CreatorStatsRes"));
    return false;
  }
  if (!blkid.is_masterchain_ext()) {
    promise.set_error(td::Status::Error("only masterchain blocks contain block creator statistics"));
    return false;
  }
  if (!(state->mode & 1)) {
    state->last_key.set_zero();
  }
  auto b = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getValidatorStats>(
          state->mode & 0xff, ton::create_tl_lite_block_id(blkid), req_count, state->last_key, min_utime),
      true);
  LOG(INFO) << "requesting up to " << req_count << " block creator stats records with respect to masterchain block "
            << blkid.to_str() << " starting from validator public key " << state->last_key.to_hex() << " created after "
            << min_utime << " (mode=" << state->mode << ")";
  return envelope_send_query(
      std::move(b), [this, blkid, req_count, state = std::move(state), min_utime, func = std::move(func),
                     promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        TRY_RESULT_PROMISE(promise, res, std::move(R));
        TRY_RESULT_PROMISE_PREFIX(promise, f,
                                  ton::fetch_tl_object<ton::lite_api::liteServer_validatorStats>(std::move(res), true),
                                  "cannot parse answer to liteServer.getValidatorStats");
        got_creator_stats(blkid, ton::create_block_id(f->id_), f->mode_, min_utime, std::move(f->state_proof_),
                          std::move(f->data_proof_), f->count_, req_count, f->complete_, std::move(func),
                          std::move(state), std::move(promise));
      });
}

void TestNode::got_creator_stats(ton::BlockIdExt req_blkid, ton::BlockIdExt blkid, int mode, ton::UnixTime min_utime,
                                 td::BufferSlice state_proof, td::BufferSlice data_proof, int count, int req_count,
                                 bool complete, TestNode::creator_stats_func_t func,
                                 std::unique_ptr<TestNode::CreatorStatsRes> status,
                                 td::Promise<std::unique_ptr<TestNode::CreatorStatsRes>> promise) {
  LOG(INFO) << "got answer to getValidatorStats query: " << count << " records out of " << req_count << ", "
            << (complete ? "complete" : "incomplete");
  if (!blkid.is_masterchain_ext()) {
    promise.set_error(td::Status::Error(PSLICE() << "reference block " << blkid.to_str()
                                                 << " for block creator statistics is not a valid masterchain block"));
    return;
  }
  if (count > req_count) {
    promise.set_error(td::Status::Error(PSLICE()
                                        << "obtained " << count << " answers to getValidatorStats query, but only "
                                        << req_count << " were requested"));
    return;
  }
  if (blkid != req_blkid) {
    promise.set_error(td::Status::Error(PSLICE()
                                        << "answer to getValidatorStats refers to masterchain block " << blkid.to_str()
                                        << " different from requested " << req_blkid.to_str()));
    return;
  }
  TRY_RESULT_PROMISE_PREFIX(promise, state,
                            block::check_extract_state_proof(blkid, state_proof.as_slice(), data_proof.as_slice()),
                            PSLICE() << "masterchain state proof for " << blkid.to_str() << " is invalid :");
  if (!(mode & 0x10000)) {
    if (status->state_proof.is_null()) {
      TRY_RESULT_PROMISE_PREFIX(
          promise, state_root, vm::std_boc_deserialize(state_proof.as_slice()),
          PSLICE() << "cannot deserialize masterchain state proof for " << blkid.to_str() << ": ");
      status->state_proof = std::move(state_root);
    }
    TRY_RESULT_PROMISE_PREFIX(
        promise, data_root, vm::std_boc_deserialize(data_proof.as_slice()),
        PSLICE() << "cannot deserialize masterchain creators data proof for " << blkid.to_str() << ": ");
    if (status->data_proof.is_null()) {
      status->data_proof = std::move(data_root);
    } else {
      TRY_RESULT_PROMISE_PREFIX(promise, data_proof2,
                                vm::MerkleProof::combine_fast_status(status->data_proof, std::move(data_root)),
                                "cannot combine Merkle proofs for creator data");
      status->data_proof = std::move(data_proof2);
    }
  }
  bool allow_eq = (mode & 3) != 1;
  ton::Bits256 key{status->last_key};
  std::ostringstream os;
  try {
    auto dict = block::get_block_create_stats_dict(std::move(state));
    if (!dict) {
      promise.set_error(td::Status::Error("cannot extract BlockCreateStats from mc state"));
      return;
    }
    for (int i = 0; i < count + (int)complete; i++) {
      auto v = dict->lookup_nearest_key(key, true, allow_eq);
      if (v.is_null()) {
        if (i != count) {
          promise.set_error(td::Status::Error(PSLICE() << "could fetch only " << i << " CreatorStats entries out of "
                                                       << count << " declared in answer to getValidatorStats"));
          return;
        }
        break;
      }
      block::DiscountedCounter mc_cnt, shard_cnt;
      if (!block::unpack_CreatorStats(std::move(v), mc_cnt, shard_cnt)) {
        promise.set_error(td::Status::Error(PSLICE() << "invalid CreatorStats record with key " << key.to_hex()));
        return;
      }
      func(key, mc_cnt, shard_cnt);
      allow_eq = false;
    }
    if (complete) {
      status->last_key.set_zero();
      status->complete = true;
      promise.set_result(std::move(status));
    } else if (!(status->mode & 0x100)) {
      status->last_key = key;
      promise.set_result(std::move(status));
    } else {
      // incomplete, send new query to fetch next entries
      status->last_key = key;
      status->mode |= 1;
      get_creator_stats(blkid, req_count, min_utime, std::move(func), std::move(status), std::move(promise));
    }
  } catch (vm::VmError& err) {
    promise.set_error(err.as_status("error while traversing block creator stats:"));
  } catch (vm::VmVirtError& err) {
    promise.set_error(err.as_status("virtualization error while traversing block creator stats:"));
  }
}

bool TestNode::check_validator_load(int start_time, int end_time, int mode, std::string file_pfx) {
  int time = now();
  if (start_time <= 0) {
    start_time += time;
  }
  if (end_time <= 0) {
    end_time += time;
  }
  if (start_time >= end_time) {
    return set_error("end time must be later than start time");
  }
  LOG(INFO) << "requesting masterchain blocks corresponding to unixtime " << start_time << " and " << end_time;
  auto P = td::split_promise([this, mode, file_pfx](td::Result<std::pair<BlockHdrInfo, BlockHdrInfo>> R) {
    if (R.is_error()) {
      LOG(ERROR) << "cannot obtain block info: " << R.move_as_error();
      return;
    }
    auto res = R.move_as_ok();
    continue_check_validator_load(res.first.blk_id, res.first.proof, res.second.blk_id, res.second.proof, mode,
                                  file_pfx);
  });
  lookup_block(ton::ShardIdFull(ton::masterchainId), 4, start_time, std::move(P.first));
  return lookup_block(ton::ShardIdFull(ton::masterchainId), 4, end_time, std::move(P.second));
}

void TestNode::continue_check_validator_load(ton::BlockIdExt blkid1, Ref<vm::Cell> root1, ton::BlockIdExt blkid2,
                                             Ref<vm::Cell> root2, int mode, std::string file_pfx) {
  LOG(INFO) << "continue_check_validator_load for blocks " << blkid1.to_str() << " and " << blkid2.to_str()
            << " : requesting configuration parameter #34";
  auto P = td::split_promise(
      [this, blkid1, root1, blkid2, root2, mode, file_pfx](td::Result<std::pair<ConfigInfo, ConfigInfo>> R) mutable {
        if (R.is_error()) {
          LOG(ERROR) << "cannot obtain configuration parameter #34 : " << R.move_as_error();
          return;
        }
        auto res = R.move_as_ok();
        root1 = vm::MerkleProof::combine_fast(std::move(root1), std::move(res.first.state_proof));
        root2 = vm::MerkleProof::combine_fast(std::move(root2), std::move(res.second.state_proof));
        if (root1.is_null() || root2.is_null()) {
          LOG(ERROR) << "cannot merge block header proof with block state proof";
          return;
        }
        auto info1 = std::make_unique<ValidatorLoadInfo>(blkid1, std::move(root1), std::move(res.first.config_proof),
                                                         std::move(res.first.config));
        auto info2 = std::make_unique<ValidatorLoadInfo>(blkid2, std::move(root2), std::move(res.second.config_proof),
                                                         std::move(res.second.config));
        continue_check_validator_load2(std::move(info1), std::move(info2), mode, file_pfx);
      });
  get_config_params_ext(blkid1, std::move(P.first), 0x4000, "", {28, 34});
  get_config_params_ext(blkid2, std::move(P.second), 0x4000, "", {28, 34});
}

td::Status TestNode::ValidatorLoadInfo::unpack_vset() {
  if (!config) {
    return td::Status::Error("no configuration to unpack validator set");
  }
  auto vset_root_c = config->get_config_param(34);
  if (vset_root_c.is_null()) {
    vset_hash.set_zero();
    return td::Status::Error("no configuration parameter 34 for block "s + blk_id.to_str());
  }
  if (vset_root.not_null() && vset_root->get_hash() == vset_root_c->get_hash()) {
    vset_root_c = vset_root;
  } else {
    vset_root = vset_root_c;
  }
  vset_hash = vset_root->get_hash().bits();
  TRY_RESULT_PREFIX_ASSIGN(
      vset, block::Config::unpack_validator_set(vset_root),
      PSLICE() << "cannot unpack validator set from configuration parameter 34 of block " << blk_id.to_str() << " :");
  valid_since = vset->utime_since;
  vset_map = vset->compute_validator_map();
  return td::Status::OK();
}

bool TestNode::ValidatorLoadInfo::store_record(const td::Bits256& key, const block::DiscountedCounter& mc_cnt,
                                               const block::DiscountedCounter& shard_cnt) {
  if (!(mc_cnt.is_valid() && shard_cnt.is_valid())) {
    return false;
  }
  if (mc_cnt.total >= (1ULL << 60) || shard_cnt.total >= (1ULL << 60)) {
    return false;
  }
  if (key.is_zero()) {
    created_total.first = (td::int64)mc_cnt.total;
    created_total.second = (td::int64)shard_cnt.total;
    return true;
  }
  auto it = vset_map.find(key);
  if (it == vset_map.end()) {
    return false;
  }
  created.at(it->second) = std::make_pair(mc_cnt.total, shard_cnt.total);
  return true;
}

bool TestNode::load_creator_stats(std::unique_ptr<TestNode::ValidatorLoadInfo> load_to,
                                  td::Promise<std::unique_ptr<TestNode::ValidatorLoadInfo>> promise, bool need_proofs) {
  if (!load_to) {
    promise.set_error(td::Status::Error("no ValidatorLoadInfo"));
    return false;
  }
  auto& info = *load_to;
  info.created_total.first = info.created_total.second = 0;
  info.created.resize(0);
  info.created.resize(info.vset->total, std::make_pair<td::uint64, td::uint64>(0, 0));
  ton::UnixTime min_utime = info.valid_since - 1000;
  return get_creator_stats(
      info.blk_id, 1000, min_utime,
      [min_utime, &info](const td::Bits256& key, const block::DiscountedCounter& mc_cnt,
                         const block::DiscountedCounter& shard_cnt) -> bool {
        info.store_record(key, mc_cnt, shard_cnt);
        return true;
      },
      std::make_unique<CreatorStatsRes>(need_proofs ? 0x100 : 0x10100),
      td::PromiseCreator::lambda([load_to = std::move(load_to), promise = std::move(promise)](
                                     td::Result<std::unique_ptr<CreatorStatsRes>> R) mutable {
        TRY_RESULT_PROMISE_PREFIX(promise, res, std::move(R), "error obtaining creator stats:");
        if (!res->complete) {
          promise.set_error(td::Status::Error("incomplete creator stats"));
          return;
        }
        // merge
        load_to->state_proof =
            vm::MerkleProof::combine_fast(std::move(load_to->state_proof), std::move(res->state_proof));
        load_to->data_proof = vm::MerkleProof::combine_fast(std::move(load_to->data_proof), std::move(res->data_proof));
        promise.set_result(std::move(load_to));
      }));
}

void TestNode::continue_check_validator_load2(std::unique_ptr<TestNode::ValidatorLoadInfo> info1,
                                              std::unique_ptr<TestNode::ValidatorLoadInfo> info2, int mode,
                                              std::string file_pfx) {
  LOG(INFO) << "continue_check_validator_load2 for blocks " << info1->blk_id.to_str() << " and "
            << info2->blk_id.to_str() << " : requesting block creators data";
  td::Status st = info1->unpack_vset();
  if (st.is_error()) {
    LOG(ERROR) << "cannot unpack validator set from block " << info1->blk_id.to_str() << " :" << st.move_as_error();
    return;
  }
  st = info2->unpack_vset();
  if (st.is_error()) {
    LOG(ERROR) << "cannot unpack validator set from block " << info2->blk_id.to_str() << " :" << st.move_as_error();
    return;
  }
  if (info1->vset_hash != info2->vset_hash || info1->valid_since != info2->valid_since) {
    LOG(ERROR) << "blocks appear to have different validator sets";
    return;
  }
  LOG(INFO) << "validator sets valid since " << info1->valid_since;
  auto P = td::split_promise(
      [this, mode,
       file_pfx](td::Result<std::pair<std::unique_ptr<ValidatorLoadInfo>, std::unique_ptr<ValidatorLoadInfo>>> R) {
        if (R.is_error()) {
          LOG(ERROR) << "cannot load block creation statistics : " << R.move_as_error();
          return;
        }
        auto res = R.move_as_ok();
        continue_check_validator_load3(std::move(res.first), std::move(res.second), mode, file_pfx);
      });
  load_creator_stats(std::move(info1), std::move(P.first), true);
  load_creator_stats(std::move(info2), std::move(P.second), true);
}

// computes the probability of creating <= x blocks if the expected value is y
static double create_prob(int x, double y) {
  if (x < 0 || y < 0) {
    return .5;
  }
  if (x >= y) {
    return .5;
  }
  if (x <= 20) {
    // Poisson
    double t = exp(-y), s = t;
    for (int n = 1; n <= x; n++) {
      s += t = (t * y) / n;
    }
    return s;
  }
  // normal approximation
  double z = (x - y) / sqrt(2. * y);
  return (1. + erf(z)) / 2;
}

static double shard_create_prob(int x, double y, double chunk_size) {
  if (x < 0 || y < 0) {
    return .5;
  }
  if (x >= y) {
    return .5;
  }
  double y0 = y / chunk_size;  // expected chunks
  if (!x) {
    return y0 > 100 ? 0 : exp(-y0);  // Poisson approximation for having participated in zero chunks
  }
  // at least ten chunks, normal approximation
  double z = (x - y) / sqrt(2. * y * chunk_size);
  return (1. + erf(z)) / 2;
}

void TestNode::continue_check_validator_load3(std::unique_ptr<TestNode::ValidatorLoadInfo> info1,
                                              std::unique_ptr<TestNode::ValidatorLoadInfo> info2, int mode,
                                              std::string file_pfx) {
  LOG(INFO) << "continue_check_validator_load3 for blocks " << info1->blk_id.to_str() << " and "
            << info2->blk_id.to_str() << " with mode=" << mode << " and file prefix `" << file_pfx;

  if (mode & 4) {
    ton::BlockSeqno start_seqno = info1->blk_id.seqno();
    ton::BlockSeqno end_seqno = info2->blk_id.seqno();
    block::ValidatorSet validator_set = *info1->vset;
    if (info1->config->get_config_param(28)->get_hash() != info2->config->get_config_param(28)->get_hash()) {
      LOG(ERROR) << "Catchain validator config (28) changed between the first and the last block";
      return;
    }
    auto catchain_config = std::make_unique<block::CatchainValidatorsConfig>(
        block::Config::unpack_catchain_validators_config(info1->config->get_config_param(28)));
    load_validator_shard_shares(
        start_seqno, end_seqno, std::move(validator_set), std::move(catchain_config),
        [=, this, info1 = std::move(info1),
         info2 = std::move(info2)](td::Result<std::map<td::Bits256, td::uint64>> R) mutable {
          if (R.is_error()) {
            LOG(ERROR) << "failed to load validator shard shares: " << R.move_as_error();
          } else {
            continue_check_validator_load4(std::move(info1), std::move(info2), mode, file_pfx, R.move_as_ok());
          }
        });
  } else {
    continue_check_validator_load4(std::move(info1), std::move(info2), mode, std::move(file_pfx), {});
  }
}

void TestNode::continue_check_validator_load4(std::unique_ptr<TestNode::ValidatorLoadInfo> info1,
                                              std::unique_ptr<TestNode::ValidatorLoadInfo> info2, int mode,
                                              std::string file_pfx,
                                              std::map<td::Bits256, td::uint64> exact_shard_shares) {
  LOG(INFO) << "continue_check_validator_load4 for blocks " << info1->blk_id.to_str() << " and "
            << info2->blk_id.to_str() << " with mode=" << mode << " and file prefix `" << file_pfx;
  if (info1->created_total.first <= 0 || info2->created_total.first <= 0) {
    LOG(ERROR) << "no total created blocks statistics";
    return;
  }
  td::TerminalIO::out() << "total: (" << info1->created_total.first << "," << info1->created_total.second << ") -> ("
                        << info2->created_total.first << "," << info2->created_total.second << ")\n";
  auto created_total_mc = info2->created_total.first - info1->created_total.first;
  auto created_total_bc = info2->created_total.second - info1->created_total.second;
  td::int64 created_mc_sum = 0, created_bc_sum = 0;
  if (created_total_mc <= 0 || created_total_bc < 0 || (created_total_mc | created_total_bc) >= (1U << 31)) {
    LOG(ERROR) << "impossible situation: zero or no blocks created: " << created_total_mc << " masterchain blocks, "
               << created_total_bc << " shardchain blocks";
    return;
  }
  int count = info1->vset->total;
  CHECK(info2->vset->total == count);
  CHECK((int)info1->created.size() == count);
  CHECK((int)info2->created.size() == count);
  std::vector<std::pair<int, int>> vals_created;
  vals_created.reserve(count);
  for (int i = 0; i < count; i++) {
    auto created_mc = info2->created[i].first - info1->created[i].first;
    auto created_bc = info2->created[i].second - info1->created[i].second;
    if (created_mc < 0 || created_bc < 0 || (created_mc | created_bc) >= (1u << 31)) {
      LOG(ERROR) << "impossible situation: validator #" << i << " created a negative amount of blocks: " << created_mc
                 << " masterchain blocks, " << created_bc << " shardchain blocks";
      return;
    }
    created_mc_sum += created_mc;
    created_bc_sum += created_bc;
    vals_created.emplace_back((int)created_mc, (int)created_bc);
    td::TerminalIO::out() << "val #" << i << ": created (" << created_mc << "," << created_bc << ") ; was ("
                          << info1->created[i].first << "," << info1->created[i].second << ")\n";
  }
  if (created_mc_sum != created_total_mc || created_bc_sum != created_total_bc) {
    LOG(ERROR) << "cannot account for all blocks created: total is (" << created_total_mc << "," << created_total_bc
               << "), but the sum for all validators is (" << created_mc_sum << "," << created_bc_sum << ")";
    return;
  }
  td::TerminalIO::out() << "total: (" << created_total_mc << "," << created_total_bc << ")\n";
  auto ccfg = block::Config::unpack_catchain_validators_config(info2->config->get_config_param(28));
  auto ccfg_old = block::Config::unpack_catchain_validators_config(info1->config->get_config_param(28));
  if (ccfg.shard_val_num != ccfg_old.shard_val_num || ccfg.shard_val_num <= 0) {
    LOG(ERROR) << "shard validator group size changed from " << ccfg_old.shard_val_num << " to " << ccfg.shard_val_num
               << ", or is not positive";
    return;
  }
  int shard_vals = ccfg.shard_val_num, master_vals = info2->vset->main;
  if (info1->vset->main != master_vals || master_vals <= 0) {
    LOG(ERROR) << "masterchain validator group size changed from " << info1->vset->main << " to " << master_vals
               << ", or is not positive";
    return;
  }

  bool use_exact_shard_share = mode & 4;
  int proofs_cnt = 0, proofs_cnt_ok = 0;
  double chunk_size = ccfg.shard_val_lifetime / 3. / shard_vals;

  std::vector<double> mtc_shard_share;
  if (use_exact_shard_share) {
    LOG(INFO) << "using exact shard shares";
    td::uint64 exact_shard_shares_sum = 0;
    for (auto& [_, count] : exact_shard_shares) {
      exact_shard_shares_sum += count;
    }
    if ((td::int64)exact_shard_shares_sum != shard_vals * created_bc_sum) {
      LOG(ERROR) << "unexpected total shard shares: blocks=" << created_bc_sum << ", shard_vals=" << shard_vals
                 << ", expected_sum=" << shard_vals * created_bc_sum << ", found=" << exact_shard_shares_sum;
      return;
    }
  } else {
    LOG(INFO) << "using MtCarloComputeShare";
    block::MtCarloComputeShare mtc(shard_vals, info2->vset->export_scaled_validator_weights());
    if (!mtc.is_ok()) {
      LOG(ERROR) << "failed to compute shard shares";
      return;
    }
    mtc_shard_share.resize(count);
    for (size_t i = 0; i < count; ++i) {
      mtc_shard_share[i] = mtc[i];
    }
  }

  auto validators = info1->vset->export_validator_set();
  for (int i = 0; i < count; i++) {
    int created_mc = vals_created[i].first, created_bc = vals_created[i].second;
    bool is_masterchain_validator = i < master_vals;

    double expected_created_mc = (is_masterchain_validator ? (double)created_mc_sum / master_vals : 0);
    double prob_mc = create_prob(created_mc, .9 * expected_created_mc);

    double expected_created_bc, prob_bc;
    if (use_exact_shard_share) {
      expected_created_bc = (double)exact_shard_shares[validators[i].key.as_bits256()] / shard_vals;
      prob_bc = create_prob(created_bc, .9 * expected_created_bc);
    } else {
      expected_created_bc = mtc_shard_share[i] * (double)created_bc_sum / shard_vals;
      prob_bc = shard_create_prob(created_bc, .9 * expected_created_bc, chunk_size);
    }

    td::Bits256 pk = info2->vset->list[i].pubkey.as_bits256();
    td::TerminalIO::out() << "val #" << i << ": pubkey " << pk.to_hex() << ", blocks created (" << created_mc << ","
                          << created_bc << "), expected (" << expected_created_mc << "," << expected_created_bc
                          << "), probabilities " << prob_mc << " and " << prob_bc << "\n";
    if ((is_masterchain_validator ? prob_mc : prob_bc) < .00001) {
      LOG(ERROR) << "validator #" << i << " with pubkey " << pk.to_hex()
                 << " : serious misbehavior detected: created less than 90% of the expected amount of blocks with "
                    "probability 99.999% : created ("
                 << created_mc << "," << created_bc << "), expected (" << expected_created_mc << ","
                 << expected_created_bc << ") masterchain/shardchain blocks\n";
      if (mode & 2) {
        auto st = write_val_create_proof(*info1, *info2, i, true, file_pfx, ++proofs_cnt);
        if (st.is_error()) {
          LOG(ERROR) << "cannot create proof: " << st.move_as_error();
        } else {
          proofs_cnt_ok++;
        }
      }
    } else if ((is_masterchain_validator ? prob_mc : prob_bc) < .005) {
      LOG(ERROR) << "validator #" << i << " with pubkey " << pk.to_hex()
                 << " : moderate misbehavior detected: created less than 90% of the expected amount of blocks with "
                    "probability 99.5% : created ("
                 << created_mc << "," << created_bc << "), expected (" << expected_created_mc << ","
                 << expected_created_bc << ") masterchain/shardchain blocks\n";
      if ((mode & 3) == 2) {
        auto st = write_val_create_proof(*info1, *info2, i, false, file_pfx, ++proofs_cnt);
        if (st.is_error()) {
          LOG(ERROR) << "cannot create proof: " << st.move_as_error();
        } else {
          proofs_cnt_ok++;
        }
      }
    }
  }
  if (proofs_cnt > 0) {
    LOG(INFO) << proofs_cnt_ok << " out of " << proofs_cnt << " proofs written to " << file_pfx << "-*.boc";
  }
}

void TestNode::load_validator_shard_shares(ton::BlockSeqno start_seqno, ton::BlockSeqno end_seqno,
                                           block::ValidatorSet validator_set,
                                           std::unique_ptr<block::CatchainValidatorsConfig> catchain_config,
                                           td::Promise<std::map<td::Bits256, td::uint64>> promise) {
  CHECK(start_seqno <= end_seqno);
  LOG(INFO) << "loading shard shares from mc blocks " << start_seqno << ".." << end_seqno << " ("
            << end_seqno - start_seqno + 1 << " blocks)";
  auto state = std::make_shared<LoadValidatorShardSharesState>();
  state->start_seqno = start_seqno;
  state->end_seqno = end_seqno;
  state->validator_set = std::move(validator_set);
  state->catchain_config = std::move(catchain_config);
  state->shard_configs.resize(end_seqno - start_seqno + 1);
  state->promise = std::move(promise);
  load_validator_shard_shares_cont(std::move(state));
}

void TestNode::load_validator_shard_shares_cont(std::shared_ptr<LoadValidatorShardSharesState> state) {
  if (!state->promise) {
    return;
  }
  if (state->loaded % 100 == 0) {
    LOG(INFO) << "loaded " << state->loaded << "/" << state->shard_configs.size() << " mc blocks";
  }
  while (state->cur_idx < state->shard_configs.size() && state->pending < 8) {
    load_block_shard_configuration(state->start_seqno + state->cur_idx,
                                   [this, state, idx = state->cur_idx](td::Result<block::ShardConfig> R) mutable {
                                     if (R.is_error()) {
                                       state->promise.set_error(R.move_as_error());
                                       state->promise = {};
                                     } else {
                                       state->shard_configs[idx] = R.move_as_ok();
                                       --state->pending;
                                       ++state->loaded;
                                       load_validator_shard_shares_cont(std::move(state));
                                     }
                                   });
    ++state->pending;
    ++state->cur_idx;
  }

  if (state->loaded != state->shard_configs.size()) {
    return;
  }
  LOG(INFO) << "loaded all " << state->shard_configs.size() << " mc blocks, computing shard shares";
  std::map<td::Bits256, td::uint64> result;
  try {
    for (size_t idx = 0; idx + 1 < state->shard_configs.size(); ++idx) {
      block::ShardConfig& shards1 = state->shard_configs[idx];
      block::ShardConfig& shards2 = state->shard_configs[idx + 1];

      // Compute validator groups, see ValidatorManagerImpl::update_shards
      auto process_shard = [&](ton::ShardIdFull shard, ton::BlockSeqno first_seqno) {
        auto desc2 = shards2.get_shard_hash(shard);
        if (desc2.is_null() || desc2->seqno() < first_seqno) {
          return;
        }
        td::uint32 blocks_count = desc2->seqno() - first_seqno + 1;
        ton::CatchainSeqno cc_seqno = shards1.get_shard_cc_seqno(shard);
        auto val_set =
            block::ConfigInfo::do_compute_validator_set(*state->catchain_config, shard, state->validator_set, cc_seqno);
        for (const auto &val : val_set) {
          result[val.key.as_bits256()] += blocks_count;
        }
      };

      for (const ton::BlockId& id : shards1.get_shard_hash_ids()) {
        ton::ShardIdFull shard = id.shard_full();
        auto desc = shards1.get_shard_hash(shard);
        CHECK(desc.not_null());
        if (desc->before_split()) {
          ton::ShardIdFull l_shard = shard_child(shard, true);
          ton::ShardIdFull r_shard = shard_child(shard, false);
          process_shard(l_shard, desc->seqno() + 1);
          process_shard(r_shard, desc->seqno() + 1);
        } else if (desc->before_merge()) {
          if (is_right_child(shard)) {
            continue;
          }
          ton::ShardIdFull sibling_shard = shard_sibling(shard);
          auto sibling_desc = shards1.get_shard_hash(sibling_shard);
          CHECK(sibling_desc.not_null());
          ton::ShardIdFull p_shard = shard_parent(shard);
          process_shard(p_shard, std::max(desc->seqno(), sibling_desc->seqno()) + 1);
        } else {
          process_shard(shard, desc->seqno() + 1);
        }
      }
    }
  } catch (vm::VmError &e) {
    state->promise.set_error(e.as_status("cannot parse shard hashes: "));
    return;
  }
  state->promise.set_value(std::move(result));
}

void TestNode::load_block_shard_configuration(ton::BlockSeqno seqno, td::Promise<block::ShardConfig> promise) {
  lookup_block(
      ton::ShardIdFull{ton::masterchainId}, 1, seqno,
      [this, promise = std::move(promise)](td::Result<BlockHdrInfo> R) mutable {
        TRY_RESULT_PROMISE(promise, res, std::move(R));
        auto b = ton::serialize_tl_object(
            ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(res.blk_id)),
            true);
        envelope_send_query(std::move(b), [this, promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          TRY_RESULT_PROMISE(promise, data, std::move(R));
          TRY_RESULT_PROMISE(promise, f, ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(data, true));
          TRY_RESULT_PROMISE(promise, root, vm::std_boc_deserialize(f->data_));
          block::ShardConfig sh_conf;
          if (!sh_conf.unpack(load_cell_slice_ref(root))) {
            promise.set_error(td::Status::Error("cannot extract shard block list from shard configuration"));
          } else {
            promise.set_value(std::move(sh_conf));
          }
        });
      });
}

bool compute_punishment_default(int interval, bool severe, td::RefInt256& fine, unsigned& fine_part) {
  if (interval <= 1000) {
    return false;  // no punishments for less than 1000 seconds
  }

  fine = td::make_refint(101 * 1000000000LL);  // 101
  fine_part = 0;

  return true; // todo: (tolya-yanot) temporary reduction of fine

  if (severe) {
    fine = td::make_refint(2500 * 1000000000LL);  // GR$2500
    fine_part = (1 << 30);                        // 1/4 of stake
  } else {
    fine = td::make_refint(1000 * 1000000000LL);  // GR$1000
    fine_part = (1 << 28);                        // 1/16 of stake
  }
  if (interval >= 80000) {
    return true;
  }
  if (interval >= 20000) {
    fine >>= 2;
    fine_part >>= 2;
    return true;
  }
  fine >>= 4;
  fine_part >>= 4;
  return true;
}

bool compute_punishment(int interval, bool severe, td::RefInt256& fine, unsigned& fine_part, Ref<vm::Cell> punishment_params) {
  if(punishment_params.is_null()) {
    return compute_punishment_default(interval, severe, fine, fine_part);
  }
  block::gen::MisbehaviourPunishmentConfig::Record rec;
  if (!tlb::unpack_cell(punishment_params, rec)) {
      return false;
  }

  if(interval <= rec.unpunishable_interval) {
      return false;
  }

  fine = block::tlb::t_Grams.as_integer(rec.default_flat_fine);
  fine_part = rec.default_proportional_fine;

  if (severe) {
    fine = fine * rec.severity_flat_mult; fine >>= 8;
    fine_part = fine_part * rec.severity_proportional_mult; fine_part >>= 8;
  }

  if (interval >= rec.long_interval) {
    fine = fine * rec.long_flat_mult; fine >>= 8;
    fine_part = fine_part * rec.long_proportional_mult; fine_part >>= 8;
    return true;
  }
  if (interval >= rec.medium_interval) {
    fine = fine * rec.medium_flat_mult; fine >>= 8;
    fine_part = fine_part * rec.medium_proportional_mult; fine_part >>= 8;
    return true;
  }
  return true;
}

bool check_punishment(int interval, bool severe, td::RefInt256 fine, unsigned fine_part, Ref<vm::Cell> punishment_params) {
  td::RefInt256 computed_fine;
  unsigned computed_fine_part;
  return compute_punishment(interval, severe, computed_fine, computed_fine_part, punishment_params) &&
         std::llabs((long long)fine_part - (long long)computed_fine_part) <=
             (std::max(fine_part, computed_fine_part) >> 3) &&
         fine * 7 <= computed_fine * 8 && computed_fine * 7 <= fine * 8;
}

td::Status TestNode::write_val_create_proof(TestNode::ValidatorLoadInfo& info1, TestNode::ValidatorLoadInfo& info2,
                                            int idx, bool severe, std::string file_pfx, int cnt) {
  std::string filename = PSTRING() << file_pfx << '-' << cnt << ".boc";
  if (!info1.has_data()) {
    return td::Status::Error("first block information is incomplete");
  }
  if (!info2.has_data()) {
    return td::Status::Error("second block information is incomplete");
  }
  LOG(INFO) << "creating proof file " << filename;
  TRY_STATUS(info1.check_header_proof(&info1.block_created_at, &info1.end_lt));
  TRY_STATUS(info2.check_header_proof(&info2.block_created_at, &info2.end_lt));
  td::Bits256 val_pk1, val_pk2;
  TRY_RESULT(prod1, info1.build_producer_info(idx, &val_pk1));
  TRY_RESULT(prod2, info2.build_producer_info(idx, &val_pk2));
  if (val_pk1 != val_pk2) {
    return td::Status::Error("validator public key mismatch");
  }
  int interval = (int)(info2.block_created_at - info1.block_created_at);
  if (interval <= 0) {
    return td::Status::Error("non-positive time interval");
  }

  auto punishment_params = info2.config->get_config_param(40);

  int severity = (severe ? 2 : 1);
  td::RefInt256 fine = td::make_refint(101000000000);
  unsigned fine_part = 0; // todo: (tolya-yanot) temporary reduction of fine  // 0xffffffff / 16;  // 1/16
  if (!compute_punishment(interval, severe, fine, fine_part, punishment_params)) {
    return td::Status::Error("cannot compute adequate punishment");
  }
  Ref<vm::Cell> cpl_descr, complaint;
  vm::CellBuilder cb;
  // no_blk_gen_diff prod_info_old:^ProducerInfo prod_info_new:^ProducerInfo = ComplaintDescr
  if (!(block::gen::t_ComplaintDescr.cell_pack_no_blk_gen_diff(cpl_descr, prod1, prod2) &&
        cb.store_long_bool(0xbc, 8)                                    // validator_complaint#bc
        && cb.store_bits_bool(val_pk1)                                 // validator_pubkey:uint256
        && cb.store_ref_bool(cpl_descr)                                // description:^ComplaintDescr
        && cb.store_long_bool(now(), 32)                               // created_at:uint32
        && cb.store_long_bool(severity, 8)                             // severity:uint8
        && cb.store_zeroes_bool(256)                                   // reward_addr:uint256
        && cb.store_zeroes_bool(4)                                     // paid:Grams
        && block::tlb::t_Grams.store_integer_ref(cb, std::move(fine))  // suggested_fine:Grams
        && cb.store_long_bool(fine_part, 32)                           // suggested_fine_part:uint32
        && cb.finalize_to(complaint))) {
    return td::Status::Error("cannot serialize ValidatorComplaint");
  }
  if (verbosity >= 5) {
    std::ostringstream os;
    os << "complaint: ";
    block::gen::t_ValidatorComplaint.print_ref(print_limit_, os, complaint);
    td::TerminalIO::out() << os.str() << std::endl;
  }
  if (!block::gen::t_ComplaintDescr.validate_ref(cpl_descr)) {
    return td::Status::Error("created an invalid ComplaintDescr");
  }
  if (!block::gen::t_ValidatorComplaint.validate_ref(complaint)) {
    return td::Status::Error("created an invalid ValidatorComplaint");
  }
  TRY_RESULT_PREFIX(boc, vm::std_boc_serialize(complaint, 2), "cannot create boc:");
  auto size = boc.size();
  TRY_STATUS_PREFIX(td::write_file(filename, std::move(boc)), PSLICE() << "cannot save file `" << filename << "` :");
  td::TerminalIO::out() << "saved validator misbehavior proof into file `" << filename << "` (" << size
                        << " bytes written)" << std::endl;
  td::TerminalIO::out() << "COMPLAINT_SAVED\t" << info1.vset_hash.to_hex() << "\t" << complaint->get_hash().to_hex()
                        << "\t" << filename << std::endl;
  return td::Status::OK();
}

td::Status TestNode::ValidatorLoadInfo::check_header_proof(ton::UnixTime* save_utime, ton::LogicalTime* save_lt) const {
  auto state_virt_root = vm::MerkleProof::virtualize(std::move(data_proof), 1);
  if (state_virt_root.is_null()) {
    return td::Status::Error("account state proof is invalid");
  }
  td::Bits256 state_hash = state_virt_root->get_hash().bits();
  TRY_STATUS(block::check_block_header_proof(vm::MerkleProof::virtualize(state_proof, 1), blk_id, &state_hash, true,
                                             save_utime, save_lt));
  return td::Status::OK();
}

static bool visit(Ref<vm::Cell> cell);

static bool visit(const vm::CellSlice& cs) {
  auto cnt = cs.size_refs();
  bool res = true;
  for (unsigned i = 0; i < cnt; i++) {
    res &= visit(cs.prefetch_ref(i));
  }
  return res;
}

static bool visit(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return true;
  }
  vm::CellSlice cs{vm::NoVm{}, std::move(cell)};
  return visit(cs);
}

static bool visit(Ref<vm::CellSlice> cs_ref) {
  return cs_ref.is_null() || visit(*cs_ref);
}

td::Result<Ref<vm::Cell>> TestNode::ValidatorLoadInfo::build_proof(int idx, td::Bits256* save_pubkey) const {
  try {
    auto state_virt_root = vm::MerkleProof::virtualize(std::move(data_proof), 1);
    if (state_virt_root.is_null()) {
      return td::Status::Error("account state proof is invalid");
    }
    vm::MerkleProofBuilder pb{std::move(state_virt_root)};
    TRY_RESULT(cfg, block::Config::extract_from_state(pb.root()));
    visit(cfg->get_config_param(28));
    block::gen::ValidatorSet::Record_validators_ext rec;
    if (!tlb::unpack_cell(cfg->get_config_param(34), rec)) {
      return td::Status::Error("cannot unpack ValidatorSet");
    }
    vm::Dictionary vdict{rec.list, 16};
    auto entry = vdict.lookup(td::BitArray<16>(idx));
    if (entry.is_null()) {
      return td::Status::Error("validator entry not found");
    }
    Ref<vm::CellSlice> pk;
    block::gen::ValidatorDescr::Record_validator rec1;
    block::gen::ValidatorDescr::Record_validator_addr rec2;
    if (tlb::csr_unpack(entry, rec1)) {
      pk = std::move(rec1.public_key);
    } else if (tlb::csr_unpack(std::move(entry), rec2)) {
      pk = std::move(rec2.public_key);
    } else {
      return td::Status::Error("cannot unpack ValidatorDescr");
    }
    block::gen::SigPubKey::Record rec3;
    if (!tlb::csr_unpack(std::move(pk), rec3)) {
      return td::Status::Error("cannot unpack ed25519_pubkey");
    }
    if (save_pubkey) {
      *save_pubkey = rec3.pubkey;
    }
    visit(std::move(entry));
    auto dict = block::get_block_create_stats_dict(pb.root());
    if (!dict) {
      return td::Status::Error("cannot extract BlockCreateStats from mc state");
    }
    visit(dict->lookup(rec3.pubkey));
    visit(dict->lookup(td::Bits256::zero()));
    return pb.extract_proof();
  } catch (vm::VmError& err) {
    return err.as_status("cannot build proof: ");
  } catch (vm::VmVirtError& err) {
    return err.as_status("cannot build proof: ");
  }
}

td::Result<Ref<vm::Cell>> TestNode::ValidatorLoadInfo::build_producer_info(int idx, td::Bits256* save_pubkey) const {
  TRY_RESULT(proof, build_proof(idx, save_pubkey));
  vm::CellBuilder cb;
  Ref<vm::Cell> res;
  if (!(cb.store_long_bool(0x34, 8)                           // prod_info#34
        && cb.store_long_bool(block_created_at, 32)           // utime:uint32
        && block::tlb::t_ExtBlkRef.store(cb, blk_id, end_lt)  // mc_blk_ref:ExtBlkRef
        && cb.store_ref_bool(state_proof)                     // state_proof:^Cell
        && cb.store_ref_bool(proof)                           // prod_proof:^Cell = ProducerInfo
        && cb.finalize_to(res))) {
    return td::Status::Error("cannot construct ProducerInfo");
  }
  if (!block::gen::t_ProducerInfo.validate_ref(res)) {
    return td::Status::Error("constructed ProducerInfo failed to pass automated validity checks");
  }
  return std::move(res);
}

td::Status TestNode::check_validator_load_proof(std::string filename, std::string vset_filename,
                                                ton::Bits256 vset_hash) {
  TRY_RESULT_PREFIX(data, td::read_file(filename), "cannot read proof file:");
  TRY_RESULT_PREFIX(root, vm::std_boc_deserialize(std::move(data)),
                    PSTRING() << "cannot deserialize boc from file `" << filename << "`:");
  Ref<vm::Cell> vset_root;
  if (!vset_filename.empty()) {
    TRY_RESULT_PREFIX(vdata, td::read_file(vset_filename), "cannot read validator set file:");
    TRY_RESULT_PREFIX_ASSIGN(vset_root, vm::std_boc_deserialize(std::move(vdata)),
                             PSLICE() << "cannot deserialize validator set boc from file `" << vset_filename << "`:");
    if (vset_hash.is_zero()) {
      vset_hash = vset_root->get_hash().bits();
    } else if (vset_hash != vset_root->get_hash().bits()) {
      return td::Status::Error("validator set hash mismatch: indicated "s + vset_hash.to_hex() + ", loaded from file " +
                               vset_root->get_hash().to_hex());
    }
  }
  if (verbosity >= 5) {
    std::ostringstream os;
    os << "complaint: ";
    block::gen::t_ValidatorComplaint.print_ref(print_limit_, os, root);
    td::TerminalIO::out() << os.str() << std::endl;
  }
  if (!block::gen::t_ValidatorComplaint.validate_ref(root)) {
    return td::Status::Error("proof file does not contain a valid ValidatorComplaint");
  }
  block::gen::ValidatorComplaint::Record rec;
  if (!tlb::unpack_cell(root, rec)) {
    return td::Status::Error("cannot unpack ValidatorComplaint");
  }
  auto cs = vm::load_cell_slice(rec.description);
  int tag = block::gen::t_ComplaintDescr.get_tag(cs);
  if (tag < 0) {
    return td::Status::Error("ComplaintDescr has an unknown tag");
  }
  if (tag != block::gen::ComplaintDescr::no_blk_gen_diff) {
    return td::Status::Error("can check only ComplaintDescr of type no_blk_gen_diff");
  }
  block::gen::ComplaintDescr::Record_no_blk_gen_diff crec;
  if (!tlb::unpack_exact(cs, crec)) {
    return td::Status::Error("cannot unpack ComplaintDescr");
  }
  TRY_RESULT_PREFIX(info1, ValidatorLoadInfo::preinit_from_producer_info(crec.prod_info_old),
                    "cannot unpack ProducerInfo in prod_info_old:")
  TRY_RESULT_PREFIX(info2, ValidatorLoadInfo::preinit_from_producer_info(crec.prod_info_new),
                    "cannot unpack ProducerInfo in prod_info_new:")
  if (info1->vset_hash != info2->vset_hash) {
    return td::Status::Error("validator hash changed between the two blocks: "s + info1->vset_hash.to_hex() + " and " +
                             info2->vset_hash.to_hex());
  }
  if (vset_hash.is_zero()) {
    vset_hash = info1->vset_hash;
  } else if (vset_hash != info1->vset_hash) {
    return td::Status::Error("validator set hash mismatch: blocks have "s + info1->vset_hash.to_hex() +
                             ", actual value is " + vset_hash.to_hex());
  }
  auto blkid2 = info2->blk_id;
  if (vset_root.not_null()) {
    info1->vset_root = info2->vset_root = std::move(vset_root);
    set_error(continue_check_validator_load_proof(std::move(info1), std::move(info2), std::move(root)));
  } else if (!get_config_params(blkid2,
                                [this, info1 = std::move(info1), info2 = std::move(info2),
                                 root = std::move(root)](td::Result<std::unique_ptr<block::Config>> res) mutable {
                                  if (res.is_error()) {
                                    LOG(ERROR)
                                        << "cannot fetch configuration parameters from key block corresponding to "
                                        << info2->blk_id.to_str() << " : " << res.move_as_error();
                                  } else {
                                    auto vset_root = res.move_as_ok()->get_config_param(34);
                                    if (vset_root.is_null()) {
                                      LOG(ERROR) << "no configuration parameter #34 in key block corresponding to "
                                                 << info2->blk_id.to_str();
                                    } else if (info2->vset_hash != vset_root->get_hash().bits()) {
                                      LOG(ERROR) << "validator hash set mismatch for block " << info2->blk_id.to_str();
                                    } else {
                                      info1->vset_root = info2->vset_root = std::move(vset_root);
                                      set_error(continue_check_validator_load_proof(std::move(info1), std::move(info2),
                                                                                    std::move(root)));
                                    }
                                  }
                                },
                                0xd000, "", {28, 34})) {
    return td::Status::Error("cannot request configuration parameters from key block corresponding to "s +
                             blkid2.to_str());
  }
  return td::Status::OK();
}

static void show_vote(td::Bits256 complaint_hash, bool outcome) {
  td::TerminalIO::out() << "COMPLAINT_VOTE_FOR\t" << complaint_hash.to_hex() << "\t" << (outcome ? "YES" : "NO")
                        << std::endl;
}

td::Status TestNode::continue_check_validator_load_proof(std::unique_ptr<ValidatorLoadInfo> info1,
                                                         std::unique_ptr<ValidatorLoadInfo> info2, Ref<vm::Cell> root) {
  TRY_STATUS(info1->unpack_vset());
  TRY_STATUS(info2->unpack_vset());
  int interval = (int)(info2->block_created_at - info1->block_created_at);
  if (interval <= 0) {
    return td::Status::Error("non-positive time interval");
  }
  block::gen::ValidatorComplaint::Record rec;
  block::gen::ComplaintDescr::Record_no_blk_gen_diff crec;
  if (!(tlb::unpack_cell(root, rec) && tlb::unpack_cell(rec.description, crec))) {
    return td::Status::Error("cannot unpack ValidatorComplaint second time (?)");
  }
  td::Bits256 val_pubkey = rec.validator_pubkey;
  int val_idx = info1->vset->lookup_public_key(val_pubkey);
  if (val_idx < 0) {
    return td::Status::Error("validator with public key "s + val_pubkey.to_hex() +
                             " is not present in active validator set");
  }
  TRY_STATUS(info1->load_special_creator_stat(val_pubkey, true));
  TRY_STATUS(info2->load_special_creator_stat(val_pubkey, true));
  td::TerminalIO::out() << "total: (" << info1->created_total.first << "," << info1->created_total.second << ") -> ("
                        << info2->created_total.first << "," << info2->created_total.second << ")\n";
  auto x = info2->created_total.first - info1->created_total.first;
  auto y = info2->created_total.second - info1->created_total.second;
  if (x <= 0 || y < 0 || (x | y) >= (1u << 31)) {
    return td::Status::Error(PSLICE() << "impossible situation: zero or no blocks created: " << x
                                      << " masterchain blocks, " << y << " shardchain blocks");
  }
  auto x1 = info2->created_special.first - info1->created_special.first;
  auto y1 = info2->created_special.second - info1->created_special.second;
  if ((x1 | y1) < 0 || (x1 | y1) >= (1u << 31)) {
    return td::Status::Error(PSLICE() << "impossible situation: validator " << val_pubkey.to_hex() << " created " << x1
                                      << " masterchain blocks, " << y1 << " shardchain blocks");
  }
  td::TerminalIO::out() << "total: (" << x << "," << y << ")\n";
  try {
    auto ccfg = block::Config::unpack_catchain_validators_config(info2->config->get_config_param(28));
    auto ccfg_old = block::Config::unpack_catchain_validators_config(info1->config->get_config_param(28));
    if (ccfg.shard_val_num != ccfg_old.shard_val_num || ccfg.shard_val_num <= 0) {
      return td::Status::Error(PSLICE() << "shard validator group size changed from " << ccfg_old.shard_val_num
                                        << " to " << ccfg.shard_val_num << ", or is not positive");
    }
    int shard_count = ccfg.shard_val_num, main_count = info2->vset->main;
    if (info1->vset->main != main_count || main_count <= 0) {
      return td::Status::Error(PSLICE() << "masterchain validator group size changed from " << info1->vset->main
                                        << " to " << main_count << ", or is not positive");
    }
    double chunk_size = ccfg.shard_val_lifetime / 3. / shard_count;
    block::MtCarloComputeShare shard_share(shard_count, info2->vset->export_scaled_validator_weights());

    double xe = (val_idx < main_count ? (double)x / main_count : 0);
    double ye = shard_share[val_idx] * (double)y / shard_count;
    td::Bits256 pk = info2->vset->list[val_idx].pubkey.as_bits256();
    CHECK(pk == val_pubkey);
    double p1 = create_prob((int)x1, .9 * xe), p2 = shard_create_prob((int)y1, .9 * ye, chunk_size);
    td::TerminalIO::out() << "val #" << val_idx << ": pubkey " << pk.to_hex() << ", blocks created (" << x1 << "," << y1
                          << "), expected (" << xe << "," << ye << "), probabilities " << p1 << " and " << p2 << "\n";
    bool severe = (rec.severity >= 2);
    if (severe && std::min(p1, p2) < .00001) {
      LOG(ERROR) << "validator #" << val_idx << " with pubkey " << pk.to_hex()
                 << " : serious misbehavior detected: created less than 90% of the expected amount of blocks with "
                    "probability 99.999% : created ("
                 << x1 << "," << y1 << "), expected (" << xe << "," << ye << ") masterchain/shardchain blocks\n";
    } else if (!severe && std::min(p1, p2) < .001) {
      LOG(ERROR) << "validator #" << val_idx << " with pubkey " << pk.to_hex()
                 << " : moderate misbehavior detected: created less than 90% of the expected amount of blocks with "
                    "probability 99.9% : created ("
                 << x1 << "," << y1 << "), expected (" << xe << "," << ye << ") masterchain/shardchain blocks\n";
    } else {
      LOG(ERROR) << "invalid (unsupported) complaint for validator #" << val_idx << " with pubkey " << pk.to_hex();
      show_vote(root->get_hash().bits(), false);
      return td::Status::OK();
    }
    auto suggested_fine = block::tlb::t_Grams.as_integer(rec.suggested_fine);
    if (suggested_fine.is_null()) {
      return td::Status::Error("cannot parse suggested fine");
    }
    if (!check_punishment(interval, severe, suggested_fine, rec.suggested_fine_part, info2->config->get_config_param(40))) {
      LOG(ERROR) << "proposed punishment (fine " << td::dec_string(suggested_fine)
                 << ", fine_part=" << (double)rec.suggested_fine_part / (1LL << 32) << " is too harsh";
      show_vote(root->get_hash().bits(), false);
      return td::Status::OK();
    }
    LOG(INFO) << "accepting suggested punishment (affirmative vote)";
    show_vote(root->get_hash().bits(), true);
    return td::Status::OK();
  } catch (vm::VmError& err) {
    return err.as_status("vm error while scanning configuration proof:");
  } catch (vm::VmVirtError& err) {
    return err.as_status("virtualization error while scanning configuration proof:");
  }
}

td::Status TestNode::ValidatorLoadInfo::load_special_creator_stat(const td::Bits256& spec_pubkey, bool load_total) {
  if (!vset) {
    return td::Status::Error("no validator set loaded");
  }
  int idx = vset->lookup_public_key(spec_pubkey);
  if (idx < 0) {
    return td::Status::Error("validator with public key "s + spec_pubkey.to_hex() + " not present in validator set");
  }
  if (virt_root.is_null()) {
    return td::Status::Error("no virtualized block state");
  }
  try {
    auto dict = block::get_block_create_stats_dict(virt_root);
    if (!dict) {
      return td::Status::Error("cannot extract BlockCreateStats from virtualized mc state");
    }
    for (int i = 0; i <= (int)load_total; i++) {
      td::Bits256 key = (i ? td::Bits256::zero() : spec_pubkey);
      auto& p = *(i ? &created_total : &created_special);
      auto cell = dict->lookup(key);
      if (cell.is_null()) {
        p.first = p.second = 0;
        if (i) {
          return td::Status::Error("no total created block statistics in BlockCreateStats");
        }
      } else {
        block::DiscountedCounter mc_cnt, shard_cnt;
        if (!block::unpack_CreatorStats(std::move(cell), mc_cnt, shard_cnt)) {
          return td::Status::Error(PSLICE() << "invalid CreatorStats record with key " << key.to_hex());
        }
        p.first = mc_cnt.total;
        p.second = shard_cnt.total;
      }
    }
    special_idx = idx;
    return td::Status::OK();
  } catch (vm::VmError& err) {
    return err.as_status("vm error while extracting block creator data: ");
  } catch (vm::VmVirtError& err) {
    return err.as_status("virtualization error while extracting block creator data: ");
  }
}

td::Result<std::unique_ptr<TestNode::ValidatorLoadInfo>> TestNode::ValidatorLoadInfo::preinit_from_producer_info(
    Ref<vm::Cell> prod_info) {
  if (prod_info.is_null()) {
    return td::Status::Error("ProducerInfo cell is null");
  }
  if (!block::gen::t_ProducerInfo.validate_ref(prod_info)) {
    return td::Status::Error("invalid ProducerInfo");
  }
  block::gen::ProducerInfo::Record rec;
  ton::BlockIdExt blk_id;
  ton::LogicalTime end_lt;
  if (!(tlb::unpack_cell(prod_info, rec) &&
        block::tlb::t_ExtBlkRef.unpack(std::move(rec.mc_blk_ref), blk_id, &end_lt))) {
    return td::Status::Error("cannot unpack ProducerInfo");
  }
  auto info = std::make_unique<ValidatorLoadInfo>(blk_id, std::move(rec.state_proof), std::move(rec.prod_proof));
  CHECK(info);
  info->end_lt = end_lt;
  info->block_created_at = rec.utime;
  TRY_STATUS_PREFIX(info->init_check_proofs(), "error checking block/state proofs:");
  return std::move(info);
}

td::Status TestNode::ValidatorLoadInfo::init_check_proofs() {
  try {
    ton::UnixTime utime;
    ton::LogicalTime lt;
    TRY_STATUS(check_header_proof(&utime, &lt));
    if (utime != block_created_at) {
      return td::Status::Error(PSLICE() << "incorrect block creation time: declared " << block_created_at << ", actual "
                                        << utime);
    }
    if (lt != end_lt) {
      return td::Status::Error(PSLICE() << "incorrect block logical time: declared " << end_lt << ", actual " << lt);
    }
    auto vstate = vm::MerkleProof::virtualize(data_proof, 1);
    if (vstate.is_null()) {
      return td::Status::Error(PSLICE() << "cannot virtualize state of block " << blk_id.to_str());
    }
    TRY_RESULT_PREFIX_ASSIGN(config, block::Config::extract_from_state(vstate, 0), "cannot unpack configuration:");
    auto vset_root = config->get_config_param(34);
    if (vset_root.is_null()) {
      vset_hash.set_zero();
      return td::Status::Error(PSLICE() << "no configuration parameter 34 (validator set) for block "
                                        << blk_id.to_str());
    }
    vset_hash = vset_root->get_hash().bits();
    virt_root = vstate;
    return td::Status::OK();
  } catch (vm::VmError& err) {
    return err.as_status("vm error:");
  } catch (vm::VmVirtError& err) {
    return err.as_status("virtualization error:");
  }
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler();

  td::actor::ActorOwn<TestNode> x;

  td::OptionParser p;
  p.set_description("Test Lite Client for TON Blockchain");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('C', "global-config", "file to read global config",
               [&](td::Slice fname) { td::actor::send_closure(x, &TestNode::set_global_config, fname.str()); });
  p.add_option('r', "disable-readline", "",
               [&]() { td::actor::send_closure(x, &TestNode::set_readline_enabled, false); });
  p.add_option('R', "enable-readline", "",
               [&]() { td::actor::send_closure(x, &TestNode::set_readline_enabled, true); });
  p.add_option('D', "db", "root for dbs",
               [&](td::Slice fname) { td::actor::send_closure(x, &TestNode::set_db_root, fname.str()); });
  p.add_checked_option('L', "print-limit", "sets maximum count of recursively printed objects", [&](td::Slice arg) {
    auto plimit = td::to_integer<int>(arg);
    td::actor::send_closure(x, &TestNode::set_print_limit, plimit);
    return plimit >= 0 ? td::Status::OK() : td::Status::Error("printing limit must be non-negative");
  });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 9) ? td::Status::OK() : td::Status::Error("verbosity must be 0..9");
  });
  p.add_option('V', "version", "shows lite-client build information", [&]() {
    std::cout << "lite-client build information: [ Commit: " << GitMetadata::CommitSHA1() << ", Date: " << GitMetadata::CommitDate() << "]\n";

    std::exit(0);
  });
  p.add_option('i', "idx", "set liteserver idx", [&](td::Slice arg) {
    auto idx = td::to_integer<int>(arg);
    td::actor::send_closure(x, &TestNode::set_liteserver_idx, idx);
  });
  p.add_checked_option('a', "addr", "connect to ip:port", [&](td::Slice arg) {
    td::IPAddress addr;
    TRY_STATUS(addr.init_host_port(arg.str()));
    td::actor::send_closure(x, &TestNode::set_remote_addr, addr);
    return td::Status::OK();
  });
  p.add_option('c', "cmd", "schedule command",
               [&](td::Slice arg) { td::actor::send_closure(x, &TestNode::add_cmd, td::BufferSlice{arg}); });
  p.add_option('t', "timeout", "timeout in batch mode", [&](td::Slice arg) {
    auto d = td::to_double(arg);
    td::actor::send_closure(x, &TestNode::set_fail_timeout, td::Timestamp::in(d));
  });
  p.add_option('p', "pub", "remote public key",
               [&](td::Slice arg) { td::actor::send_closure(x, &TestNode::set_public_key, td::BufferSlice{arg}); });
  p.add_option('b', "b64", "remote public key as base64",
               [&](td::Slice arg) { td::actor::send_closure(x, &TestNode::decode_public_key, td::BufferSlice{arg}); });
  p.add_option('d', "daemonize", "set SIGHUP", [&]() {
    td::set_signal_handler(td::SignalType::HangUp, [](int sig) {
#if TD_DARWIN || TD_LINUX
      close(0);
      setsid();
#endif
    }).ensure();
  });
#if TD_DARWIN || TD_LINUX
  p.add_option('l', "logname", "log to file", [&](td::Slice fname) {
    auto FileLog = td::FileFd::open(td::CSlice(fname.str().c_str()),
                                    td::FileFd::Flags::Create | td::FileFd::Flags::Append | td::FileFd::Flags::Write)
                       .move_as_ok();

    dup2(FileLog.get_native_fd().fd(), 1);
    dup2(FileLog.get_native_fd().fd(), 2);
  });
#endif

  vm::init_vm(true).ensure();  // enable vm debug

  td::actor::Scheduler scheduler({2});

  scheduler.run_in_context([&] { x = td::actor::create_actor<TestNode>("testnode"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] {
    td::actor::send_closure(x, &TestNode::run);
    x.release();
  });
  scheduler.run();

  return 0;
}
