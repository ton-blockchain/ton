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
#include "external-message.hpp"
#include "vm/boc.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/block-db.h"

namespace ton {

namespace validator {
using td::Ref;

ExtMessageQ::ExtMessageQ(td::BufferSlice data, td::Ref<vm::Cell> root, AccountIdPrefixFull addr_prefix)
    : root_(std::move(root)), addr_prefix_(addr_prefix), data_(std::move(data)) {
  hash_ = block::compute_file_hash(data_);
}

td::Result<Ref<ExtMessageQ>> ExtMessageQ::create_ext_message(td::BufferSlice data) {
  if (data.size() > max_ext_msg_size) {
    return td::Status::Error("external message too large, rejecting");
  }
  vm::BagOfCells boc;
  auto res = boc.deserialize(data.as_slice());
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 1) {
    return td::Status::Error("external message is not a valid bag of cells");  // not a valid bag-of-Cells
  }
  auto ext_msg = boc.get_root_cell();
  if (ext_msg->get_level() != 0) {
    return td::Status::Error("external message must have zero level");
  }
  if (ext_msg->get_depth() >= max_ext_msg_depth) {
    return td::Status::Error("external message is too deep");
  }
  vm::CellSlice cs{vm::NoVmOrd{}, ext_msg};
  if (cs.prefetch_ulong(2) != 2) {  // ext_in_msg_info$10
    return td::Status::Error("external message must begin with ext_in_msg_info$10");
  }
  ton::Bits256 hash{ext_msg->get_hash().bits()};
  if (!block::gen::t_Message_Any.validate_ref(128, ext_msg)) {
    return td::Status::Error("external message is not a (Message Any) according to automated checks");
  }
  if (!block::tlb::t_Message.validate_ref(128, ext_msg)) {
    return td::Status::Error("external message is not a (Message Any) according to hand-written checks");
  }
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  if (!tlb::unpack_cell_inexact(ext_msg, info)) {
    return td::Status::Error("cannot unpack external message header");
  }
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  if (!dest_prefix.is_valid()) {
    return td::Status::Error("destination of an inbound external message is an invalid blockchain address");
  }
  return Ref<ExtMessageQ>{true, std::move(data), std::move(ext_msg), dest_prefix};
}

}  // namespace validator
}  // namespace ton
