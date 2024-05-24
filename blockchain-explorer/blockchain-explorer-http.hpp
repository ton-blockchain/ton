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

#include "ton/ton-types.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "td/utils/Random.h"
#include "block/block.h"

extern bool local_scripts;

class HttpAnswer {
 public:
  struct MessageCell {
    td::Ref<vm::Cell> root;
  };
  struct AddressCell {
    td::Ref<vm::CellSlice> root;
  };
  struct TransactionCell {
    block::StdAddress addr;
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };
  struct AccountCell {
    block::StdAddress addr;
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
    std::vector<td::Ref<vm::Cell>> q_roots;
  };
  struct BlockHeaderCell {
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };
  struct BlockShardsCell {
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };

  struct AccountLink {
    block::StdAddress account_id;
    ton::BlockIdExt block_id;
  };
  struct MessageLink {
    td::Ref<vm::Cell> root;
  };
  struct TransactionLink {
    block::StdAddress account_id;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  };
  struct TransactionLinkShort {
    ton::BlockIdExt block_id;
    block::StdAddress account_id;
    ton::LogicalTime lt;
  };
  struct BlockLink {
    ton::BlockIdExt block_id;
  };
  struct BlockViewLink {
    ton::BlockIdExt block_id;
  };
  struct ConfigViewLink {
    ton::BlockIdExt block_id;
  };
  struct BlockDownloadLink {
    ton::BlockIdExt block_id;
  };
  struct BlockSearch {
    ton::BlockIdExt block_id;
  };
  struct AccountSearch {
    ton::BlockIdExt block_id;
    block::StdAddress addr;
  };
  struct TransactionSearch {
    ton::BlockIdExt block_id;
    block::StdAddress addr;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  };
  struct TransactionList {
    struct TransactionDescr {
      TransactionDescr(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash)
          : addr(addr), lt(lt), hash(hash) {
      }
      block::StdAddress addr;
      ton::LogicalTime lt;
      ton::Bits256 hash;
    };
    ton::BlockIdExt block_id;
    std::vector<TransactionDescr> vec;
    td::uint32 req_count_;
  };
  struct CodeBlock {
    std::string data;
  };
  struct ConfigParam {
    td::int32 idx;
    td::Ref<vm::Cell> root;
  };
  struct Error {
    td::Status error;
  };
  struct Notification {
    std::string text;
  };
  template <class T>
  struct RawData {
    td::Ref<vm::Cell> root;
    T x;
    template <typename... Args>
    RawData(td::Ref<vm::Cell> root, Args &&... args) : root(std::move(root)), x(std::forward<Args>(args)...) {
    }
  };

 public:
  HttpAnswer(std::string title, std::string prefix) : title_(title), prefix_(prefix) {
    buf_ = td::BufferSlice{1 << 28};
    sb_ = std::make_unique<td::StringBuilder>(buf_.as_slice());
  }

  void set_title(std::string title) {
    title_ = title;
  }
  void set_block_id(ton::BlockIdExt block_id) {
    block_id_ = block_id;
    workchain_id_ = block_id_.id.workchain;
  }
  void set_account_id(block::StdAddress addr) {
    account_id_ = addr;
  }
  void set_workchain(ton::WorkchainId workchain_id) {
    workchain_id_ = workchain_id;
  }

  std::string abort(td::Status error);
  std::string abort(std::string error);

  std::string finish();
  std::string header();
  std::string footer();

  template <typename T>
  HttpAnswer &operator<<(T x) {
    sb() << x;
    return *this;
  }
  td::StringBuilder &sb() {
    return *sb_;
  }
  HttpAnswer &operator<<(td::Bits256 x) {
    sb() << x.to_hex();
    return *this;
  }
  HttpAnswer &operator<<(td::BitString x) {
    sb() << x.to_hex();
    return *this;
  }
  HttpAnswer &operator<<(AddressCell addr);
  HttpAnswer &operator<<(MessageCell msg);
  HttpAnswer &operator<<(ton::BlockIdExt block_id);
  HttpAnswer &operator<<(ton::BlockId block_id);
  HttpAnswer &operator<<(TransactionCell trans);
  HttpAnswer &operator<<(AccountCell trans);
  HttpAnswer &operator<<(BlockHeaderCell head);
  HttpAnswer &operator<<(BlockShardsCell shards);
  HttpAnswer &operator<<(BlockSearch head);
  HttpAnswer &operator<<(AccountSearch head);
  HttpAnswer &operator<<(TransactionSearch head);

  HttpAnswer &operator<<(AccountLink account);
  HttpAnswer &operator<<(MessageLink msg);
  HttpAnswer &operator<<(TransactionLink trans);
  HttpAnswer &operator<<(TransactionLinkShort trans);
  HttpAnswer &operator<<(BlockLink block);
  HttpAnswer &operator<<(BlockViewLink block);
  HttpAnswer &operator<<(ConfigViewLink block);
  HttpAnswer &operator<<(BlockDownloadLink block);

  HttpAnswer &operator<<(Error error);
  HttpAnswer &operator<<(Notification notification);

  HttpAnswer &operator<<(TransactionList trans);
  HttpAnswer &operator<<(CodeBlock block) {
    return *this << "<pre><code>" << block.data << "</code></pre>";
  }
  HttpAnswer &operator<<(ConfigParam conf);

  template <class T>
  HttpAnswer &operator<<(RawData<T> data) {
    std::ostringstream outp;
    data.x.print_ref(outp, data.root);
    vm::load_cell_slice(data.root).print_rec(outp);
    return *this << CodeBlock{outp.str()};
  }

 private:
  void block_id_link(ton::BlockIdExt block_id);

  std::string title_;
  ton::BlockIdExt block_id_;
  ton::WorkchainId workchain_id_ = ton::workchainInvalid;
  block::StdAddress account_id_;

  std::string prefix_;
  td::Status error_;

  std::unique_ptr<td::StringBuilder> sb_;
  td::BufferSlice buf_;
};

template <>
struct HttpAnswer::RawData<void> {
  td::Ref<vm::Cell> root;
  RawData(td::Ref<vm::Cell> root) : root(std::move(root)) {
  }
};
template <>
inline HttpAnswer &HttpAnswer::operator<<(RawData<void> data) {
  std::ostringstream outp;
  vm::load_cell_slice(data.root).print_rec(outp);
  return *this << CodeBlock{outp.str()};
}
