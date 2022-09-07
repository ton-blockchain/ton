/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include "adnl/adnl.h"
#include "adnl/utils.hpp"
#include "auto/tl/ton_api.h"

#include "catchain-types.h"
#include "catchain-receiver-interface.h"

namespace ton {

namespace catchain {

constexpr int VERBOSITY_NAME(CATCHAIN_WARNING) = verbosity_WARNING;
constexpr int VERBOSITY_NAME(CATCHAIN_NOTICE) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(CATCHAIN_INFO) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(CATCHAIN_DEBUG) = verbosity_DEBUG;
constexpr int VERBOSITY_NAME(CATCHAIN_EXTRA_DEBUG) = verbosity_DEBUG + 1;

class CatChainReceivedBlock;
class CatChainReceiverSource;

class CatChainReceiver : public CatChainReceiverInterface {
 public:
  struct PrintId {
    CatChainSessionId instance_;
    PublicKeyHash local_id_;
  };
  virtual PrintId print_id() const = 0;
  virtual CatChainReceivedBlock *create_block(tl_object_ptr<ton_api::catchain_block> block,
                                              td::SharedSlice payload) = 0;
  virtual CatChainReceivedBlock *create_block(tl_object_ptr<ton_api::catchain_block_dep> block) = 0;
  virtual CatChainReceiverSource *get_source(td::uint32 source_id) const = 0;
  virtual PublicKeyHash get_source_hash(td::uint32 source_id) const = 0;
  virtual td::uint32 get_forks_cnt() const = 0;
  virtual td::uint32 get_sources_cnt() const = 0;
  virtual CatChainSessionId get_incarnation() const = 0;
  virtual void run_block(CatChainReceivedBlock *block) = 0;
  virtual void deliver_block(CatChainReceivedBlock *block) = 0;
  virtual td::uint32 add_fork() = 0;
  virtual void add_prepared_event(td::BufferSlice data) = 0;
  virtual void on_blame(td::uint32 source_id) = 0;

  virtual const CatChainOptions &opts() const = 0;

  virtual td::Status validate_block_sync(const tl_object_ptr<ton_api::catchain_block_dep> &dep) const = 0;
  virtual td::Status validate_block_sync(const tl_object_ptr<ton_api::catchain_block> &block,
                                         const td::Slice &payload) const = 0;

  virtual ~CatChainReceiver() = default;
};

td::uint64 get_max_block_height(const CatChainOptions& opts, size_t sources_cnt);

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiver::PrintId &print_id) {
  sb << "[catchainreceiver " << print_id.instance_ << "@" << print_id.local_id_ << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiver *catchain) {
  sb << catchain->print_id();
  return sb;
}

}  // namespace td
