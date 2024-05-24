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
#include "interfaces/proof.h"
#include "interfaces/config.h"
#include "block/block-db.h"
#include "block/mc-config.h"
#include "vm/db/StaticBagOfCellsDb.h"
#include "config.hpp"

namespace ton {
namespace validator {
using td::Ref;

class ProofLinkQ : virtual public ProofLink {
 protected:
  BlockIdExt id_;
  td::BufferSlice data_;

 public:
  ProofLinkQ(const BlockIdExt &id, td::BufferSlice data) : id_(id), data_(std::move(data)) {
  }
  ProofLinkQ *make_copy() const override {
    return new ProofLinkQ(id_, data_.clone());
  }
  BlockIdExt block_id() const override {
    return id_;
  }
  td::BufferSlice data() const override {
    return data_.clone();
  }
  td::Result<BlockSeqno> prev_key_mc_seqno() const override;
  td::Result<td::Ref<ConfigHolder>> get_key_block_config() const override;
  td::Result<BasicHeaderInfo> get_basic_header_info() const override;

  struct VirtualizedProof {
    Ref<vm::Cell> root, sig_root;
    std::shared_ptr<vm::StaticBagOfCellsDb> boc;
    VirtualizedProof() = default;
    VirtualizedProof(Ref<vm::Cell> _vroot, Ref<vm::Cell> _sigroot, std::shared_ptr<vm::StaticBagOfCellsDb> _boc)
        : root(std::move(_vroot)), sig_root(std::move(_sigroot)), boc(std::move(_boc)) {
    }
    void clear() {
      root.clear();
      sig_root.clear();
      boc.reset();
    }
  };
  td::Result<VirtualizedProof> get_virtual_root(bool lazy = false) const;
};

#if TD_MSVC
#pragma warning(push)
#pragma warning(disable : 4250)  // Proof is an interface, so there is no problem here
#endif
class ProofQ : public Proof, public ProofLinkQ {
 public:
  ProofQ(BlockIdExt masterchain_block_id, td::BufferSlice data) : ProofLinkQ(masterchain_block_id, std::move(data)) {
  }
  ProofQ *make_copy() const override {
    return new ProofQ(id_, data_.clone());
  }
  td::Result<Ref<ProofLink>> export_as_proof_link() const override;
  td::Result<Ref<vm::Cell>> get_signatures_root() const;
};
#if TD_MSVC
#pragma warning(pop)
#endif

}  // namespace validator
}  // namespace ton
