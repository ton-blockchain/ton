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

#include "td/utils/buffer.h"
#include "td/utils/SharedSlice.h"
#include "auto/tl/ton_api.h"

#include "catchain/catchain-receiver.h"

namespace ton {

namespace catchain {

class CatChainReceiver;
class CatChainReceiverSource;

class CatChainReceivedBlock {
 public:
  // getters
  virtual const td::SharedSlice &get_payload() const = 0;
  virtual CatChainBlockHash get_hash() const = 0;
  virtual const td::SharedSlice &get_signature() const = 0;

  virtual CatChainBlockHeight get_height() const = 0;
  virtual CatChainReceivedBlock *get_prev() const = 0;
  virtual CatChainBlockHash get_prev_hash() const = 0;

  virtual const std::vector<CatChainBlockHeight> &get_vt() const = 0;
  virtual std::vector<CatChainBlockHash> get_dep_hashes() const = 0;

  virtual CatChainReceiver *get_chain() const = 0;

  virtual td::uint32 get_fork_id() const = 0;
  virtual td::uint32 get_source_id() const = 0;

  virtual tl_object_ptr<ton_api::catchain_block> export_tl() const = 0;
  virtual tl_object_ptr<ton_api::catchain_block_dep> export_tl_dep() const = 0;

  virtual void find_pending_deps(std::vector<CatChainBlockHash> &vec, td::uint32 max_size) const = 0;

  virtual bool has_rev_deps() const = 0;

 public:
  // state
  virtual bool initialized() const = 0;
  virtual bool delivered() const = 0;
  virtual bool is_ill() const = 0;
  virtual bool is_custom() const = 0;
  virtual bool in_db() const = 0;

 public:
  //change state
  virtual void initialize(tl_object_ptr<ton_api::catchain_block> block, td::SharedSlice payload) = 0;
  virtual void set_ill() = 0;
  virtual void written() = 0;
  virtual void run() = 0;

 public:
  static std::unique_ptr<CatChainReceivedBlock> create(tl_object_ptr<ton_api::catchain_block> block,
                                                       td::SharedSlice payload, CatChainReceiver *chain);
  static std::unique_ptr<CatChainReceivedBlock> create(tl_object_ptr<ton_api::catchain_block_dep> block,
                                                       CatChainReceiver *chain);
  static std::unique_ptr<CatChainReceivedBlock> create_root(td::uint32 source_id, CatChainSessionId session_id,
                                                            CatChainReceiver *chain);

  static tl_object_ptr<ton_api::catchain_block_id> block_id(const CatChainReceiver *chain,
                                                            const tl_object_ptr<ton_api::catchain_block> &block,
                                                            const td::Slice &payload);
  static tl_object_ptr<ton_api::catchain_block_id> block_id(const CatChainReceiver *chain,
                                                            const tl_object_ptr<ton_api::catchain_block_dep> &block);
  static CatChainBlockHash block_hash(const CatChainReceiver *chain,
                                      const tl_object_ptr<ton_api::catchain_block> &block,
                                      const td::Slice &payload);
  static CatChainBlockHash block_hash(const CatChainReceiver *chain,
                                      const tl_object_ptr<ton_api::catchain_block_dep> &block);
  static td::Status pre_validate_block(const CatChainReceiver *chain,
                                       const tl_object_ptr<ton_api::catchain_block> &block,
                                       const td::Slice &payload);
  static td::Status pre_validate_block(const CatChainReceiver *chain,
                                       const tl_object_ptr<ton_api::catchain_block_dep> &block);
  static CatChainBlockPayloadHash data_payload_hash(const CatChainReceiver *chain,
                                                    const tl_object_ptr<ton_api::catchain_block_data> &data,
                                                    const td::Slice &payload);


  virtual ~CatChainReceivedBlock() = default;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceivedBlock &block) {
  sb << "[block " << block.get_chain()->get_incarnation() << " " << block.get_source_id() << " " << block.get_fork_id()
     << " " << block.get_hash() << "]";
  return sb;
}
inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceivedBlock *block) {
  sb << *block;
  return sb;
}

}  // namespace td

