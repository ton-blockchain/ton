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
#include "interfaces/message-queue.h"

namespace ton {
namespace validator {
using td::Ref;

class ShardStateQ;

class MessageQueueQ : public MessageQueue {
  BlockIdExt blkid;
  RootHash rhash;
  Ref<vm::Cell> root;
  MessageQueueQ* make_copy() const override;

 protected:
  friend class ShardStateQ;
  MessageQueueQ(const MessageQueueQ& other) = default;
  MessageQueueQ(MessageQueueQ&& other) = default;

 public:
  MessageQueueQ(const BlockIdExt& _id, Ref<vm::Cell> _root) : blkid(_id), root(std::move(_root)) {
    if (root.is_null()) {
      rhash.set_zero();
    } else {
      rhash = root->get_hash().bits();
    }
  }
  virtual ~MessageQueueQ() = default;
  ShardIdFull get_shard() const override {
    return ShardIdFull(blkid);
  }
  BlockSeqno get_seqno() const override {
    return blkid.id.seqno;
  }
  BlockIdExt get_block_id() const override {
    return blkid;
  }
  RootHash root_hash() const override {
    return rhash;
  }
  td::Ref<vm::Cell> root_cell() const override {
    return root;
  }
};

}  // namespace validator
}  // namespace ton
