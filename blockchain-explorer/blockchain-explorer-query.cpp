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
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "blockchain-explorer-query.hpp"
#include "blockchain-explorer-http.hpp"
#include "block/mc-config.h"
#include "crypto/block/check-proof.h"

#include "auto/tl/lite_api.h"

#include "tl-utils/tl-utils.hpp"
#include "tl-utils/lite-utils.hpp"

#include "ton/ton-tl.hpp"
#include "ton/lite-tl.hpp"

#include "common/errorcode.h"
#include "block/block-auto.h"
#include "crypto/vm/utils.h"
#include "td/utils/crypto.h"

#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/continuation.h"
#include "vm/cp0.h"

namespace {

td::Ref<vm::Tuple> prepare_vm_c7(ton::UnixTime now, ton::LogicalTime lt, td::Ref<vm::CellSlice> my_addr,
                                 const block::CurrencyCollection &balance) {
  td::BitArray<256> rand_seed;
  td::RefInt256 rand_seed_int{true};
  td::Random::secure_bytes(rand_seed.as_slice());
  if (!rand_seed_int.unique_write().import_bits(rand_seed.cbits(), 256, false)) {
    return {};
  }
  auto tuple = vm::make_tuple_ref(td::make_refint(0x076ef1ea),  // [ magic:0x076ef1ea
                                  td::make_refint(0),           //   actions:Integer
                                  td::make_refint(0),           //   msgs_sent:Integer
                                  td::make_refint(now),         //   unixtime:Integer
                                  td::make_refint(lt),          //   block_lt:Integer
                                  td::make_refint(lt),          //   trans_lt:Integer
                                  std::move(rand_seed_int),     //   rand_seed:Integer
                                  balance.as_vm_tuple(),        //   balance_remaining:[Integer (Maybe Cell)]
                                  my_addr,                      //  myself:MsgAddressInt
                                  vm::StackEntry());            //  global_config:(Maybe Cell) ] = SmartContractInfo;
  LOG(DEBUG) << "SmartContractInfo initialized with " << vm::StackEntry(tuple).to_string();
  return vm::make_tuple_ref(std::move(tuple));
}

}  // namespace

td::Result<ton::BlockIdExt> parse_block_id(std::map<std::string, std::string> &opts, bool allow_empty) {
  if (allow_empty) {
    if (opts.count("workchain") == 0 && opts.count("shard") == 0 && opts.count("seqno") == 0) {
      return ton::BlockIdExt{};
    }
  }
  try {
    ton::BlockIdExt block_id;
    auto it = opts.find("workchain");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "workchain not set");
    }
    block_id.id.workchain = std::stoi(it->second);
    it = opts.find("shard");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "shard not set");
    }
    block_id.id.shard = std::stoull(it->second, nullptr, 16);
    it = opts.find("seqno");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "seqno not set");
    }
    auto s = std::stoull(it->second);
    auto seqno = static_cast<ton::BlockSeqno>(s);
    if (s != seqno) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "seqno too big");
    }
    block_id.id.seqno = seqno;
    it = opts.find("roothash");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "roothash not set");
    }
    if (it->second.length() != 64) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "roothash bad length");
    }
    auto R = td::hex_decode(td::Slice(it->second));
    if (R.is_error()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "roothash bad hex");
    }
    block_id.root_hash.as_slice().copy_from(td::as_slice(R.move_as_ok()));
    it = opts.find("filehash");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "filehash not set");
    }
    if (it->second.length() != 64) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "filehash bad length");
    }
    R = td::hex_decode(td::Slice(it->second));
    if (R.is_error()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "filehash bad hex");
    }
    block_id.file_hash.as_slice().copy_from(td::as_slice(R.move_as_ok()));
    return block_id;
  } catch (...) {
    return td::Status::Error(ton::ErrorCode::protoviolation, "cannot parse int");
  }
}

td::Result<ton::AccountIdPrefixFull> parse_account_prefix(std::map<std::string, std::string> &opts, bool allow_empty) {
  if (allow_empty) {
    if (opts.count("workchain") == 0 && opts.count("shard") == 0 && opts.count("account") == 0) {
      return ton::AccountIdPrefixFull{ton::masterchainId, 0};
    }
  }
  try {
    ton::AccountIdPrefixFull account_id;
    auto it = opts.find("workchain");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "workchain not set");
    }
    account_id.workchain = std::stoi(it->second);
    it = opts.find("shard");
    if (it == opts.end()) {
      it = opts.find("account");
      if (it == opts.end()) {
        return td::Status::Error(ton::ErrorCode::protoviolation, "shard/account not set");
      }
    }
    account_id.account_id_prefix = std::stoull(it->second, nullptr, 16);
    return account_id;
  } catch (...) {
    return td::Status::Error(ton::ErrorCode::protoviolation, "cannot parse int");
  }
}

td::Result<block::StdAddress> parse_account_addr(std::map<std::string, std::string> &opts) {
  auto it = opts.find("account");
  if (it == opts.end()) {
    return td::Status::Error(ton::ErrorCode::error, "no account id");
  }
  std::string acc_string = it->second;
  block::StdAddress a;
  if (a.parse_addr(td::Slice(acc_string))) {
    return a;
  }
  ton::WorkchainId workchain_id;
  it = opts.find("accountworkchain");
  if (it == opts.end()) {
    it = opts.find("workchain");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::error, "no account workchain id");
    }
  }
  try {
    workchain_id = std::stoi(it->second);
  } catch (...) {
    return td::Status::Error(ton::ErrorCode::error, "bad account workchain id");
  }
  if (acc_string.size() == 64) {
    TRY_RESULT(R, td::hex_decode(acc_string));
    a.addr.as_slice().copy_from(td::Slice(R));
    a.workchain = workchain_id;
    return a;
  }
  return td::Status::Error(ton::ErrorCode::error, "bad account id");
}

void HttpQueryCommon::abort_query(td::Status error) {
  if (promise_) {
    HttpAnswer A{"error", prefix_};
    A.abort(std::move(error));
    auto page = A.finish();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQueryBlockData::HttpQueryBlockData(ton::BlockIdExt block_id, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), block_id_(block_id) {
}

HttpQueryBlockData::HttpQueryBlockData(std::map<std::string, std::string> opts, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
  } else {
    error_ = R.move_as_error();
  }
}

void HttpQueryBlockData::abort_query(td::Status error) {
  if (promise_) {
    promise_.set_result(nullptr);
  }
  stop();
}

void HttpQueryBlockData::finish_query() {
  if (promise_) {
    auto response = MHD_create_response_from_buffer(data_.length(), data_.as_slice().begin(), MHD_RESPMEM_MUST_COPY);
    promise_.set_result(response);
  }
  stop();
}

void HttpQueryBlockData::start_up() {
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(block_id_)), true);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockData::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockData::got_block_data, R.move_as_ok());
    }
  });

  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryBlockData::got_block_data(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  data_ = std::move(F.move_as_ok()->data_);
  finish_query();
}

HttpQueryBlockView::HttpQueryBlockView(ton::BlockIdExt block_id, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), block_id_(block_id) {
}

HttpQueryBlockView::HttpQueryBlockView(std::map<std::string, std::string> opts, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
  } else {
    error_ = R.move_as_error();
  }
}

void HttpQueryBlockView::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"viewblock", prefix_};
      A.set_block_id(block_id_);
      auto res = vm::std_boc_deserialize(data_.clone());
      if (res.is_error()) {
        return A.abort(PSTRING() << "cannot deserialize block: " << res.move_as_error());
      }
      create_header(A);
      auto root = res.move_as_ok();
      A << HttpAnswer::RawData<block::gen::Block>{root};
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

void HttpQueryBlockView::start_up_query() {
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(block_id_)), true);

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockView::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockView::got_block_data, R.move_as_ok());
    }
  });

  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryBlockView::got_block_data(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
  }
  data_ = std::move(F.move_as_ok()->data_);
  finish_query();
}

HttpQueryBlockInfo::HttpQueryBlockInfo(ton::BlockIdExt block_id, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), block_id_(block_id) {
}

HttpQueryBlockInfo::HttpQueryBlockInfo(std::map<std::string, std::string> opts, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
  } else {
    error_ = R.move_as_error();
  }
}

void HttpQueryBlockInfo::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_block_header, R.move_as_ok());
    }
  });
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlockHeader>(ton::create_tl_lite_block_id(block_id_), 0),
      true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
  pending_queries_ = 1;

  if (block_id_.is_masterchain()) {
    auto P_2 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::failed_to_get_shard_info,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_shard_info, R.move_as_ok());
      }
    });
    auto query_2 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(block_id_)),
        true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_2), std::move(P_2));
    pending_queries_++;
  }
  auto query_3 = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
                                              ton::create_tl_lite_block_id(block_id_), 7, 1024, nullptr, false, false),
                                          true);
  auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_transactions, R.move_as_ok());
    }
  });
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query_3), std::move(P_3));
  pending_queries_++;
}

void HttpQueryBlockInfo::got_block_header(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  data_ = std::move(F.move_as_ok()->header_proof_);

  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockInfo::got_shard_info(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  shard_data_ = std::move(F.move_as_ok()->data_);

  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockInfo::failed_to_get_shard_info(td::Status error) {
  shard_data_error_ = std::move(error);
  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockInfo::got_transactions(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactions>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  trans_req_count_ = f->req_count_;

  for (auto &T : f->ids_) {
    transactions_.emplace_back(block::StdAddress{block_id_.id.workchain, T->account_},
                               static_cast<ton::LogicalTime>(T->lt_), T->hash_);
  }

  if (f->incomplete_ && transactions_.size() > 0) {
    const auto &T = *transactions_.rbegin();
    auto query_3 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
            ton::create_tl_lite_block_id(block_id_), 7 + 128, 1024,
            ton::create_tl_object<ton::lite_api::liteServer_transactionId3>(T.addr.addr, T.lt), false, false),
        true);
    auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_transactions, R.move_as_ok());
      }
    });
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_3), std::move(P_3));
  } else {
    if (!--pending_queries_) {
      finish_query();
    }
  }
}

void HttpQueryBlockInfo::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"blockinfo", prefix_};
      A.set_block_id(block_id_);
      create_header(A);
      auto res = vm::std_boc_deserialize(data_.clone());
      if (res.is_error()) {
        return A.abort(PSTRING() << "cannot deserialize block header data: " << res.move_as_error());
      }
      A << HttpAnswer::BlockHeaderCell{block_id_, res.move_as_ok()};

      if (shard_data_.size() > 0) {
        auto R = vm::std_boc_deserialize(shard_data_.clone());
        if (R.is_error()) {
          return A.abort(PSTRING() << "cannot deserialize shard configuration: " << R.move_as_error());
        } else {
          A << HttpAnswer::BlockShardsCell{block_id_, R.move_as_ok()};
        }
      }
      if (shard_data_error_.is_error()) {
        A << HttpAnswer::Error{shard_data_error_.clone()};
      }

      HttpAnswer::TransactionList I;
      I.block_id = block_id_;
      I.req_count_ = trans_req_count_;
      for (auto &T : transactions_) {
        I.vec.emplace_back(T.addr, T.lt, T.hash);
      }
      A << I;

      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQueryBlockSearch::HttpQueryBlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account,
                                           ton::BlockSeqno seqno, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise))
    , account_prefix_{workchain, account}
    , mode_(1)
    , seqno_(seqno) {
}
HttpQueryBlockSearch::HttpQueryBlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account,
                                           ton::LogicalTime lt, std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), account_prefix_{workchain, account}, mode_(2), lt_(lt) {
}
HttpQueryBlockSearch::HttpQueryBlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, bool dummy,
                                           ton::UnixTime utime, std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise))
    , account_prefix_{workchain, account}
    , mode_(4)
    , utime_(utime) {
}

HttpQueryBlockSearch::HttpQueryBlockSearch(std::map<std::string, std::string> opts, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R2 = parse_account_prefix(opts, false);
  if (R2.is_ok()) {
    account_prefix_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  if (opts.count("seqno") + opts.count("lt") + opts.count("utime") != 1) {
    error_ = td::Status::Error(ton::ErrorCode::protoviolation, "exactly one of seqno/lt/utime must be set");
    return;
  }
  if (opts.count("seqno") == 1) {
    try {
      seqno_ = static_cast<td::uint32>(std::stoull(opts["seqno"]));
      mode_ = 1;
    } catch (...) {
      error_ = td::Status::Error("cannot parse seqno");
      return;
    }
  }
  if (opts.count("lt") == 1) {
    try {
      lt_ = std::stoull(opts["lt"]);
      mode_ = 2;
    } catch (...) {
      error_ = td::Status::Error("cannot parse lt");
      return;
    }
  }
  if (opts.count("utime") == 1) {
    try {
      seqno_ = static_cast<td::uint32>(std::stoull(opts["utime"]));
      mode_ = 1;
    } catch (...) {
      error_ = td::Status::Error("cannot parse utime");
      return;
    }
  }
}

void HttpQueryBlockSearch::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockSearch::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockSearch::got_block_header, R.move_as_ok());
    }
  });
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_lookupBlock>(
                                            mode_,
                                            ton::create_tl_lite_block_id_simple(ton::BlockId{
                                                account_prefix_.workchain, account_prefix_.account_id_prefix, seqno_}),
                                            lt_, utime_),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryBlockSearch::got_block_header(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  data_ = std::move(f->header_proof_);
  block_id_ = ton::create_block_id(f->id_);

  if (block_id_.is_masterchain()) {
    auto P_2 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockSearch::failed_to_get_shard_info,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockSearch::got_shard_info, R.move_as_ok());
      }
    });
    auto query_2 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(block_id_)),
        true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_2), std::move(P_2));
    pending_queries_++;
  }

  auto query_3 = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
                                              ton::create_tl_lite_block_id(block_id_), 7, 1024, nullptr, false, false),
                                          true);
  auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockSearch::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockSearch::got_transactions, R.move_as_ok());
    }
  });
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query_3), std::move(P_3));
  pending_queries_++;
}

void HttpQueryBlockSearch::got_shard_info(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  shard_data_ = std::move(F.move_as_ok()->data_);

  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockSearch::failed_to_get_shard_info(td::Status error) {
  shard_data_error_ = std::move(error);
  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockSearch::got_transactions(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactions>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  trans_req_count_ = f->req_count_;

  for (auto &T : f->ids_) {
    transactions_.emplace_back(block::StdAddress{block_id_.id.workchain, T->account_},
                               static_cast<ton::LogicalTime>(T->lt_), T->hash_);
  }

  if (f->incomplete_ && transactions_.size() > 0) {
    const auto &T = *transactions_.rbegin();
    auto query_3 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
            ton::create_tl_lite_block_id(block_id_), 7 + 128, 1024,
            ton::create_tl_object<ton::lite_api::liteServer_transactionId3>(T.addr.addr, T.lt), false, false),
        true);
    auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockSearch::abort_query,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockSearch::got_transactions, R.move_as_ok());
      }
    });
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_3), std::move(P_3));
  } else {
    if (!--pending_queries_) {
      finish_query();
    }
  }
}

void HttpQueryBlockSearch::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"blockinfo", prefix_};
      A.set_block_id(block_id_);
      create_header(A);
      auto res = vm::std_boc_deserialize(data_.clone());
      if (res.is_error()) {
        return A.abort(PSTRING() << "cannot deserialize block header data: " << res.move_as_error());
      }
      A << HttpAnswer::BlockHeaderCell{block_id_, res.move_as_ok()};

      if (shard_data_.size() > 0) {
        auto R = vm::std_boc_deserialize(shard_data_.clone());
        if (R.is_error()) {
          return A.abort(PSTRING() << "cannot deserialize shard configuration: " << R.move_as_error());
        } else {
          A << HttpAnswer::BlockShardsCell{block_id_, R.move_as_ok()};
        }
      }
      if (shard_data_error_.is_error()) {
        A << HttpAnswer::Error{shard_data_error_.clone()};
      }

      HttpAnswer::TransactionList I;
      I.block_id = block_id_;
      I.req_count_ = trans_req_count_;
      for (auto &T : transactions_) {
        I.vec.emplace_back(T.addr, T.lt, T.hash);
      }
      A << I;

      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}
HttpQueryViewAccount::HttpQueryViewAccount(ton::BlockIdExt block_id, block::StdAddress addr, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), block_id_(block_id), addr_(addr) {
}

HttpQueryViewAccount::HttpQueryViewAccount(std::map<std::string, std::string> opts, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts, true);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
    if (!block_id_.is_valid()) {
      block_id_.id.workchain = ton::masterchainId;
      block_id_.id.shard = ton::shardIdAll;
      block_id_.id.seqno = static_cast<td::uint32>(0xffffffff);
      block_id_.root_hash.set_zero();
      block_id_.file_hash.set_zero();
    }
  } else {
    error_ = R.move_as_error();
    return;
  }
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
}

void HttpQueryViewAccount::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryViewAccount::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryViewAccount::got_account, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
                                            ton::create_tl_lite_block_id(block_id_), std::move(a)),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryViewAccount::got_account(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->state_);
  proof_ = std::move(f->proof_);
  res_block_id_ = ton::create_block_id(f->shardblk_);

  finish_query();
}

void HttpQueryViewAccount::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"account", prefix_};
      A.set_account_id(addr_);
      A.set_block_id(res_block_id_);
      auto R = vm::std_boc_deserialize(data_.clone());
      if (R.is_error()) {
        return A.abort(PSTRING() << "FATAL: cannot deserialize account state" << R.move_as_error());
      }
      auto Q = vm::std_boc_deserialize_multi(proof_.clone());
      if (Q.is_error()) {
        return A.abort(PSTRING() << "FATAL: cannot deserialize account proof" << Q.move_as_error());
      }
      auto Q_roots = Q.move_as_ok();
      auto root = R.move_as_ok();
      A << HttpAnswer::AccountCell{addr_, res_block_id_, root, Q_roots};
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQueryViewTransaction::HttpQueryViewTransaction(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash,
                                                   std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), addr_(addr), lt_(lt), hash_(hash) {
}

HttpQueryViewTransaction::HttpQueryViewTransaction(std::map<std::string, std::string> opts, std::string prefix,
                                                   td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  try {
    lt_ = std::stoull(opts["lt"]);
  } catch (...) {
    error_ = td::Status::Error("cannot trans parse lt");
    return;
  }
  try {
    auto h = opts["hash"];
    if (h.length() != 64) {
      error_ = td::Status::Error("cannot trans parse hash");
      return;
    }
    auto R = td::hex_decode(td::Slice(h));
    if (R.is_error()) {
      error_ = td::Status::Error("cannot trans parse hash");
      return;
    }
    hash_.as_slice().copy_from(R.move_as_ok());
  } catch (...) {
    error_ = td::Status::Error("cannot trans parse hash");
    return;
  }
}

void HttpQueryViewTransaction::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryViewTransaction::abort_query,
                              R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryViewTransaction::got_transaction, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(1, std::move(a), lt_, hash_), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryViewTransaction::got_transaction(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_transactionList>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->transactions_);
  if (f->ids_.size() == 0) {
    abort_query(td::Status::Error("no transactions found"));
    return;
  }
  res_block_id_ = ton::create_block_id(f->ids_[0]);

  finish_query();
}

void HttpQueryViewTransaction::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"transaction", prefix_};
      A.set_block_id(res_block_id_);
      A.set_account_id(addr_);
      auto R = vm::std_boc_deserialize_multi(std::move(data_));
      if (R.is_error()) {
        return A.abort(PSTRING() << "FATAL: cannot deserialize transactions BoC");
      }
      auto list = R.move_as_ok();
      auto n = list.size();
      if (n != 1) {
        return A.abort(PSTRING() << "obtained " << n << " transaction, but only 1 have been requested");
      } else {
        A << HttpAnswer::TransactionCell{addr_, res_block_id_, list[0]};
      }
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQueryViewTransaction2::HttpQueryViewTransaction2(ton::BlockIdExt block_id, block::StdAddress addr,
                                                     ton::LogicalTime lt, std::string prefix,
                                                     td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), block_id_(block_id), addr_(addr), lt_(lt) {
}

HttpQueryViewTransaction2::HttpQueryViewTransaction2(std::map<std::string, std::string> opts, std::string prefix,
                                                     td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
  } else {
    error_ = R.move_as_error();
    return;
  }
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  try {
    lt_ = std::stoull(opts["lt"]);
  } catch (...) {
    error_ = td::Status::Error("cannot trans parse lt");
    return;
  }
}

void HttpQueryViewTransaction2::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryViewTransaction2::abort_query,
                              R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryViewTransaction2::got_transaction, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getOneTransaction>(
                                            ton::create_tl_lite_block_id(block_id_), std::move(a), lt_),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryViewTransaction2::got_transaction(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_transactionInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->transaction_);

  finish_query();
}

void HttpQueryViewTransaction2::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"transaction", prefix_};
      A.set_block_id(block_id_);
      A.set_account_id(addr_);
      auto R = vm::std_boc_deserialize(std::move(data_));
      if (R.is_error()) {
        return A.abort(PSTRING() << "FATAL: cannot deserialize transactions BoC");
      }
      auto list = R.move_as_ok();
      A << HttpAnswer::TransactionCell{addr_, block_id_, list};
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQueryViewLastBlock::HttpQueryViewLastBlock(std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

HttpQueryViewLastBlock::HttpQueryViewLastBlock(std::map<std::string, std::string> opts, std::string prefix,
                                               td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

void HttpQueryViewLastBlock::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryViewLastBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &HttpQueryViewLastBlock::got_result, R.move_as_ok());
    }
  });

  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryViewLastBlock::got_result(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  res_block_id_ = ton::create_block_id(f->last_);

  finish_query();
}

void HttpQueryViewLastBlock::finish_query() {
  if (promise_) {
    td::actor::create_actor<HttpQueryBlockInfo>("blockinfo", res_block_id_, prefix_, std::move(promise_)).release();
  }
  stop();
}

HttpQueryConfig::HttpQueryConfig(std::string prefix, ton::BlockIdExt block_id, std::vector<td::int32> params,
                                 td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)), block_id_(block_id), params_(std::move(params)) {
}

HttpQueryConfig::HttpQueryConfig(std::map<std::string, std::string> opts, std::string prefix,
                                 td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)) {
  auto R = parse_block_id(opts, true);
  if (R.is_error()) {
    error_ = R.move_as_error();
    return;
  }
  block_id_ = R.move_as_ok();

  auto it = opts.find("param");
  if (it != opts.end()) {
    auto R2 = td::to_integer_safe<int>(it->second);
    if (R2.is_error()) {
      error_ = R2.move_as_error();
      return;
    }
    params_.push_back(R2.move_as_ok());
  }
}

void HttpQueryConfig::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }
  if (block_id_.is_valid()) {
    send_main_query();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryConfig::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &HttpQueryConfig::got_block, R.move_as_ok());
      }
    });

    auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query), std::move(P));
  }
}

void HttpQueryConfig::got_block(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  block_id_ = ton::create_block_id(f->last_);

  send_main_query();
}

void HttpQueryConfig::send_main_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryConfig::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &HttpQueryConfig::got_result, R.move_as_ok());
    }
  });
  auto query =
      params_.size() > 0
          ? ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getConfigParams>(
                                         0, ton::create_tl_lite_block_id(block_id_), std::vector<int>(params_)),
                                     true)
          : ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getConfigAll>(
                                         0, ton::create_tl_lite_block_id(block_id_)),
                                     true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryConfig::got_result(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_configInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();

  state_proof_ = std::move(f->state_proof_);
  config_proof_ = std::move(f->config_proof_);

  finish_query();
}

void HttpQueryConfig::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"config", prefix_};
      A.set_block_id(block_id_);
      auto R = block::check_extract_state_proof(block_id_, state_proof_.as_slice(), config_proof_.as_slice());
      if (R.is_error()) {
        A.abort(PSTRING() << "masterchain state proof for " << block_id_.to_str()
                          << " is invalid : " << R.move_as_error());
        return A.finish();
      }
      try {
        auto res = block::Config::extract_from_state(R.move_as_ok(), 0);
        if (res.is_error()) {
          A.abort(PSTRING() << "cannot unpack configuration: " << res.move_as_error());
          return A.finish();
        }
        auto config = res.move_as_ok();
        if (params_.size() > 0) {
          A << "<p>params: ";
          for (int i : params_) {
            auto value = config->get_config_param(i);
            if (value.not_null()) {
              A << "<a href=\"#configparam" << i << "\">" << i << "</a> ";
            }
          }
          A << "</p>";
          for (int i : params_) {
            auto value = config->get_config_param(i);
            if (value.not_null()) {
              A << HttpAnswer::ConfigParam{i, value};
            } else {
              A << HttpAnswer::Error{td::Status::Error(404, PSTRING() << "empty param " << i)};
            }
          }
        } else {
          A << "<p>params: ";
          config->foreach_config_param([&](int i, td::Ref<vm::Cell> value) {
            if (value.not_null()) {
              A << "<a href=\"#configparam" << i << "\">" << i << "</a> ";
            }
            return true;
          });
          A << "</p>";
          config->foreach_config_param([&](int i, td::Ref<vm::Cell> value) {
            if (value.not_null()) {
              A << HttpAnswer::ConfigParam{i, value};
            }
            return true;
          });
        }
      } catch (vm::VmError &err) {
        A.abort(PSTRING() << "error while traversing configuration: " << err.get_msg());
      } catch (vm::VmVirtError &err) {
        A.abort(PSTRING() << "virtualization error while traversing configuration: " << err.get_msg());
      }
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQuerySendForm::HttpQuerySendForm(std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)) {
}

HttpQuerySendForm::HttpQuerySendForm(std::map<std::string, std::string> opts, std::string prefix,
                                     td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)) {
}

void HttpQuerySendForm::start_up() {
  finish_query();
}

void HttpQuerySendForm::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"send", prefix_};
      A << "<div class=\"row\"><form action=\"" << prefix_
        << "send\" method=\"post\" enctype=\"multipart/form-data\"><div class=\"form-group-row\">"
        << "<label for=\"filedata\">bag of cells</label>"
        << "<input type=\"file\" class=\"form-control-file\" id=\"filedata\" name=\"filedata\">"
        << "<button type=\"submit\" class=\"btn btn-primary\">send</button>"
        << "</div></form></div>";
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQuerySend::HttpQuerySend(std::string prefix, td::BufferSlice data, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)), data_(std::move(data)) {
}

HttpQuerySend::HttpQuerySend(std::map<std::string, std::string> opts, std::string prefix,
                             td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)) {
  auto it = opts.find("filedata");
  if (it != opts.end()) {
    data_ = td::BufferSlice{it->second};
  } else {
    error_ = td::Status::Error("no file data");
    return;
  }
}

void HttpQuerySend::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQuerySend::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &HttpQuerySend::got_result, R.move_as_ok());
    }
  });
  auto query =
      ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_sendMessage>(std::move(data_)), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQuerySend::got_result(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_sendMsgStatus>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
  } else {
    status_ = F.move_as_ok()->status_;
  }
  finish_query();
}

void HttpQuerySend::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"send", prefix_};
      if (status_ >= 0) {
        A << HttpAnswer::Notification{"success"};
      } else {
        A << HttpAnswer::Error{td::Status::Error(status_, "failed")};
      }
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}

HttpQueryRunMethod::HttpQueryRunMethod(ton::BlockIdExt block_id, block::StdAddress addr, std::string method_name,
                                       std::vector<vm::StackEntry> params, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise))
    , block_id_(block_id)
    , addr_(addr)
    , method_name_(std::move(method_name))
    , params_(std::move(params)) {
}

HttpQueryRunMethod::HttpQueryRunMethod(std::map<std::string, std::string> opts, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts, true);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
    if (!block_id_.is_valid()) {
      block_id_.id.workchain = ton::masterchainId;
      block_id_.id.shard = ton::shardIdAll;
      block_id_.id.seqno = static_cast<td::uint32>(0xffffffff);
      block_id_.root_hash.set_zero();
      block_id_.file_hash.set_zero();
    }
  } else {
    error_ = R.move_as_error();
    return;
  }
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  auto it = opts.find("method");
  if (it == opts.end()) {
    error_ = td::Status::Error("no method");
    return;
  } else {
    method_name_ = it->second;
  }
  it = opts.find("params");
  if (it != opts.end()) {
    auto R3 = vm::parse_stack_entries(it->second);
    if (R3.is_error()) {
      error_ = R3.move_as_error();
      return;
    }
    params_ = R3.move_as_ok();
  }
}

void HttpQueryRunMethod::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryRunMethod::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryRunMethod::got_account, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
                                            ton::create_tl_lite_block_id(block_id_), std::move(a)),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryRunMethod::got_account(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->state_);
  proof_ = std::move(f->proof_);
  shard_proof_ = std::move(f->shard_proof_);
  block_id_ = ton::create_block_id(f->id_);
  res_block_id_ = ton::create_block_id(f->shardblk_);

  finish_query();
}

void HttpQueryRunMethod::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"account", prefix_};
      A.set_account_id(addr_);
      A.set_block_id(res_block_id_);

      block::AccountState account_state;
      account_state.blk = block_id_;
      account_state.shard_blk = res_block_id_;
      account_state.shard_proof = std::move(shard_proof_);
      account_state.proof = std::move(proof_);
      account_state.state = std::move(data_);
      auto r_info = account_state.validate(block_id_, addr_);
      if (r_info.is_error()) {
        A.abort(r_info.move_as_error());
        return A.finish();
      }
      auto info = r_info.move_as_ok();
      if (info.root.is_null()) {
        A.abort(PSTRING() << "account state of " << addr_ << " is empty (cannot run method `" << method_name_ << "`)");
        return A.finish();
      }
      block::gen::Account::Record_account acc;
      block::gen::AccountStorage::Record store;
      block::CurrencyCollection balance;
      if (!(tlb::unpack_cell(info.root, acc) && tlb::csr_unpack(acc.storage, store) &&
            balance.validate_unpack(store.balance))) {
        A.abort("error unpacking account state");
        return A.finish();
      }
      int tag = block::gen::t_AccountState.get_tag(*store.state);
      switch (tag) {
        case block::gen::AccountState::account_uninit:
          A.abort(PSTRING() << "account " << addr_ << " not initialized yet (cannot run any methods)");
          return A.finish();
        case block::gen::AccountState::account_frozen:
          A.abort(PSTRING() << "account " << addr_ << " frozen (cannot run any methods)");
          return A.finish();
      }

      CHECK(store.state.write().fetch_ulong(1) == 1);  // account_init$1 _:StateInit = AccountState;
      block::gen::StateInit::Record state_init;
      CHECK(tlb::csr_unpack(store.state, state_init));
      auto code = state_init.code->prefetch_ref();
      auto data = state_init.data->prefetch_ref();
      auto stack = td::make_ref<vm::Stack>(std::move(params_));
      td::int64 method_id = (td::crc16(td::Slice{method_name_}) & 0xffff) | 0x10000;
      stack.write().push_smallint(method_id);
      long long gas_limit = vm::GasLimits::infty;
      // OstreamLogger ostream_logger(ctx.error_stream);
      // auto log = create_vm_log(ctx.error_stream ? &ostream_logger : nullptr);
      vm::GasLimits gas{gas_limit};
      LOG(DEBUG) << "creating VM";
      vm::VmState vm{code, std::move(stack), gas, 1, data, vm::VmLog()};
      vm.set_c7(prepare_vm_c7(info.gen_utime, info.gen_lt, acc.addr, balance));  // tuple with SmartContractInfo
      // vm.incr_stack_trace(1);    // enable stack dump after each step
      int exit_code = ~vm.run();
      if (exit_code != 0) {
        A.abort(PSTRING() << "VM terminated with error code " << exit_code);
        return A.finish();
      }
      stack = vm.get_stack_ref();
      {
        std::ostringstream os;
        os << "result: ";
        stack->dump(os, 3);

        A << HttpAnswer::CodeBlock{os.str()};
      }

      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}
HttpQueryStatus::HttpQueryStatus(std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

HttpQueryStatus::HttpQueryStatus(std::map<std::string, std::string> opts, std::string prefix,
                                 td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

void HttpQueryStatus::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }

  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<CoreActorInterface::RemoteNodeStatusList> R) {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &HttpQueryStatus::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &HttpQueryStatus::got_results, R.move_as_ok());
        }
      });
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::get_results, 60, std::move(P));
}

void HttpQueryStatus::got_results(CoreActorInterface::RemoteNodeStatusList results) {
  results_ = std::move(results);

  finish_query();
}

void HttpQueryStatus::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      std::map<td::uint32, std::set<td::uint32>> m;

      HttpAnswer A{"status", prefix_};
      A << "<div class=\"table-responsive my-3\">\n"
        << "<table class=\"table-sm\">\n"
        << "<tr><td>ip</td>";
      for (auto &x : results_.results) {
        A << "<td>" << static_cast<td::int32>(x->ts_.at_unix()) << "</td>";
      }
      A << "</tr>\n";
      for (td::uint32 i = 0; i < results_.ips.size(); i++) {
        A << "<tr>";
        if (results_.ips[i].is_valid()) {
          A << "<td>" << results_.ips[i] << "</td>";
        } else {
          A << "<td>hidden</td>";
        }
        td::uint32 j = 0;
        for (auto &X : results_.results) {
          if (!X->values_[i].is_valid()) {
            A << "<td>FAIL</td>";
          } else {
            if (m[j].count(X->values_[i].id.seqno) == 0) {
              m[j].insert(X->values_[i].id.seqno);
              A << "<td><a href=\"" << HttpAnswer::BlockLink{X->values_[i]} << "\">" << X->values_[i].id.seqno
                << "</a></td>";
            } else {
              A << "<td>" << X->values_[i].id.seqno << "</td>";
            }
          }
          j++;
        }
        A << "</tr>\n";
      }
      A << "</table></div>";
      return A.finish();
    }();
    promise_.set_value(
        MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY));
  }
  stop();
}
