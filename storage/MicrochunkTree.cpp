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
*/

#include "MicrochunkTree.h"
#include "Torrent.h"
#include "vm/cells/CellSlice.h"
#include "vm/cells/MerkleProof.h"

namespace ton {

static td::Ref<vm::Cell> prun(const td::Ref<vm::Cell> &node) {
  vm::CellBuilder cb;
  cb.store_long(static_cast<td::uint8>(vm::Cell::SpecialType::PrunnedBranch), 8);
  cb.store_long(1, 8);
  cb.store_bytes(node->get_hash(0).as_slice());
  cb.store_long(node->get_depth(0), 16);
  return cb.finalize(true);
}

MicrochunkTree::Builder::Builder(td::uint64 file_size, td::uint64 prun_size)
    : file_size_(file_size), prun_size_(prun_size) {
  total_size_ = MICROCHUNK_SIZE;
  while (total_size_ < file_size) {
    total_size_ *= 2;
  }
}

void MicrochunkTree::Builder::add_data(td::Slice s) {
  CHECK(cur_size_ + s.size() <= file_size_);
  while (s.size() > 0) {
    size_t buf_ptr = cur_size_ % MICROCHUNK_SIZE;
    size_t buf_remaining = MICROCHUNK_SIZE - buf_ptr;
    if (buf_remaining > s.size()) {
      memcpy(cur_microchunk_ + buf_ptr, s.data(), s.size());
      cur_size_ += s.size();
      return;
    }
    memcpy(cur_microchunk_ + buf_ptr, s.data(), buf_remaining);
    cur_size_ += buf_remaining;
    s.remove_prefix(buf_remaining);
    add_microchunk(td::Slice(cur_microchunk_, MICROCHUNK_SIZE));
  }
}

MicrochunkTree MicrochunkTree::Builder::finalize() {
  CHECK(cur_size_ == file_size_);
  if (cur_size_ % MICROCHUNK_SIZE != 0) {
    size_t buf_ptr = cur_size_ % MICROCHUNK_SIZE;
    size_t buf_remaining = MICROCHUNK_SIZE - buf_ptr;
    memset(cur_microchunk_ + buf_ptr, 0, buf_remaining);
    cur_size_ += buf_remaining;
    add_microchunk(td::Slice(cur_microchunk_, MICROCHUNK_SIZE));
  }
  memset(cur_microchunk_, 0, MICROCHUNK_SIZE);
  while (cur_size_ < total_size_) {
    add_microchunk(td::Slice(cur_microchunk_, MICROCHUNK_SIZE));
    cur_size_ += MICROCHUNK_SIZE;
  }
  CHECK(proof_.size() == 1);
  MicrochunkTree tree(vm::CellBuilder::create_merkle_proof(std::move(proof_[0])));
  CHECK(tree.total_size_ == total_size_);
  return tree;
}

void MicrochunkTree::Builder::add_microchunk(td::Slice s) {
  CHECK(s.size() == MICROCHUNK_SIZE);
  td::Ref<vm::Cell> node = vm::CellBuilder().store_zeroes(2).store_bytes(s).finalize_novm();
  while (!proof_.empty() && proof_.back()->get_depth(0) == node->get_depth(0)) {
    td::Ref<vm::Cell> left = std::move(proof_.back());
    proof_.pop_back();
    node = vm::CellBuilder().store_zeroes(2).store_ref(std::move(left)).store_ref(std::move(node)).finalize_novm();
    if ((MICROCHUNK_SIZE << node->get_depth(0)) <= prun_size_) {
      node = prun(node);
    }
  }
  proof_.push_back(std::move(node));
}

MicrochunkTree::MicrochunkTree(td::Ref<vm::Cell> root_proof) : root_proof_(root_proof) {
  td::Ref<vm::Cell> virt_root = vm::MerkleProof::virtualize(root_proof_, 1);
  CHECK(!virt_root.is_null());
  CHECK(virt_root->get_depth() <= 50);
  total_size_ = MICROCHUNK_SIZE << virt_root->get_depth();
  root_hash_ = virt_root->get_hash().bits();
}

class GetMicrochunkProof {
 public:
  GetMicrochunkProof(td::uint64 l, td::uint64 r, Torrent &torrent) : l(l), r(r), torrent(torrent) {
  }

  td::Result<td::Ref<vm::Cell>> unprun(td::uint64 il, td::uint64 ir) {
    if (ir - il == MicrochunkTree::MICROCHUNK_SIZE) {
      TRY_RESULT(data, get_microchunk(il));
      return vm::CellBuilder().store_zeroes(2).store_bytes(data).finalize_novm();
    }
    td::uint64 imid = (il + ir) / 2;
    TRY_RESULT(node_l, unprun(il, imid));
    TRY_RESULT(node_r, unprun(imid, ir));
    td::Ref<vm::Cell> node =
        vm::CellBuilder().store_zeroes(2).store_ref(std::move(node_l)).store_ref(std::move(node_r)).finalize_novm();
    if (l >= ir || il >= r) {
      node = prun(node);
    }
    return node;
  }

  td::Result<td::Ref<vm::Cell>> unprun(const td::Ref<vm::Cell> &node, td::uint64 il, td::uint64 ir) {
    vm::CellSlice cs(vm::NoVm(), node);
    if (!cs.is_special()) {
      return node;
    }
    TRY_RESULT(result, unprun(il, ir));
    if (result->get_hash(0) != node->get_hash(0)) {
      return td::Status::Error("Hash mismatch");
    }
    return result;
  }

  td::Result<td::Ref<vm::Cell>> get_proof(td::Ref<vm::Cell> node, td::uint64 il, td::uint64 ir) {
    if (l >= ir || il >= r) {
      return prun(node);
    }
    if (ir - il == MicrochunkTree::MICROCHUNK_SIZE) {
      return unprun(node, il, ir);
    }
    if (l <= il && ir <= r) {
      return prun(node);
    }
    td::uint64 imid = (il + ir) / 2;
    TRY_RESULT_ASSIGN(node, unprun(node, il, ir));
    vm::CellSlice cs(vm::NoVm(), node);
    if (cs.size_ext() != 2 + (2 << 16)) {
      return td::Status::Error("Invalid node in microchunk tree");
    }
    TRY_RESULT(node_l, get_proof(cs.prefetch_ref(0), il, imid));
    TRY_RESULT(node_r, get_proof(cs.prefetch_ref(1), imid, ir));
    return vm::CellBuilder().store_zeroes(2).store_ref(std::move(node_l)).store_ref(std::move(node_r)).finalize_novm();
  }

 private:
  td::uint64 l, r;
  Torrent &torrent;

  td::uint64 cache_offset = 0;
  std::string cache;

  td::Result<td::Slice> get_microchunk(td::uint64 l) {
    DCHECK(l % MicrochunkTree::MICROCHUNK_SIZE == 0);
    td::uint64 r = l + MicrochunkTree::MICROCHUNK_SIZE;
    if (!(cache_offset <= l && r <= cache_offset + cache.size())) {
      td::uint64 piece_size = torrent.get_info().piece_size;
      td::uint64 piece_i = l / piece_size;
      if (piece_i < torrent.get_info().pieces_count()) {
        TRY_RESULT(piece, torrent.get_piece_data(piece_i));
        piece.resize(piece_size, '\0');
        cache = std::move(piece);
      } else {
        cache = std::string(piece_size, '\0');
      }
      cache_offset = piece_i * piece_size;
    }
    return td::Slice{cache.data() + (l - cache_offset), MicrochunkTree::MICROCHUNK_SIZE};
  }
};

td::Result<td::Ref<vm::Cell>> MicrochunkTree::get_proof(td::uint64 l, td::uint64 r, Torrent &torrent) const {
  if (root_proof_.is_null()) {
    return td::Status::Error("Empty microchunk tree");
  }
  if (l % MICROCHUNK_SIZE != 0 || r % MICROCHUNK_SIZE != 0 || l >= r || r > total_size_) {
    return td::Status::Error("Invalid range");
  }
  if (!torrent.inited_info()) {
    return td::Status::Error("Torrent info is not ready");
  }
  if (!torrent.get_info().piece_size % MICROCHUNK_SIZE != 0) {
    return td::Status::Error("Invalid piece size in torrent");
  }
  td::Ref<vm::Cell> root_raw = vm::CellSlice(vm::NoVm(), root_proof_).prefetch_ref();
  TRY_RESULT(result, GetMicrochunkProof(l, r, torrent).get_proof(std::move(root_raw), 0, total_size_));
  return vm::CellBuilder::create_merkle_proof(std::move(result));
}

td::Result<MicrochunkTree> MicrochunkTree::Builder::build_for_torrent(Torrent &torrent, td::uint64 prun_size) {
  if (!torrent.inited_info()) {
    return td::Status::Error("Torrent info is not available");
  }
  const TorrentInfo &info = torrent.get_info();
  Builder builder(info.file_size, prun_size);
  td::uint64 pieces_count = info.pieces_count();
  for (td::uint64 i = 0; i < pieces_count; ++i) {
    TRY_RESULT(piece, torrent.get_piece_data(i));
    builder.add_data(piece);
  }
  MicrochunkTree tree = builder.finalize();
  return tree;
}

}  // namespace ton
