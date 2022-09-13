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

#include "catchain-receiver.h"

#include "keys/encryptor.h"

namespace ton {

namespace catchain {

class CatChainReceiver;
class CatChainReceivedBlock;

class CatChainReceiverSource {
 public:
  virtual td::uint32 get_id() const = 0;
  virtual PublicKeyHash get_hash() const = 0;
  virtual PublicKey get_full_id() const = 0;
  virtual adnl::AdnlNodeIdShort get_adnl_id() const = 0;

  virtual td::uint32 add_fork() = 0;

  virtual bool blamed() const = 0;
  virtual void blame(td::uint32 fork, CatChainBlockHeight height) = 0;
  virtual void blame() = 0;

  virtual const std::vector<td::uint32> &get_forks() const = 0;
  virtual const std::vector<CatChainBlockHeight> &get_blamed_heights() const = 0;
  virtual Encryptor *get_encryptor_sync() const = 0;
  virtual td::uint32 get_forks_cnt() const = 0;

  virtual CatChainBlockHeight delivered_height() const = 0;
  virtual CatChainBlockHeight received_height() const = 0;
  virtual CatChainReceivedBlock *get_block(CatChainBlockHeight height) const = 0;
  virtual void block_received(CatChainBlockHeight height) = 0;
  virtual void block_delivered(CatChainBlockHeight height) = 0;

  virtual bool has_unreceived() const = 0;
  virtual bool has_undelivered() const = 0;

  virtual void on_new_block(CatChainReceivedBlock *block) = 0;
  virtual void on_found_fork_proof(const td::Slice &fork) = 0;
  virtual td::BufferSlice fork_proof() const = 0;
  virtual bool fork_is_found() const = 0;

  static td::Result<std::unique_ptr<CatChainReceiverSource>> create(CatChainReceiver *chain, PublicKey pub_key,
                                                                    adnl::AdnlNodeIdShort adnl_id, td::uint32 id);

  virtual CatChainReceiver *get_chain() const = 0;

  virtual ~CatChainReceiverSource() = default;
};

}  // namespace catchain

}  // namespace ton

namespace td {

inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiverSource &source) {
  sb << "[source " << source.get_chain()->get_incarnation() << " " << source.get_id() << "]";
  return sb;
}
inline td::StringBuilder &operator<<(td::StringBuilder &sb, const ton::catchain::CatChainReceiverSource *source) {
  sb << *source;
  return sb;
}

}  // namespace td
