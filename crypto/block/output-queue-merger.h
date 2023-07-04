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
#include "ton/ton-types.h"
#include "vm/cells/CellSlice.h"
#include "block/mc-config.h"

namespace block {
using td::Ref;

struct OutputQueueMerger {
  struct MsgKeyValue {
    static constexpr int max_key_len = 32 + 64 + 256;
    Ref<vm::CellSlice> msg;
    unsigned long long lt;
    int source;
    int key_len{0};
    td::BitArray<max_key_len> key;
    MsgKeyValue() = default;
    MsgKeyValue(int src, Ref<vm::Cell> node);
    MsgKeyValue(td::ConstBitPtr key_pfx, int key_pfx_len, int src, Ref<vm::Cell> node);
    bool operator<(const MsgKeyValue& other) const;
    bool is_fork() const {
      return key_len < max_key_len;
    }
    bool invalidate();
    static bool less(const std::unique_ptr<MsgKeyValue>& he1, const std::unique_ptr<MsgKeyValue>& he2);
    static bool greater(const std::unique_ptr<MsgKeyValue>& he1, const std::unique_ptr<MsgKeyValue>& he2);

   protected:
    friend struct OutputQueueMerger;
    static ton::LogicalTime get_node_lt(Ref<vm::Cell> node, int key_pfx_len);
    bool replace_with_child(bool child_idx);
    bool replace_by_prefix(td::ConstBitPtr req_pfx, int req_pfx_len);
    bool unpack_node(td::ConstBitPtr key_pfx, int key_pfx_len, Ref<vm::Cell> node);
    bool split(MsgKeyValue& second);
  };
  //
  std::vector<std::unique_ptr<MsgKeyValue>> msg_list;

 public:
  struct Neighbor {
    ton::BlockIdExt block_id_;
    td::Ref<vm::Cell> outmsg_root_;
    bool disabled_;
    td::int32 msg_limit_;  // -1 - unlimited
    Neighbor(ton::BlockIdExt block_id, td::Ref<vm::Cell> outmsg_root, bool disabled = false, td::int32 msg_limit = -1)
        : block_id_(block_id), outmsg_root_(std::move(outmsg_root)), disabled_(disabled), msg_limit_(msg_limit) {
    }
  };

  OutputQueueMerger(ton::ShardIdFull queue_for, std::vector<Neighbor> neighbors);
  bool is_eof() const {
    return eof;
  }
  MsgKeyValue* cur();
  std::unique_ptr<MsgKeyValue> extract_cur();
  bool next();

 private:
  td::BitArray<32 + 64> common_pfx;
  int common_pfx_len;
  std::vector<std::unique_ptr<MsgKeyValue>> heap;
  std::size_t pos{0};
  std::vector<td::int32> src_remaining_msgs_;
  bool eof;
  bool failed;
  void add_root(int src, Ref<vm::Cell> outmsg_root, td::int32 msg_limit);
  bool load();
};

}  // namespace block
