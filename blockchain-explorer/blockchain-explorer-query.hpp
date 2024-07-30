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

#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "block/block.h"
#include "blockchain-explorer.hpp"

#include <map>

#include <microhttpd.h>

td::Result<ton::BlockIdExt> parse_block_id(std::map<std::string, std::string> &opts, bool allow_empty = false);
td::Result<block::StdAddress> parse_account_addr(std::map<std::string, std::string> &opts);

class HttpAnswer;

class HttpQueryCommon : public td::actor::Actor {
 public:
  HttpQueryCommon(std::string prefix, td::Promise<MHD_Response *> promise)
      : prefix_(std::move(prefix)), promise_(std::move(promise)) {
  }
  void start_up() override {
    if (error_.is_error()) {
      abort_query(std::move(error_));
      return;
    }
    start_up_query();
  }
  virtual void start_up_query() {
    UNREACHABLE();
  }
  virtual void abort_query(td::Status error);
  void create_header(HttpAnswer &ans) {
  }

 protected:
  td::Status error_;

  std::string prefix_;
  td::Promise<MHD_Response *> promise_;
};

class HttpQueryBlockData : public HttpQueryCommon {
 public:
  HttpQueryBlockData(ton::BlockIdExt block_id, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockData(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void abort_query(td::Status error) override;
  void finish_query();

  void start_up() override;
  void got_block_data(td::BufferSlice result);

 private:
  ton::BlockIdExt block_id_;

  td::BufferSlice data_;
};

class HttpQueryBlockView : public HttpQueryCommon {
 public:
  HttpQueryBlockView(ton::BlockIdExt block_id, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockView(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_block_data(td::BufferSlice result);

 private:
  ton::BlockIdExt block_id_;

  td::BufferSlice data_;
};

class HttpQueryBlockInfo : public HttpQueryCommon {
 public:
  HttpQueryBlockInfo(ton::BlockIdExt block_id, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockInfo(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_block_header(td::BufferSlice result);
  void got_shard_info(td::BufferSlice result);
  void got_transactions(td::BufferSlice result);

  void failed_to_get_shard_info(td::Status error);

 private:
  ton::BlockIdExt block_id_;

  td::int32 pending_queries_ = 0;

  td::BufferSlice data_;
  td::BufferSlice shard_data_;
  td::Status shard_data_error_;

  struct TransactionDescr {
    TransactionDescr(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash) : addr(addr), lt(lt), hash(hash) {
    }
    block::StdAddress addr;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  };
  std::vector<TransactionDescr> transactions_;
  td::uint32 trans_req_count_;
};

class HttpQueryBlockSearch : public HttpQueryCommon {
 public:
  HttpQueryBlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, ton::BlockSeqno seqno,
                       std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, ton::LogicalTime lt,
                       std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, bool dummy, ton::UnixTime utime,
                       std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockSearch(std::map<std::string, std::string> opts, std::string prefix,
                       td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_block_header(td::BufferSlice result);
  void got_shard_info(td::BufferSlice result);
  void got_transactions(td::BufferSlice result);

  void failed_to_get_shard_info(td::Status error);

 private:
  ton::AccountIdPrefixFull account_prefix_;
  td::uint32 mode_ = 0;
  ton::BlockSeqno seqno_ = 0;
  ton::LogicalTime lt_ = 0;
  ton::UnixTime utime_ = 0;

  ton::BlockIdExt block_id_;

  td::BufferSlice data_;
  td::BufferSlice shard_data_;
  td::Status shard_data_error_;

  td::uint32 pending_queries_ = 0;

  struct TransactionDescr {
    TransactionDescr(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash) : addr(addr), lt(lt), hash(hash) {
    }
    block::StdAddress addr;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  };
  std::vector<TransactionDescr> transactions_;
  td::uint32 trans_req_count_;
};

class HttpQueryViewAccount : public HttpQueryCommon {
 public:
  HttpQueryViewAccount(ton::BlockIdExt block_id, block::StdAddress addr, std::string prefix,
                       td::Promise<MHD_Response *> promise);
  HttpQueryViewAccount(std::map<std::string, std::string> opts, std::string prefix,
                       td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_account(td::BufferSlice result);

 private:
  ton::BlockIdExt block_id_;
  block::StdAddress addr_;

  td::BufferSlice data_;
  td::BufferSlice proof_;
  ton::BlockIdExt res_block_id_;
};

class HttpQueryViewTransaction : public HttpQueryCommon {
 public:
  HttpQueryViewTransaction(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash, std::string prefix,
                           td::Promise<MHD_Response *> promise);
  HttpQueryViewTransaction(std::map<std::string, std::string> opts, std::string prefix,
                           td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_transaction(td::BufferSlice result);

 private:
  block::StdAddress addr_;
  ton::LogicalTime lt_;
  ton::Bits256 hash_;

  td::BufferSlice data_;
  ton::BlockIdExt res_block_id_;
};

class HttpQueryViewTransaction2 : public HttpQueryCommon {
 public:
  HttpQueryViewTransaction2(ton::BlockIdExt block_id, block::StdAddress addr, ton::LogicalTime lt, std::string prefix,
                            td::Promise<MHD_Response *> promise);
  HttpQueryViewTransaction2(std::map<std::string, std::string> opts, std::string prefix,
                            td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_transaction(td::BufferSlice result);

 private:
  ton::BlockIdExt block_id_;
  block::StdAddress addr_;
  ton::LogicalTime lt_;
  ton::Bits256 hash_;

  td::BufferSlice data_;
};

class HttpQueryViewLastBlock : public HttpQueryCommon {
 public:
  HttpQueryViewLastBlock(std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryViewLastBlock(std::map<std::string, std::string> opts, std::string prefix,
                         td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up() override;
  void got_result(td::BufferSlice result);

 private:
  ton::BlockIdExt res_block_id_;
};

class HttpQueryConfig : public HttpQueryCommon {
 public:
  HttpQueryConfig(std::string prefix, ton::BlockIdExt block_id, std::vector<td::int32> params,
                  td::Promise<MHD_Response *> promise);
  HttpQueryConfig(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up() override;
  void got_block(td::BufferSlice result);
  void send_main_query();
  void got_result(td::BufferSlice result);

 private:
  ton::BlockIdExt block_id_;
  std::vector<td::int32> params_;

  td::BufferSlice state_proof_;
  td::BufferSlice config_proof_;
};

class HttpQuerySendForm : public HttpQueryCommon {
 public:
  HttpQuerySendForm(std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQuerySendForm(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void start_up() override;
  void finish_query();

 private:
};

class HttpQuerySend : public HttpQueryCommon {
 public:
  HttpQuerySend(std::string prefix, td::BufferSlice data, td::Promise<MHD_Response *> promise);
  HttpQuerySend(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up() override;
  void got_result(td::BufferSlice result);

 private:
  td::BufferSlice data_;
  td::int32 status_;
};

class HttpQueryRunMethod : public HttpQueryCommon {
 public:
  HttpQueryRunMethod(ton::BlockIdExt block_id, block::StdAddress addr, std::string method_name,
                     std::vector<vm::StackEntry> params, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryRunMethod(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up_query() override;
  void got_account(td::BufferSlice result);

 private:
  ton::BlockIdExt block_id_;
  block::StdAddress addr_;

  std::string method_name_;
  std::vector<vm::StackEntry> params_;

  td::BufferSlice data_;
  td::BufferSlice proof_;
  td::BufferSlice shard_proof_;
  ton::BlockIdExt res_block_id_;
};

class HttpQueryStatus : public HttpQueryCommon {
 public:
  HttpQueryStatus(std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryStatus(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query();

  void start_up() override;
  void got_results(CoreActorInterface::RemoteNodeStatusList results);

 private:
  CoreActorInterface::RemoteNodeStatusList results_;
};

