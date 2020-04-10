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
#include "ihr-message.hpp"
#include "vm/boc.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "block/block-db.h"
#include "vm/cells/MerkleProof.h"

namespace ton {

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

IhrMessageQ::IhrMessageQ(td::BufferSlice data, td::Ref<vm::Cell> root, BlockIdExt block,
                         AccountIdPrefixFull addr_prefix)
    : root_(std::move(root)), addr_prefix_(addr_prefix), data_(std::move(data)), blkid_(block) {
  hash_ = block::compute_file_hash(data_);
}

td::Result<Ref<IhrMessageQ>> IhrMessageQ::create_ihr_message(td::BufferSlice data) {
  if (data.size() > max_ihr_msg_size) {
    return td::Status::Error("IHR message too large, rejecting");
  }
  vm::BagOfCells boc;
  auto res = boc.deserialize(data.as_slice());
  if (res.is_error()) {
    return res.move_as_error();
  }
  if (boc.get_root_count() != 3) {
    return td::Status::Error("IHR message is not a valid bag of cells with three roots");  // not a valid bag-of-Cells
  }
  auto ihr_msg = boc.get_root_cell(0), blk = boc.get_root_cell(1), proof = boc.get_root_cell(2);
  if (ihr_msg->get_level() != 0 || blk->get_level() != 0 || proof->get_level() != 0) {
    return td::Status::Error("IHR message must have zero level");
  }
  vm::CellSlice cs{vm::NoVmOrd{}, ihr_msg};
  if (cs.prefetch_ulong(1) != 0) {  // int_msg_info$0
    return td::Status::Error("IHR message must begin with int_msg_info$0");
  }
  ton::Bits256 hash{ihr_msg->get_hash().bits()};
  if (!block::gen::t_Message_Any.validate_ref(ihr_msg)) {
    return td::Status::Error("IHR message is not a (Message Any) according to automated checks");
  }
  if (!block::tlb::t_Message.validate_ref(ihr_msg)) {
    return td::Status::Error("IHR message is not a (Message Any) according to hand-written checks");
  }
  block::gen::CommonMsgInfo::Record_int_msg_info info;
  if (!tlb::unpack_cell_inexact(ihr_msg, info)) {
    return td::Status::Error("cannot unpack IHR message header");
  }
  auto dest_prefix = block::tlb::t_MsgAddressInt.get_prefix(info.dest);
  if (!dest_prefix.is_valid()) {
    return td::Status::Error("destination of an IHR message is an invalid blockchain address");
  }
  cs.load_ord(std::move(blk));
  BlockIdExt blkid;
  if (!(block::tlb::t_BlockIdExt.unpack(cs, blkid) && cs.empty_ext())) {
    return td::Status::Error("IHR message does not contain a valid source BlockIdExt");
  }
  try {
    auto virt_root = vm::MerkleProof::virtualize(proof, 1);
    if (virt_root.is_null()) {
      return td::Status::Error("IHR message does not contain a valid Merkle proof");
    }
    RootHash virt_hash{virt_root->get_hash().bits()};
    if (virt_hash != blkid.root_hash) {
      return td::Status::Error("IHR message contains a Merkle proof with incorrect root hash: expected " +
                               blkid.root_hash.to_hex() + ", found " + virt_hash.to_hex());
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    ShardIdFull shard;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
          block::tlb::t_ShardIdent.unpack(info.shard.write(), shard) && !info.vert_seq_no &&
          tlb::unpack_cell(blk.extra, extra))) {
      return td::Status::Error("cannot unpack block header in the Merkle proof of an IHR message");
    }
    if (blkid.shard_full() != shard || blkid.seqno() != BlockSeqno(info.seq_no)) {
      return td::Status::Error(
          "block header in the Merkle proof of an IHR message does not belong to the declared source block");
    }
    vm::AugmentedDictionary out_msg_dict{vm::load_cell_slice_ref(extra.out_msg_descr), 256,
                                         block::tlb::aug_OutMsgDescr};
    Bits256 key{ihr_msg->get_hash().bits()};
    auto descr = out_msg_dict.lookup(key);
    out_msg_dict.reset();
    if (descr.is_null()) {
      return td::Status::Error(
          "IHR message contains an invalid proof with OutMsgDescr not containing a key equal to the hash of the "
          "message");
    }
    if (descr->prefetch_ulong(3) != 1 || !descr->size_refs()) {  // expect msg_export_new$001
      return td::Status::Error(
          "IHR message contains an invalid proof with OutMsg record not of type msg_export_new$001");
    }
    cs.load_ord(descr->prefetch_ref());
    if (!cs.size_refs()) {
      return td::Status::Error("IHR message contains an invalid MsgEnvelope");
    }
    if (key != cs.prefetch_ref()->get_hash().bits()) {
      return td::Status::Error(
          "IHR message contains an invalid proof with MsgEnvelope not pointing to the message included");
    }
  } catch (vm::VmError err) {
    return td::Status::Error("error while processing Merkle proof provided in IHR message: "s + err.get_msg());
  } catch (vm::VmVirtError err) {
    return td::Status::Error("error while processing Merkle proof provided in IHR message: "s + err.get_msg());
  }
  return Ref<IhrMessageQ>{true, std::move(data), std::move(ihr_msg), blkid, dest_prefix};
}

}  // namespace validator
}  // namespace ton
