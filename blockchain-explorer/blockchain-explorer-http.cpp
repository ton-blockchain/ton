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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "blockchain-explorer-http.hpp"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "block/mc-config.h"
#include "ton/ton-shard.h"

HttpAnswer& HttpAnswer::operator<<(AddressCell addr_c) {
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(addr_c.root, wc, addr)) {
    abort("<cannot unpack addr>");
    return *this;
  }
  block::StdAddress caddr{wc, addr};
  *this << "<a href=\"" << AccountLink{caddr, ton::BlockIdExt{}} << "\">" << caddr.rserialize(true) << "</a>";
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(MessageCell msg) {
  if (msg.root.is_null()) {
    abort("<message not found");
    return *this;
  }
  vm::CellSlice cs{vm::NoVmOrd(), msg.root};
  block::gen::CommonMsgInfo info;
  td::Ref<vm::CellSlice> src, dest;
  *this << "<div id=\"msg" << msg.root->get_hash() << "\">";
  *this << "<div class=\"table-responsive my-3\">\n"
        << "<table class=\"table-sm table-striped\">\n"
        << "<tr><th>hash</th><td>" << msg.root->get_hash().to_hex() << "</td></tr>\n";
  switch (block::gen::t_CommonMsgInfo.get_tag(cs)) {
    case block::gen::CommonMsgInfo::ext_in_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(cs, info)) {
        abort("<cannot unpack inbound external message>");
        return *this;
      }
      *this << "<tr><th>type</th><td>external</td></tr>\n"
            << "<tr><th>source</th><td>NONE</td></tr>\n"
            << "<tr><th>destination</th><td>" << AddressCell{info.dest} << "</td></tr>\n";
      break;
    }
    case block::gen::CommonMsgInfo::ext_out_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info;
      if (!tlb::unpack(cs, info)) {
        abort("<cannot unpack outbound external message>");
        return *this;
      }
      *this << "<tr><th>type</th><td>external OUT</td></tr>\n"
            << "<tr><th>source</th><td>" << AddressCell{info.src} << "</td></tr>\n"
            << "<tr><th>destination</th><td>NONE</td></tr>\n"
            << "<tr><th>lt</th><td>" << info.created_lt << "</td></tr>\n"
            << "<tr><th>time</th><td>" << info.created_at << "</td></tr>\n";
      break;
    }
    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
        abort("cannot unpack internal message");
        return *this;
      }
      td::RefInt256 value;
      td::Ref<vm::Cell> extra;
      if (!block::unpack_CurrencyCollection(info.value, value, extra)) {
        abort("cannot unpack message value");
        return *this;
      }
      *this << "<tr><th>type</th><td>internal</td></tr>\n"
            << "<tr><th>source</th><td>" << AddressCell{info.src} << "</td></tr>\n"
            << "<tr><th>destination</th><td>" << AddressCell{info.dest} << "</td></tr>\n"
            << "<tr><th>lt</th><td>" << info.created_lt << "</td></tr>\n"
            << "<tr><th>time</th><td>" << info.created_at << "</td></tr>\n"
            << "<tr><th>value</th><td>" << value << "</td></tr>\n";
      break;
    }
    default:
      abort("cannot unpack message");
      return *this;
  }

  *this << "</table></div>\n";
  *this << RawData<block::gen::Message>{msg.root, block::gen::t_Anything} << "</div>";
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(ton::BlockIdExt block_id) {
  return *this << "<a href=\"" << BlockLink{block_id} << "\">" << block_id.id.to_str() << "</a>";
}

HttpAnswer& HttpAnswer::operator<<(ton::BlockId block_id) {
  return *this << "<a href=\"" << prefix_ << "search?workchain=" << block_id.workchain
               << "&shard=" << ton::shard_to_str(block_id.shard) << "&seqno=" << block_id.seqno << "\">"
               << block_id.to_str() << "</a>";
}

HttpAnswer& HttpAnswer::operator<<(BlockSearch bs) {
  *this << "<form class=\"container\" action=\"" << prefix_ << "search\" method=\"get\">"
        << "<div class=\"row\">"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>workchain</label>"
        << "<input type=\"text\" class=\"form-control mr-2\" name=\"workchain\" value=\""
        << (bs.block_id.is_valid() ? std::to_string(bs.block_id.id.workchain) : "") << "\">"
        << "</div>\n"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>shard/account</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"shard\" value=\""
        << (bs.block_id.is_valid() ? ton::shard_to_str(bs.block_id.id.shard) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>seqno</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"seqno\" value=\""
        << (bs.block_id.is_valid() ? std::to_string(bs.block_id.id.seqno) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label class=\"d-none d-lg-block\">&nbsp;</label>"
        << "<div><button type=\"submit\" class=\"btn btn-primary mr-2\">Submit</button></div>"
        << "</div></div><div class=\"row\">"
        << "<div class=\"form-group col-md-6\">"
        << "<label>logical time</label>"
        << "<input type=\"text\" class=\"form-control mr-2\" name=\"lt\">"
        << "</div>\n"
        << "<div class=\"form-group col-md-6\">"
        << "<label>unix time</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"utime\"></div>"
        << "</div><div class=\"row\">"
        << "<div class=\"form-group col-md-6\">"
        << "<label>root hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"roothash\" value=\""
        //<< (!bs.block_id.id.is_valid() || bs.block_id.root_hash.is_zero() ? "" : bs.block_id.root_hash.to_hex())
        << "\"></div>"
        << "<div class=\"col-md-6\">"
        << "<label>file hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"filehash\" value=\""
        //<< (!bs.block_id.id.is_valid() || bs.block_id.file_hash.is_zero() ? "" : bs.block_id.file_hash.to_hex())
        << "\"></div>"
        << "</div></form>\n";
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(AccountSearch bs) {
  *this << "<form class=\"container\" action=\"" << prefix_ << "account\" method=\"get\">"
        << "<div class=\"row\">"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>workchain</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"workchain\" value=\""
        << (bs.block_id.is_valid() ? std::to_string(bs.block_id.id.workchain) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>shard</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"shard\" value=\""
        << (bs.block_id.is_valid() ? ton::shard_to_str(bs.block_id.id.shard) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>seqno</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"seqno\" value=\""
        << (bs.block_id.is_valid() ? std::to_string(bs.block_id.id.seqno) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label class=\"d-none d-lg-block\">&nbsp;</label>"
        << "<div><button type=\"submit\" class=\"btn btn-primary mr-2\">Submit</button>"
        << "<button class=\"btn btn-outline-primary\" type=\"reset\">Reset</button></div>"
        << "</div></div><div class=\"row\">"
        << "<div class=\"form-group col-md-6\">"
        << "<label>root hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"roothash\" value=\""
        << (!bs.block_id.id.is_valid() || bs.block_id.root_hash.is_zero() ? "" : bs.block_id.root_hash.to_hex())
        << "\"></div>"
        << "<div class=\"form-group col-md-6\">"
        << "<label>file hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"filehash\" value=\""
        << (!bs.block_id.id.is_valid() || bs.block_id.file_hash.is_zero() ? "" : bs.block_id.file_hash.to_hex())
        << "\"></div>"
        << "</div><div class=\"row\">"
        << "<div class=\"form-group col-md-12\">"
        << "<label>account id</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"account\" value=\""
        << (bs.addr.addr.is_zero() ? "" : bs.addr.rserialize(true)) << "\"></div>"
        << "</div>\n"
        << "</form>\n";
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(TransactionSearch bs) {
  *this << "<form class=\"container\" action=\"" << prefix_ << "transaction\" method=\"get\">"
        << "<div class=\"row\">"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>workchain</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"workchain\" value=\""
        << (bs.block_id.is_valid() ? std::to_string(bs.block_id.id.workchain) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>shard</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"shard\" value=\""
        << (bs.block_id.is_valid() ? ton::shard_to_str(bs.block_id.id.shard) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label>seqno</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"seqno\" value=\""
        << (bs.block_id.is_valid() ? std::to_string(bs.block_id.id.seqno) : "") << "\"></div>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<label class=\"d-none d-lg-block\">&nbsp;</label>"
        << "<div><button type=\"submit\" class=\"btn btn-primary mr-2\">Submit</button>"
        << "<button class=\"btn btn-outline-primary\" type=\"reset\">Reset</button></div>"
        << "</div></div><div class=\"row\">"
        << "<div class=\"form-group col-md-6\">"
        << "<label>root hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"roothash\" value=\""
        << (!bs.block_id.id.is_valid() || bs.block_id.root_hash.is_zero() ? "" : bs.block_id.root_hash.to_hex())
        << "\"></div>"
        << "<div class=\"form-group col-md-6\">"
        << "<label>file hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"filehash\" value=\""
        << (!bs.block_id.id.is_valid() || bs.block_id.file_hash.is_zero() ? "" : bs.block_id.file_hash.to_hex())
        << "\"></div>"
        << "</div><div class=\"row\">"
        << "<div class=\"form-group col-md-12\">"
        << "<label>account id</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"account\" value=\""
        << (bs.addr.addr.is_zero() ? "" : bs.addr.rserialize(true)) << "\"></div>"
        << "</div><div class=\"row\">"
        << "<div class=\"form-group col-md-3\">"
        << "<label>transaction lt</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"lt\" value=\""
        << (bs.lt ? std::to_string(bs.lt) : "") << "\"></div>"
        << "<div class=\"form-group col-md-9\">"
        << "<label>transaction hash</label>"
        << "<input type =\"text\" class=\"form-control mr-2\" name=\"hash\" value=\""
        << (bs.hash.is_zero() ? "" : bs.hash.to_hex()) << "\"></div>"
        << "</div>\n"
        << "</form>\n";
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(TransactionCell trans_c) {
  if (trans_c.root.is_null()) {
    abort("transaction not found");
    return *this;
  }
  block::gen::Transaction::Record trans;
  if (!tlb::unpack_cell(trans_c.root, trans)) {
    abort("cannot unpack");
    return *this;
  }
  *this << "<div class=\"table-responsive my-3\">\n"
        << "<table class=\"table-sm table-striped\">\n"
        << "<tr><th>block</th><td><a href=\"" << BlockLink{trans_c.block_id} << "\">" << trans_c.block_id.id.to_str()
        << "</a></td></tr>"
        << "<tr><th>workchain</th><td>" << trans_c.addr.workchain << "</td></tr>"
        << "<tr><th>account hex</th><td>" << trans_c.addr.addr.to_hex() << "</td></tr>"
        << "<tr><th>account</th><td>" << trans_c.addr.rserialize(true) << "</td></tr>"
        << "<tr><th>hash</th><td>" << trans_c.root->get_hash().to_hex() << "</td></tr>\n"
        << "<tr><th>lt</th><td>" << trans.lt << "</td></tr>\n"
        << "<tr><th>time</th><td>" << trans.now << "</td></tr>\n"
        << "<tr><th>out messages</th><td>";
  vm::Dictionary dict{trans.r1.out_msgs, 15};
  for (td::int32 i = 0; i < trans.outmsg_cnt; i++) {
    auto out_msg = dict.lookup_ref(td::BitArray<15>{i});
    *this << " <a href=\"" << MessageLink{out_msg} << "\">" << i << "</a>";
  }
  *this << "</td></tr>\n"
        << "<tr><th>in message</th><td>";
  auto in_msg = trans.r1.in_msg->prefetch_ref();
  if (in_msg.is_null()) {
    *this << "NONE";
  } else {
    *this << "<a href=\"" << MessageLink{in_msg} << "\">" << in_msg->get_hash() << "</a>";
  }
  *this << "</td></tr>\n"
        << "<tr><th>prev transaction</th><td>";

  auto prev_lt = trans.prev_trans_lt;
  auto prev_hash = trans.prev_trans_hash;
  if (prev_lt > 0) {
    *this << "<a href=\"" << TransactionLink{trans_c.addr, prev_lt, prev_hash} << "\">lt=" << prev_lt
          << " hash=" << prev_hash.to_hex() << "</a>";
  } else {
    *this << "NONE";
  }
  *this << "</td></tr></table></div>\n";
  if (in_msg.not_null()) {
    *this << "<hr />" << MessageCell{in_msg};
  }
  for (int x = 0; x < trans.outmsg_cnt && x < 100; x++) {
    auto out_msg = dict.lookup_ref(td::BitArray<15>{x});
    *this << "<hr />" << MessageCell{out_msg};
  }
  *this << "<hr />";

  return *this << RawData<block::gen::Transaction>{trans_c.root} << "</div>";
}

HttpAnswer& HttpAnswer::operator<<(AccountCell acc_c) {
  *this << "<div>";
  auto block_id = acc_c.block_id;
  if (!block_id.is_valid_full()) {
    abort(PSTRING() << "shard block id " << block_id.to_str() << " in answer is invalid");
    return *this;
  }
  if (!ton::shard_contains(block_id.shard_full(), ton::extract_addr_prefix(acc_c.addr.workchain, acc_c.addr.addr))) {
    abort(PSTRING() << "received data from shard block " << block_id.to_str()
                    << " that cannot contain requested account " << acc_c.addr.workchain << ":"
                    << acc_c.addr.addr.to_hex());
    return *this;
  }
  if (acc_c.q_roots.size() != 2) {
    abort(PSTRING() << "account state proof must have exactly two roots");
    return *this;
  }
  ton::LogicalTime last_trans_lt = 0;
  ton::Bits256 last_trans_hash;
  last_trans_hash.set_zero();
  try {
    auto state_root = vm::MerkleProof::virtualize(acc_c.q_roots[1], 1);
    if (state_root.is_null()) {
      abort("account state proof is invalid");
      return *this;
    }
    block::gen::ShardStateUnsplit::Record sstate;
    if (!(tlb::unpack_cell(std::move(state_root), sstate))) {
      abort("cannot unpack state header");
      return *this;
    }
    vm::AugmentedDictionary accounts_dict{vm::load_cell_slice_ref(sstate.accounts), 256, block::tlb::aug_ShardAccounts};
    auto acc_csr = accounts_dict.lookup(acc_c.addr.addr);
    if (acc_csr.not_null()) {
      if (acc_c.root.is_null()) {
        abort(PSTRING() << "account state proof shows that account state for " << acc_c.addr.workchain << ":"
                        << acc_c.addr.addr.to_hex() << " must be non-empty, but it actually is empty");
        return *this;
      }
      block::gen::ShardAccount::Record acc_info;
      if (!tlb::csr_unpack(std::move(acc_csr), acc_info)) {
        abort("cannot unpack ShardAccount from proof");
        return *this;
      }
      if (acc_info.account->get_hash().bits().compare(acc_c.root->get_hash().bits(), 256)) {
        abort(PSTRING() << "account state hash mismatch: Merkle proof expects "
                        << acc_info.account->get_hash().bits().to_hex(256) << " but received data has "
                        << acc_c.root->get_hash().bits().to_hex(256));
        return *this;
      }
      last_trans_hash = acc_info.last_trans_hash;
      last_trans_lt = acc_info.last_trans_lt;
    } else if (acc_c.root.not_null()) {
      abort(PSTRING() << "account state proof shows that account state for " << acc_c.addr.workchain << ":"
                      << acc_c.addr.addr.to_hex() << " must be empty, but it is not");
      return *this;
    }
  } catch (vm::VmError err) {
    abort(PSTRING() << "error while traversing account proof : " << err.get_msg());
    return *this;
  } catch (vm::VmVirtError err) {
    abort(PSTRING() << "virtualization error while traversing account proof : " << err.get_msg());
    return *this;
  }

  *this << "<form class=\"container\" action=\"" << prefix_ << "runmethod\" method=\"get\">"
        << "<div class=\"row\">"
        << "<p>Run get method<p>"
        << "<div class=\"form-group col-lg-3 col-md-4\">"
        << "<input type=\"text\" class=\"form-control mr-2\" name=\"method\" placeholder=\"method\">"
        << "</div>\n"
        << "<div class=\"form-group col-lg-4 col-md-6\">"
        << "<input type=\"text\" class=\"form-control mr-2\" name=\"params\" placeholder=\"paramerers\"></div>"
        << "<input type=\"hidden\" name=\"account\" value=\"" << acc_c.addr.rserialize(true) << "\">"
        << "<input type=\"hidden\" name=\"workchain\" value=\"" << block_id.id.workchain << "\">"
        << "<input type=\"hidden\" name=\"shard\" value=\"" << ton::shard_to_str(block_id.id.shard) << "\">"
        << "<input type=\"hidden\" name=\"seqno\" value=\"" << block_id.id.seqno << "\">"
        << "<input type=\"hidden\" name=\"roothash\" value=\"" << block_id.root_hash.to_hex() << "\">"
        << "<input type=\"hidden\" name=\"filehash\" value=\"" << block_id.file_hash.to_hex() << "\">"
        << "<div><button type=\"submit\" class=\"btn btn-primary mr-2\">Run!</button></div>"
        << "</div></form>\n";

  *this << "<div class=\"table-responsive my-3\">\n"
        << "<table class=\"table-sm table-striped\">\n";
  *this << "<tr><th>block</th><td><a href=\"" << BlockLink{acc_c.block_id} << "\">" << block_id.id.to_str()
        << "</a></td></tr>";
  *this << "<tr><th>workchain</th><td>" << acc_c.addr.workchain << "</td></tr>";
  *this << "<tr><th>account hex</th><td>" << acc_c.addr.addr.to_hex() << "</td></tr>";
  *this << "<tr><th>account</th><td>" << acc_c.addr.rserialize(true) << "</td></tr>";
  if (last_trans_lt > 0) {
    *this << "<tr><th>last transaction</th><td>"
          << "<a href=\"" << TransactionLink{acc_c.addr, last_trans_lt, last_trans_hash} << "\">lt=" << last_trans_lt
          << " hash=" << last_trans_hash.to_hex() << "</a></td></tr>\n";
  } else {
    *this << "<tr><th>last transaction</th><td>no transactions</td></tr>";
  }
  *this << "</table></div>\n";

  *this << "<p><a class=\"btn btn-primary\" href=\"" << prefix_ << "account?account=" << acc_c.addr.rserialize(true)
        << "\">go to current state</a></p>\n";

  if (acc_c.root.not_null()) {
    *this << RawData<block::gen::Account>{acc_c.root};
  } else {
    *this << "<div class=\"alert alert-info\">account state is empty</div>";
  }
  return *this << "</div>";
}

HttpAnswer& HttpAnswer::operator<<(BlockHeaderCell head_c) {
  *this << "<div>";
  vm::CellSlice cs{vm::NoVm{}, head_c.root};
  auto block_id = head_c.block_id;
  try {
    auto virt_root = vm::MerkleProof::virtualize(head_c.root, 1);
    if (virt_root.is_null()) {
      abort("invalid merkle proof");
      return *this;
    }
    ton::RootHash vhash{virt_root->get_hash().bits()};
    std::vector<ton::BlockIdExt> prev;
    ton::BlockIdExt mc_blkid;
    bool after_split;
    auto res = block::unpack_block_prev_blk_ext(virt_root, block_id, prev, mc_blkid, after_split);
    if (res.is_error()) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str() << ": " << res);
      return *this;
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info))) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str());
      return *this;
    }
    bool before_split = info.before_split;
    *this << "<div class=\"table-responsive my-3\">\n"
          << "<table class=\"table-sm table-striped\">\n"
          << "<tr><th>block</th><td>" << block_id.id.to_str() << "</td></tr>\n"
          << "<tr><th>roothash</th><td>" << block_id.root_hash.to_hex() << "</td></tr>\n"
          << "<tr><th>filehash</th><td>" << block_id.file_hash.to_hex() << "</td></tr>\n"
          << "<tr><th>time</th><td>" << info.gen_utime << "</td></tr>\n"
          << "<tr><th>lt</th><td>" << info.start_lt << " .. " << info.end_lt << "</td></tr>\n"
          << "<tr><th>global_id</th><td>" << blk.global_id << "</td></tr>\n"
          << "<tr><th>version</th><td>" << info.version << "</td></tr>\n"
          << "<tr><th>flags</th><td>" << info.flags << "</td></tr>\n"
          << "<tr><th>key_block</th><td>" << info.key_block << "</td></tr>\n"
          << "<tr><th>not_master</th><td>" << info.not_master << "</td></tr>\n"
          << "<tr><th>after_merge</th><td>" << info.after_merge << "</td></tr>\n"
          << "<tr><th>after_split</th><td>" << info.after_split << "</td></tr>\n"
          << "<tr><th>before_split</th><td>" << info.before_split << "</td></tr>\n"
          << "<tr><th>want_merge</th><td>" << info.want_merge << "</td></tr>\n"
          << "<tr><th>want_split</th><td>" << info.want_split << "</td></tr>\n"
          << "<tr><th>validator_list_hash_short</th><td>" << info.gen_validator_list_hash_short << "</td></tr>\n"
          << "<tr><th>catchain_seqno</th><td>" << info.gen_catchain_seqno << "</td></tr>\n"
          << "<tr><th>min_ref_mc_seqno</th><td>" << info.min_ref_mc_seqno << "</td></tr>\n"
          << "<tr><th>vert_seqno</th><td>" << info.vert_seq_no << "</td></tr>\n"
          << "<tr><th>vert_seqno_incr</th><td>" << info.vert_seqno_incr << "</td></tr>\n"
          << "<tr><th>prev_key_block_seqno</th><td>"
          << ton::BlockId{ton::masterchainId, ton::shardIdAll, info.prev_key_block_seqno} << "</td></tr>\n";
    for (auto id : prev) {
      *this << "<tr><th>prev block</th><td>" << id << "</td></tr>\n";
    }
    if (!before_split) {
      *this << "<tr><th>next block</th><td>"
            << ton::BlockId{block_id.id.workchain, block_id.id.shard, block_id.id.seqno + 1} << "</td></tr>\n";
    } else {
      *this << "<tr><th>next block</th><td>"
            << ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, true), block_id.id.seqno + 1}
            << "</td></tr>\n";
      *this << "<tr><th>next block</th><td>"
            << ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, false), block_id.id.seqno + 1}
            << "</td></tr>\n";
    }
    *this << "<tr><th>masterchain block</th><td>" << mc_blkid << "</td></tr>\n"
          << "</table></div>";
  } catch (vm::VmError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
    return *this;
  } catch (vm::VmVirtError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
    return *this;
  }

  *this << "<p><a class=\"btn btn-primary mr-2\" href=\"" << BlockDownloadLink{block_id} << "\" download=\""
        << block_id.file_hash << ".boc\">download block</a>"
        << "<a class=\"btn btn-primary\" href=\"" << BlockViewLink{block_id} << "\">view block</a>\n";
  if (block_id.is_masterchain()) {
    *this << "<a class=\"btn btn-primary\" href=\"" << ConfigViewLink{block_id} << "\">view config</a>\n";
  }
  return *this << "</p></div>";
}

HttpAnswer& HttpAnswer::operator<<(BlockShardsCell shards_c) {
  block::ShardConfig sh_conf;
  if (!sh_conf.unpack(vm::load_cell_slice_ref(shards_c.root))) {
    abort("cannot extract shard block list from shard configuration");
    return *this;
  } else {
    auto ids = sh_conf.get_shard_hash_ids(true);

    auto workchain = ton::masterchainId;
    *this << "<div class=\"table-responsive my-3\">\n"
          << "<table class=\"table\">\n<tbody>\n"
          << "<thead>\n"
          << "<tr>\n"
          << "<th scope=\"col\">shard</th>"
          << "<th scope=\"col\">seqno</th>"
          << "<th scope=\"col\">created</th>"
          << "<th scope=\"col\">wantsplit</th>"
          << "<th scope=\"col\">wantmerge</th>"
          << "<th scope=\"col\">beforesplit</th>"
          << "<th scope=\"col\">beforemerge</th>"
          << "</tr>\n"
          << "</thead>\n";
    for (auto id : ids) {
      auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));

      if (id.workchain != workchain) {
        if (workchain != ton::masterchainId) {
          *this << "<tr></tr>\n";
        }
        workchain = id.workchain;
      }
      *this << "<tr>";
      ton::ShardIdFull shard{id.workchain, id.shard};
      if (ref.not_null()) {
        *this << "<td>" << shard.to_str() << "</td><td><a href=\"" << HttpAnswer::BlockLink{ref->top_block_id()}
              << "\">" << ref->top_block_id().id.seqno << "</a></td><td>" << ref->created_at() << "</td>"
              << "<td>" << ref->want_split_ << "</td>"
              << "<td>" << ref->want_merge_ << "</td>"
              << "<td>" << ref->before_split_ << "</td>"
              << "<td>" << ref->before_merge_ << "</td>";
      } else {
        *this << "<td>" << shard.to_str() << "</td>";
      }
      *this << "</tr>";
    }
    return *this << "</tbody></table></div>";
  }
}

HttpAnswer& HttpAnswer::operator<<(AccountLink account) {
  *this << prefix_ << "account?";
  if (account.block_id.is_valid()) {
    block_id_link(account.block_id);
    *this << "&";
  }
  return *this << "account=" << account.account_id.rserialize(true);
}

HttpAnswer& HttpAnswer::operator<<(MessageLink msg) {
  return *this << "#msg" << msg.root->get_hash();
}

HttpAnswer& HttpAnswer::operator<<(TransactionLink trans) {
  return *this << prefix_ << "transaction?"
               << "account=" << trans.account_id.rserialize(true) << "&lt=" << trans.lt << "&hash=" << trans.hash;
}

HttpAnswer& HttpAnswer::operator<<(TransactionLinkShort trans) {
  *this << prefix_ << "transaction2?";
  block_id_link(trans.block_id);
  return *this << "&account=" << trans.account_id.rserialize(true) << "&lt=" << trans.lt;
}

HttpAnswer& HttpAnswer::operator<<(BlockLink block) {
  *this << prefix_ << "block?";
  block_id_link(block.block_id);
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(BlockViewLink block) {
  *this << prefix_ << "viewblock?";
  block_id_link(block.block_id);
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(ConfigViewLink block) {
  *this << prefix_ << "config?";
  block_id_link(block.block_id);
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(BlockDownloadLink block) {
  *this << prefix_ << "download?";
  block_id_link(block.block_id);
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(TransactionList trans) {
  *this << "<div class=\"table-responsive my-3\">\n"
        << "<table class=\"table\">\n<tbody>\n"
        << "<thead>\n"
        << "<tr>\n"
        << "<th scope=\"col\">seq</th>"
        << "<th scope=\"col\">account</th>"
        << "<th scope=\"col\">lt</th>"
        << "<th scope=\"col\">hash</th>"
        << "<th scope=\"col\">link</th>"
        << "</tr>\n"
        << "</thead>\n";
  td::uint32 idx = 0;
  for (auto& x : trans.vec) {
    *this << "<tr><td><a href=\"" << TransactionLink{x.addr, x.lt, x.hash} << "\">" << ++idx << "</a></td>"
          << "<td><a href=\"" << AccountLink{x.addr, trans.block_id} << "\">" << x.addr.rserialize(true) << "</a></td>"
          << "<td>" << x.lt << "</td>"
          << "<td>" << x.hash.to_hex() << "</td>"
          << "<td><a href=\"" << TransactionLink{x.addr, x.lt, x.hash} << "\">view</a></td></tr>";
  }
  if (trans.vec.size() == trans.req_count_) {
    *this << "<tr><td>" << ++idx << "</td>"
          << "<td>more</td>"
          << "<td>more</td>"
          << "<td>more</td></tr>";
  }
  return *this << "</tbody></table></div>";
}

HttpAnswer& HttpAnswer::operator<<(ConfigParam conf) {
  std::ostringstream os;
  *this << "<div id=\"configparam" << conf.idx << "\"><h3>param " << conf.idx << "</h3>";
  if (conf.idx >= 0) {
    *this << RawData<block::gen::ConfigParam>{conf.root, conf.idx};
  } else {
    *this << RawData<void>{conf.root};
  }
  *this << "</div>\n";
  return *this;
}

HttpAnswer& HttpAnswer::operator<<(Error error) {
  return *this << "<div class=\"alert alert-danger\">" << error.error.to_string() << "</div>";
}

HttpAnswer& HttpAnswer::operator<<(Notification n) {
  return *this << "<div class=\"alert alert-success\">" << n.text << "</div>";
}

void HttpAnswer::block_id_link(ton::BlockIdExt block_id) {
  *this << "workchain=" << block_id.id.workchain << "&shard=" << ton::shard_to_str(block_id.id.shard)
        << "&seqno=" << block_id.id.seqno << "&roothash=" << block_id.root_hash << "&filehash=" << block_id.file_hash;
}

std::string HttpAnswer::abort(td::Status error) {
  if (error_.is_ok()) {
    error_ = std::move(error);
  }
  return header() + "<div class=\"alert alert-danger\">" + error_.to_string() + "</div>" + footer();
}

std::string HttpAnswer::abort(std::string error) {
  return abort(td::Status::Error(404, error));
}

std::string HttpAnswer::header() {
  sb_->clear();
  *this << "<!DOCTYPE html>\n"
        << "<html lang=\"en\"><head><meta charset=\"utf-8\"><title>" << title_ << "</title>\n"
        << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, minimum-scale=1.0, "
           "maximum-scale=1.0, user-scalable=no\" />\n"
        << "<meta name=\"format-detection\" content=\"telephone=no\" />\n"
        << "<!-- Latest compiled and minified CSS -->\n"
        << "<link rel=\"stylesheet\" href=\"https://maxcdn.bootstrapcdn.com/bootstrap/4.3.1/css/bootstrap.min.css\">\n"
        << "<!-- jQuery library -->"
        << "<script src=\"https://ajax.googleapis.com/ajax/libs/jquery/3.4.0/jquery.min.js\"></script>\n"
        << "<!-- Popper JS -->\n"
        << "<script src=\"https://cdnjs.cloudflare.com/ajax/libs/popper.js/1.14.7/umd/popper.min.js\"></script>\n"
        << "<!-- Latest compiled JavaScript -->\n"
        << "<script src=\"https://maxcdn.bootstrapcdn.com/bootstrap/4.3.1/js/bootstrap.min.js\"></script>\n"
        << "</head><body>\n"
        << "<div class=\"container-fluid\">\n"
        << "<nav class=\"navbar navbar-expand px-0 mt-1 flex-wrap\">\n"
        << "<ul class=\"navbar-nav ml-1 mr-5 my-1\">\n"
        << "<li class=\"nav-item\"><a class=\"nav-link\" href=\"" << prefix_ << "status\">status</a></li>\n"
        << "<li class=\"nav-item\"><a class=\"nav-link\" href=\"" << prefix_ << "last\">last</a></li>\n"
        << "</ul>";
  *this << "<form class=\"my-1 my-lg-0 flex-grow-1\" action=\"" << prefix_ << "account\" method=\"get\">"
        << "<div class=\"input-group ml-auto\" style=\"max-width:540px;\">"
        << "<input class=\"form-control mr-2 rounded\" type=\"search\" placeholder=\"account\" aria-label=\"account\" "
        << "name=\"account\">";
  *this << "<div class=\"input-group-append\"><button class=\"btn btn-outline-primary rounded\" "
           "type=\"submit\">view</button></div>"
        << "</div></form>"
        << "</nav>\n";

  *this << "<p>\n"
        << "<a class=\"btn btn-primary mt-1\" data-toggle=\"collapse\" href=\"#blocksearch\" role=\"button\" "
           "aria-expanded=\"false\" aria-controls=\"blocksearch\">\n"
        << "Search block\n"
        << "</a>\n"
        << "<a class=\"btn btn-primary mt-1\" data-toggle=\"collapse\" href=\"#accountsearch\" role=\"button\" "
           "aria-expanded=\"false\" aria-controls=\"accountsearch\">\n"
        << "Search account\n"
        << "</a>\n"
        << "<a class=\"btn btn-primary mt-1\" data-toggle=\"collapse\" href=\"#transactionsearch\" role=\"button\" "
           "aria-expanded=\"false\" aria-controls=\"transactionsearch\">\n"
        << "Search transaction\n"
        << "</a>\n"
        << "</p>\n";

  *this << "<div id=\"searchgroup\">\n"
        << "<div class=\"collapse\" data-parent=\"#searchgroup\" id=\"blocksearch\">\n"
        << "<div class=\"card card-body\">\n"
        << BlockSearch{block_id_} << "</div></div>\n";
  *this << "<div class=\"collapse\" data-parent=\"#searchgroup\" id=\"accountsearch\">\n"
        << "<div class=\"card card-body\">\n"
        << AccountSearch{block_id_, account_id_} << "</div></div>\n";
  *this << "<div class=\"collapse\" data-parent=\"#searchgroup\" id=\"transactionsearch\">\n"
        << "<div class=\"card card-body\">\n"
        << TransactionSearch{block_id_, account_id_, 0, ton::Bits256::zero()} << "</div></div></div>\n";

  return sb_->as_cslice().c_str();
}

std::string HttpAnswer::footer() {
  return PSTRING() << "</div></body></html>";
}

std::string HttpAnswer::finish() {
  if (error_.is_ok()) {
    std::string data = sb_->as_cslice().c_str();
    return header() + data + footer();
  } else {
    return header() + "<div class=\"alert alert-danger\">" + error_.to_string() + "</div>" + footer();
  }
}
