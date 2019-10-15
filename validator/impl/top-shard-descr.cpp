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

    Copyright 2017-2019 Telegram Systems LLP
*/
#include "top-shard-descr.hpp"
#include "common/errorcode.h"
#include "shard.hpp"
#include "signature-set.hpp"
#include "validator-set.hpp"

#include "vm/cells.h"
#include "vm/cells/MerkleProof.h"
#include "vm/boc.h"
#include "block/block-parse.h"
#include "block/block-auto.h"

namespace ton {

namespace validator {
using td::Ref;
using namespace std::literals::string_literals;

ShardTopBlockDescrQ* ShardTopBlockDescrQ::make_copy() const {
  return new ShardTopBlockDescrQ{*this};
}

td::Status ShardTopBlockDescrQ::unpack_one_proof(BlockIdExt& cur_id, Ref<vm::Cell> proof_root, bool is_head) {
  auto virt_root = vm::MerkleProof::virtualize(proof_root, 1);
  if (virt_root.is_null()) {
    return td::Status::Error(-666, "link for block "s + cur_id.to_str() + " inside ShardTopBlockDescr of " +
                                       block_id_.to_str() +
                                       " does not contain a valid Merkle proof for the block header");
  }
  RootHash virt_hash{virt_root->get_hash().bits()};
  if (virt_hash != cur_id.root_hash) {
    return td::Status::Error(-666, "link for block "s + cur_id.to_str() + " inside ShardTopBlockDescr of " +
                                       block_id_.to_str() +
                                       " contains a Merkle proof with incorrect root hash: expected " +
                                       cur_id.root_hash.to_hex() + ", found " + virt_hash.to_hex());
  }
  bool after_split;
  BlockIdExt mc_blkid;
  auto res = block::unpack_block_prev_blk_try(virt_root, cur_id, link_prev_, mc_blkid, after_split);
  if (res.is_error()) {
    return td::Status::Error(-666, "error in link for block "s + cur_id.to_str() + " inside ShardTopBlockDescr of " +
                                       block_id_.to_str() + ": " + res.move_as_error().to_string());
  }
  block::gen::Block::Record blk;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  block::gen::ValueFlow::Record flow;
  block::CurrencyCollection fees_collected, funds_created;
  if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info) && !info.version &&
        block::gen::t_ValueFlow.force_validate_ref(blk.value_flow) && tlb::unpack_cell(blk.value_flow, flow) &&
        /*tlb::unpack_cell(blk.extra, extra) &&*/ fees_collected.unpack(flow.fees_collected) &&
        funds_created.unpack(flow.r2.created))) {
    return td::Status::Error(-666, "cannot unpack block header in link for block "s + cur_id.to_str());
  }
  // remove this "try ... catch ..." later and uncomment tlb::unpack_cell(blk.extra, extra) in the previous condition
  try {
    if (!tlb::unpack_cell(blk.extra, extra)) {
      return td::Status::Error(-666,
                               "cannot unpack block extra header (BlockExtra) in link for block "s + cur_id.to_str());
    }
  } catch (vm::VmVirtError& err) {
    // backward compatibility with older Proofs / ProofLinks
    LOG(WARNING) << "virtualization error while parsing BlockExtra in proof link of " << cur_id.to_str()
                 << ", setting creator_id to zero: " << err.get_msg();
    extra.created_by.set_zero();
  }
  CHECK(after_split == info.after_split);
  if (info.gen_catchain_seqno != catchain_seqno_) {
    return td::Status::Error(
        -666, PSTRING() << "link for block " << cur_id.to_str()
                        << " is invalid because block header has catchain_seqno = " << info.gen_catchain_seqno
                        << " while ShardTopBlockDescr declares " << catchain_seqno_);
  }
  if (info.gen_validator_list_hash_short != validator_set_hash_) {
    return td::Status::Error(-666, PSTRING() << "link for block " << cur_id.to_str()
                                             << " is invalid because block header has validator_set_hash = "
                                             << info.gen_validator_list_hash_short
                                             << " while ShardTopBlockDescr declares " << validator_set_hash_);
  }
  if (chain_mc_blk_ids_.empty()) {
    after_split_ = info.after_split;
    after_merge_ = info.after_merge;
    before_split_ = info.before_split;
    gen_utime_ = first_gen_utime_ = info.gen_utime;
    vert_seqno_ = info.vert_seq_no;
  } else {
    auto nx_mc_seqno = chain_mc_blk_ids_.back().id.seqno;
    if (nx_mc_seqno < mc_blkid.id.seqno) {
      return td::Status::Error(
          -666, std::string{"link for block "} + cur_id.to_str() + " refers to masterchain block " + mc_blkid.to_str() +
                    " while the next block refers to an older masterchain block " + chain_mc_blk_ids_.back().to_str());
    } else if (nx_mc_seqno == mc_blkid.id.seqno && chain_mc_blk_ids_.back() != mc_blkid) {
      return td::Status::Error(-666, std::string{"link for block "} + cur_id.to_str() +
                                         " refers to masterchain block " + mc_blkid.to_str() +
                                         " while the next block refers to a different same height masterchain block " +
                                         chain_mc_blk_ids_.back().to_str());
    }
    if (info.before_split) {
      return td::Status::Error(
          -666, std::string{"intermediate link for block "} + cur_id.to_str() + " is declared to be before a split");
    }
    if (info.gen_utime > first_gen_utime_) {
      return td::Status::Error(-666, PSTRING() << "block creation unixtime goes back from " << info.gen_utime << " to "
                                               << first_gen_utime_ << " in intermediate link for blocks "
                                               << cur_id.to_str() << " and " << chain_blk_ids_.back().to_str());
    }
    first_gen_utime_ = info.gen_utime;
    if (vert_seqno_ != BlockSeqno(info.vert_seq_no)) {
      return td::Status::Error(-666, PSTRING() << "intermediate link for block " << cur_id.to_str()
                                               << " has vertical seqno " << info.vert_seq_no
                                               << " distinct from the final value in chain " << vert_seqno_);
    }
  }
  chain_mc_blk_ids_.push_back(mc_blkid);
  chain_blk_ids_.push_back(cur_id);
  chain_fees_.emplace_back(std::move(fees_collected), std::move(funds_created));
  creators_.push_back(extra.created_by);
  if (!is_head) {
    if (info.after_split || info.after_merge) {
      return td::Status::Error(
          -666, std::string{"intermediate link for block "} + cur_id.to_str() + " is after a split or a merge");
    }
    CHECK(link_prev_.size() == 1);
    CHECK(link_prev_[0].id.shard == cur_id.id.shard);
    if (link_prev_[0].id.seqno + 1 != cur_id.id.seqno) {
      return td::Status::Error(-666, std::string{"intermediate link for block "} + cur_id.to_str() +
                                         " increases seqno by more than one from " + link_prev_[0].to_str());
    }
    cur_id = link_prev_[0];
  } else {
    hd_after_split_ = info.after_split;
    hd_after_merge_ = info.after_merge;
    CHECK(link_prev_.size() == 1U + info.after_merge);
    BlockSeqno sq = link_prev_[0].id.seqno;
    if (hd_after_merge_) {
      sq = std::max(sq, link_prev_[1].id.seqno);
    }
    if (sq + 1 != cur_id.id.seqno) {
      return td::Status::Error(
          -666, std::string{"initial link for block "} + cur_id.to_str() + " increases seqno by more than one from " +
                    link_prev_[0].to_str() +
                    (hd_after_merge_ ? std::string{" + "} + link_prev_[1].to_str() : std::string{""}));
    }
  }
  return td::Status::OK();
}

td::Status ShardTopBlockDescrQ::unpack() {
  if (root_.is_null()) {
    if (data_.empty()) {
      return td::Status::Error(-666, "Shard top block description has no serialized data and no root cell");
    }
    auto res = vm::std_boc_deserialize(data_.clone());
    if (res.is_error()) {
      return res.move_as_error();
    }
    root_ = res.move_as_ok();
  }
  block::gen::TopBlockDescr::Record rec;
  if (!(block::gen::t_TopBlockDescr.force_validate_ref(root_) && tlb::unpack_cell(root_, rec) &&
        block::tlb::t_BlockIdExt.unpack(rec.proof_for.write(), block_id_))) {
    std::cerr << "invalid ShardTopBlockDescr: ";
    block::gen::t_TopBlockDescr.print_ref(std::cerr, root_);
    vm::load_cell_slice(root_).print_rec(std::cerr);
    return td::Status::Error(-666, "Shard top block description is not a valid TopBlockDescr TL-B object");
  }
  LOG(DEBUG) << "unpacking a ShardTopBlockDescr for " << block_id_.to_str() << " with " << rec.len << " links";
  CHECK(rec.len > 0 && rec.len <= 8);
  // unpack signatures
  Ref<vm::Cell> sig_root = rec.signatures->prefetch_ref();
  if (sig_root.not_null()) {
    vm::CellSlice cs{vm::NoVmOrd(), sig_root};
    bool have_sig;
    if (!(cs.fetch_ulong(8) == 0x11                     // block_signatures#11
          && cs.fetch_uint_to(32, validator_set_hash_)  // validator_set_hash:uint32
          && cs.fetch_uint_to(32, catchain_seqno_)      // catchain_seqno:uint32
          && cs.fetch_uint_to(32, sig_count_)           // sig_count:uint32
          && cs.fetch_uint_to(64, sig_weight_)          // sig_weight:uint64
          && cs.fetch_bool_to(have_sig) && have_sig == (sig_count_ > 0) &&
          cs.size_ext() == ((unsigned)have_sig << 16))) {
      return td::Status::Error(
          -666, std::string{"cannot parse BlockSignatures in ShardTopBlockDescr for "} + block_id_.to_str());
    }
    sig_root_ = cs.prefetch_ref();
    sig_set_ = BlockSignatureSetQ::fetch(sig_root_);
    if (sig_set_.is_null() && sig_count_) {
      return td::Status::Error(
          -666, std::string{"cannot deserialize signature list in ShardTopBlockDescr for "} + block_id_.to_str());
    }
  } else {
    validator_set_hash_ = 0;
    catchain_seqno_ = 0;
    sig_count_ = 0;
    sig_weight_ = 0;
    sig_root_.clear();
  }
  if (!sig_count_ && !is_fake_) {
    return td::Status::Error(-666, std::string{"invalid BlockSignatures in ShardTopBlockDescr for "} +
                                       block_id_.to_str() + ": no signatures present, and fake mode is not enabled");
  }
  is_fake_ = !sig_count_;
  // unpack proof link chain
  auto chain = std::move(rec.chain);
  BlockIdExt cur_id = block_id_;
  for (int i = 0; i < rec.len; i++) {
    CHECK(chain->size_ext() == (i == rec.len - 1 ? 0x10000u : 0x20000u));
    auto proof = chain->prefetch_ref();
    proof_roots_.push_back(proof);
    if (i < rec.len - 1) {
      chain = vm::load_cell_slice_ref(chain->prefetch_ref(1));
    }
    try {
      auto res = unpack_one_proof(cur_id, std::move(proof), i == rec.len - 1);
      if (res.is_error()) {
        return res;
      }
    } catch (vm::VmError& err) {
      return td::Status::Error("error unpacking proof link for "s + cur_id.to_str() + " in ShardTopBlockDescr for " +
                               block_id_.to_str() + " : " + err.get_msg());
    } catch (vm::VmVirtError& err) {
      return td::Status::Error("virtualization error unpacking proof link for "s + cur_id.to_str() +
                               " in ShardTopBlockDescr for " + block_id_.to_str() + " : " + err.get_msg());
    }
  }
  is_valid_ = true;
  return td::Status::OK();
}

td::Result<Ref<ShardTopBlockDescrQ>> ShardTopBlockDescrQ::fetch(td::BufferSlice data, bool is_fake) {
  Ref<ShardTopBlockDescrQ> ref{true, std::move(data), is_fake};
  auto err = ref.unique_write().unpack();
  if (err.is_error()) {
    return err;
  } else {
    return std::move(ref);
  }
}

td::Result<Ref<ShardTopBlockDescrQ>> ShardTopBlockDescrQ::fetch(Ref<vm::Cell> root, bool is_fake) {
  Ref<ShardTopBlockDescrQ> ref{true, std::move(root), is_fake};
  auto err = ref.unique_write().unpack();
  if (err.is_error()) {
    return err;
  } else {
    return std::move(ref);
  }
}

bool ShardTopBlockDescrQ::may_be_valid(BlockHandle last_masterchain_block_handle,
                                       Ref<MasterchainState> last_masterchain_block_state) const {
  int res_flags = 0;
  return prevalidate(last_masterchain_block_handle->id(), std::move(last_masterchain_block_state),
                     Mode::allow_next_vset, res_flags)
      .is_ok();
}

td::Result<int> ShardTopBlockDescrQ::validate_internal(BlockIdExt last_mc_block_id, Ref<MasterchainState> last_mc_state,
                                                       int& res_flags, int mode) const {
  if (!is_valid()) {
    return td::Status::Error(-666, "ShardTopBlockDescr is invalid or uninitialized");
  }
  CHECK(chain_blk_ids_.size() > 0 && chain_blk_ids_.size() <= 8);
  CHECK(chain_mc_blk_ids_.size() == chain_blk_ids_.size());
  Ref<MasterchainStateQ> state = Ref<MasterchainStateQ>(last_mc_state);
  if (state.is_null()) {
    return td::Status::Error(-666, "cannot validate ShardTopBlockDescr: no masterchain state given");
  }
  if (last_mc_block_id.id.seqno < chain_mc_blk_ids_[0].id.seqno) {
    BlockSeqno delta = chain_mc_blk_ids_[0].id.seqno - last_mc_block_id.id.seqno;
    // too new
    if ((mode & Mode::fail_new) || (delta > 8 && (mode & Mode::fail_too_new))) {
      return td::Status::Error(-666, "ShardTopBlockDescr for "s + block_id_.to_str() +
                                         " is too new for us: it refers to masterchain block " +
                                         chain_mc_blk_ids_[0].id.to_str() + " but we know only " +
                                         last_mc_block_id.to_str());
    }
    return -1;  // valid, but too new
  }
  auto config = state->get_config();
  if (config->get_vert_seqno() != vert_seqno_) {
    if (vert_seqno_ < config->get_vert_seqno()) {
      return td::Status::Error(-666, PSTRING() << "ShardTopBlockDescr for " << block_id_.to_str()
                                               << " is too old: it has vertical seqno " << vert_seqno_
                                               << " but we already know about " << config->get_vert_seqno());
    }
    if (mode & Mode::fail_new) {
      return td::Status::Error(-666, PSTRING() << "ShardTopBlockDescr for " << block_id_.to_str()
                                               << " is too new for us: it has vertical seqno " << vert_seqno_
                                               << " but we know only about " << config->get_vert_seqno());
    }
  }
  BlockSeqno next_mc_seqno = ~BlockSeqno(0);
  for (const auto& mcid : chain_mc_blk_ids_) {
    if (mcid.id.seqno > next_mc_seqno) {
      res_flags |= 1;  // permanently invalid
      return td::Status::Error(
          -666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                    " is invalid because its chain refers to masterchain blocks with non-monotonic seqno");
    }
    next_mc_seqno = mcid.id.seqno;
    auto valid =
        (mcid.id.seqno == last_mc_block_id.id.seqno) ? (mcid == last_mc_block_id) : config->check_old_mc_block_id(mcid);
    if (!valid) {
      res_flags |= 1;  // permanently invalid
      return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                         " is invalid because it refers to masterchain block " + mcid.to_str() +
                                         " which is not an ancestor of our block " + last_mc_block_id.to_str());
    }
  }
  auto oldl = config->get_shard_hash(ShardIdFull{block_id_.id.workchain, block_id_.id.shard - 1}, false);
  if (oldl.is_null()) {
    return td::Status::Error(
        -666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                  " is invalid or too new because this workchain is absent from known masterchain configuration");
  }
  if (oldl->seqno() >= block_id_.id.seqno) {
    // we know a shardchain block that it is at least as new as this one
    if (!(mode & allow_old)) {
      res_flags |= 1;  // permanently invalidate unless old ShardTopBlockDescr are allowed
    }
    return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                       " is too old: we already know a newer shardchain block " + oldl->blk_.to_str());
  }
  if (oldl->seqno() < link_prev_[0].id.seqno) {
    if (mode & Mode::fail_new) {
      return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                         " is too new for us: it starts from shardchain block " +
                                         link_prev_[0].id.to_str() + " but we know only " + oldl->blk_.to_str());
    }
    return -1;  // valid, but too new
  }
  auto oldr = oldl;
  if (ton::shard_is_proper_ancestor(shard(), oldl->shard())) {
    oldr = config->get_shard_hash(ShardIdFull{block_id_.id.workchain, block_id_.id.shard + 1}, false);
    if (oldr.is_null()) {
      return td::Status::Error(
          -666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                    " is invalid or too new because this workchain is absent from known masterchain configuration (?)");
    }
    if (oldr->seqno() >= block_id_.id.seqno) {
      // we know a shardchain block that it is at least as new as this one
      res_flags |= 1;  // permanently invalidate unless old ShardTopBlockDescr are allowed
      return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                         " is invalid in a strange fashion: we already know a newer shardchain block " +
                                         oldr->blk_.to_str() +
                                         " but only in the right branch; corresponds to a shardchain fork?");
    }
    CHECK(ton::shard_is_proper_ancestor(shard(), oldr->shard()));
    CHECK(oldl->shard() < oldr->shard());
  } else {
    CHECK(ton::shard_is_ancestor(oldl->shard(), shard()));
  }
  if (oldr->seqno() < link_prev_.back().id.seqno) {
    if (mode & Mode::fail_new) {
      return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                         " is too new for us: it starts from shardchain block " +
                                         link_prev_.back().id.to_str() + " but we know only " + oldr->blk_.to_str());
    }
    return -1;  // valid, but too new
  }
  unsigned clen = block_id_.id.seqno - std::max(oldl->seqno(), oldr->seqno());
  CHECK(clen > 0 && clen <= 8);
  CHECK(clen <= size());
  if (clen < size()) {
    if (chain_blk_ids_[clen] != oldl->blk_) {
      res_flags |= 1;
      return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                         " is invalid: it contains a reference to its ancestor " +
                                         chain_blk_ids_[clen].to_str() +
                                         " but the masterchain refers to another shardchain block " +
                                         (oldl->seqno() < oldr->seqno() ? oldr->blk_.to_str() : oldl->blk_.to_str()) +
                                         " of the same height");
    }
    CHECK(oldl->shard() == shard());
    CHECK(oldl == oldr);
  } else {
    if (link_prev_[0] != oldl->blk_) {
      res_flags |= 1;
      return td::Status::Error(
          -666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                    " is invalid: it contains a reference to its ancestor " + link_prev_[0].to_str() +
                    " but the masterchain instead refers to another shardchain block " + oldl->blk_.to_str());
    }
    if (link_prev_.back() != oldr->blk_) {
      res_flags |= 1;
      return td::Status::Error(
          -666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                    " is invalid: it contains a reference to its ancestor " + link_prev_.back().to_str() +
                    " but the masterchain instead refers to another shardchain block " + oldr->blk_.to_str());
    }
  }
  LOG(DEBUG) << "ShardTopBlockDescr for " << block_id_.to_str() << " appears to have a valid chain of " << clen
             << " new links out of " << size();
  // check validator_set_{ts,hash}
  int vset_ok = 0;
  auto vset = state->get_validator_set(shard());
  CHECK(vset.not_null());
  if (vset->get_catchain_seqno() == catchain_seqno_ && vset->get_validator_set_hash() == validator_set_hash_) {
    res_flags |= 4;
    vset_ok = 1;
  } else if (mode & allow_next_vset) {
    auto nvset = state->get_next_validator_set(shard());
    if (nvset->get_catchain_seqno() == catchain_seqno_ && nvset->get_validator_set_hash() == validator_set_hash_) {
      vset = std::move(nvset);
      res_flags |= 8;
      vset_ok = 2;
    }
  }
  if (!vset_ok) {
    res_flags |= 1;
    return td::Status::Error(-666, PSTRING()
                                       << "ShardTopBlockDescr for " << block_id_.to_str()
                                       << " is invalid because it refers to shard validator set with hash "
                                       << validator_set_hash_ << " and catchain_seqno " << catchain_seqno_
                                       << " while the current masterchain configuration expects "
                                       << vset->get_validator_set_hash() << " and " << vset->get_catchain_seqno());
  }
  // check signatures
  if ((mode & skip_check_sig) || is_fake_ || sig_ok_) {
    return (int)clen;
  }
  if (sig_bad_) {
    return td::Status::Error(
        -666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() + " does not have valid signatures");
  }
  CHECK(sig_set_.not_null());
  auto sig_chk = vset->check_signatures(block_id_.root_hash, block_id_.file_hash, sig_set_);
  if (sig_chk.is_error()) {
    res_flags |= 0x21;
    return td::Status::Error(-666, std::string{"ShardTopBlockDescr for "} + block_id_.to_str() +
                                       " does not have valid signatures: " + sig_chk.move_as_error().to_string());
  }
  res_flags |= 0x10;  // signatures checked ok
  auto wt = sig_chk.move_as_ok();
  if (wt != sig_weight_) {
    res_flags |= 1;
    return td::Status::Error(-666, PSTRING() << "ShardTopBlockDescr for " << block_id_.to_str()
                                             << " has incorrect signature weight " << sig_weight_
                                             << " (actual weight is " << wt << ")");
  }
  LOG(DEBUG) << "ShardTopBlockDescr for " << block_id_.to_str() << " has valid validator signatures of total weight "
             << sig_weight_ << " out of " << Ref<ValidatorSetQ>(vset)->get_total_weight();
  return (int)clen;
}

td::Result<int> ShardTopBlockDescrQ::prevalidate(BlockIdExt last_mc_block_id, Ref<MasterchainState> last_mc_state,
                                                 int mode, int& res_flags) const {
  res_flags = 0;
  auto res = validate_internal(last_mc_block_id, last_mc_state, res_flags, mode);
  return res;
}

td::Result<int> ShardTopBlockDescrQ::validate(BlockIdExt last_mc_block_id, Ref<MasterchainState> last_mc_state,
                                              int mode, int& res_flags) {
  res_flags = 0;
  auto res = validate_internal(last_mc_block_id, last_mc_state, res_flags, mode);
  if (res_flags & 1) {
    // permanently invalid
    is_valid_ = false;
  }
  if (res_flags & 0x10) {
    sig_ok_ = true;
  }
  if (res_flags & 0x20) {
    sig_bad_ = true;
  }
  if (res_flags & 4) {
    vset_cur_ = true;
    vset_next_ = false;
  } else if (res_flags & 8) {
    vset_cur_ = false;
    vset_next_ = true;
  }
  return res;
}

std::vector<BlockIdExt> ShardTopBlockDescrQ::get_prev_at(int pos) const {
  if (!is_valid() || pos < 0 || (unsigned)pos > size()) {
    return {};
  }
  if ((unsigned)pos < size()) {
    return {chain_blk_ids_.at(pos)};
  } else {
    return link_prev_;
  }
}

Ref<block::McShardHash> ShardTopBlockDescrQ::get_prev_descr(int pos, int sum_cnt) const {
  if (!is_valid() || pos < 0 || sum_cnt < 0 || (unsigned)pos >= size() || (unsigned)sum_cnt > size() ||
      (unsigned)(pos + sum_cnt) > size()) {
    return {};
  }
  auto virt_root = vm::MerkleProof::virtualize(proof_roots_.at(pos), 1);
  auto res = block::McShardHash::from_block(std::move(virt_root), chain_blk_ids_.at(pos).file_hash);
  if (res.not_null()) {
    auto& total_fees = res.write().fees_collected_;
    auto& funds_created = res.write().funds_created_;
    total_fees.set_zero();
    funds_created.set_zero();
    for (int i = 0; i < sum_cnt; i++) {
      total_fees += chain_fees_.at(pos + i).first;
      funds_created += chain_fees_[pos + i].second;
    }
  }
  return res;
}

std::vector<td::Bits256> ShardTopBlockDescrQ::get_creator_list(int count) const {
  if (!is_valid() || count < 0 || (unsigned)count > size()) {
    return {};
  }
  std::vector<td::Bits256> res;
  for (int i = count - 1; i >= 0; i--) {
    res.push_back(creators_.at(i));
  }
  return res;
}

void ValidateShardTopBlockDescr::finish_query() {
  if (promise_) {
    promise_.set_value(std::move(descr_));
  }
  stop();
}

void ValidateShardTopBlockDescr::abort_query(td::Status reason) {
  if (promise_) {
    promise_.set_error(std::move(reason));
  }
  stop();
}

bool ValidateShardTopBlockDescr::fatal_error(td::Status error) {
  abort_query(std::move(error));
  return false;
}

bool ValidateShardTopBlockDescr::fatal_error(std::string err_msg, int err_code) {
  return fatal_error(td::Status::Error(err_code, err_msg));
}

void ValidateShardTopBlockDescr::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout in ValidateShardTopBlockDescr"));
}

void ValidateShardTopBlockDescr::start_up() {
  auto res = ShardTopBlockDescrQ::fetch(std::move(data_), is_fake_);
  if (res.is_error()) {
    abort_query(res.move_as_error());
    return;
  }
  descr_ = res.move_as_ok();
  CHECK(descr_->is_valid());
  int res_flags = 0;
  auto val_res = descr_.write().validate(mc_blkid_, state_, ShardTopBlockDescrQ::Mode::allow_next_vset, res_flags);
  if (val_res.is_error()) {
    abort_query(val_res.move_as_error());
    return;
  }
  CHECK(descr_->is_valid());
  finish_query();
}

}  // namespace validator

}  // namespace ton
