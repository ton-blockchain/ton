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
#include "overlay/overlays.h"
#include "catchain-types.h"

namespace ton {

namespace catchain {

class CatChainBlock {
 public:
  class Extra {
   public:
    virtual ~Extra() = default;
  };
  virtual td::SharedSlice &payload() = 0;
  virtual const td::SharedSlice &payload() const = 0;
  virtual Extra *extra() const = 0;
  virtual std::unique_ptr<Extra> move_extra() = 0;
  virtual void set_extra(std::unique_ptr<Extra> extra) = 0;

  virtual td::uint32 source() const = 0;
  virtual td::uint32 fork() const = 0;
  virtual PublicKeyHash source_hash() const = 0;
  virtual CatChainBlockHash hash() const = 0;
  virtual CatChainBlockHeight height() const = 0;

  virtual CatChainBlock *prev() = 0;
  virtual const CatChainBlock *prev() const = 0;
  virtual const std::vector<CatChainBlock *> &deps() const = 0;
  virtual const std::vector<CatChainBlockHeight> &vt() const = 0;

  virtual bool preprocess_is_sent() const = 0;
  virtual void preprocess_sent() = 0;

  virtual bool is_processed() const = 0;
  virtual void set_processed() = 0;

  virtual bool is_descendant_of(CatChainBlock *block) = 0;

  static std::unique_ptr<CatChainBlock> create(td::uint32 src, td::uint32 fork_id, const PublicKeyHash &src_hash,
                                               CatChainBlockHeight height, const CatChainBlockHash &hash,
                                               td::SharedSlice payload, CatChainBlock *prev,
                                               std::vector<CatChainBlock *> deps, std::vector<CatChainBlockHeight> vt);

  virtual ~CatChainBlock() = default;
};

class CatChain : public td::actor::Actor {
 public:
  class Callback {
   public:
    virtual void process_blocks(std::vector<CatChainBlock *> blocks) = 0;
    virtual void finished_processing() = 0;
    virtual void preprocess_block(CatChainBlock *block) = 0;
    virtual void process_broadcast(const PublicKeyHash &src, td::BufferSlice data) = 0;
    virtual void process_message(const PublicKeyHash &src, td::BufferSlice data) = 0;
    virtual void process_query(const PublicKeyHash &src, td::BufferSlice data,
                               td::Promise<td::BufferSlice> promise) = 0;
    virtual void started() = 0;
    virtual ~Callback() = default;
  };
  struct PrintId {
    CatChainSessionId instance_;
    PublicKeyHash local_id_;
  };
  virtual PrintId print_id() const = 0;
  virtual void processed_block(td::BufferSlice payload) = 0;
  virtual void need_new_block(td::Timestamp t) = 0;
  virtual void debug_add_fork(td::BufferSlice payload, CatChainBlockHeight height) = 0;

  virtual void send_broadcast(td::BufferSlice data) = 0;
  virtual void send_message(const PublicKeyHash &dst, td::BufferSlice data) = 0;
  virtual void send_query(const PublicKeyHash &dst, std::string name, td::Promise<td::BufferSlice> promise,
                          td::Timestamp timeout, td::BufferSlice query) = 0;
  virtual void send_query_via(const PublicKeyHash &dst, std::string name, td::Promise<td::BufferSlice> promise,
                              td::Timestamp timeout, td::BufferSlice query, td::uint64 max_answer_size,
                              td::actor::ActorId<adnl::AdnlSenderInterface> via) = 0;
  virtual void destroy() = 0;

  static td::actor::ActorOwn<CatChain> create(std::unique_ptr<Callback> callback, const CatChainOptions &opts,
                                              td::actor::ActorId<keyring::Keyring> keyring,
                                              td::actor::ActorId<adnl::Adnl> adnl,
                                              td::actor::ActorId<overlay::Overlays> overlay_manager,
                                              std::vector<CatChainNode> ids, const PublicKeyHash &local_id,
                                              const CatChainSessionId &unique_hash, std::string db_root,
                                              std::string db_suffix, bool allow_unsafe_self_blocks_resync);
  ~CatChain() override = default;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChain::PrintId &print_id) {
  sb << "[catchain " << print_id.instance_ << "@" << print_id.local_id_ << "]";
  return sb;
}

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChain *catchain) {
  sb << catchain->print_id();
  return sb;
}

}  // namespace td
