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
#include "catchain-block.hpp"

namespace ton {

namespace catchain {

std::unique_ptr<CatChainBlock> CatChainBlock::create(td::uint32 src, td::uint32 fork, const PublicKeyHash &src_hash,
                                                     CatChainBlockHeight height, const CatChainBlockHash &hash,
                                                     td::SharedSlice payload, CatChainBlock *prev,
                                                     std::vector<CatChainBlock *> deps,
                                                     std::vector<CatChainBlockHeight> vt) {
  return std::make_unique<CatChainBlockImpl>(src, fork, src_hash, height, hash, std::move(payload), prev,
                                             std::move(deps), std::move(vt));
}

CatChainBlockImpl::CatChainBlockImpl(td::uint32 src, td::uint32 fork, const PublicKeyHash &src_hash,
                                     CatChainBlockHeight height, const CatChainBlockHash &hash,
                                     td::SharedSlice payload, CatChainBlock *prev,
                                     std::vector<CatChainBlock *> deps, std::vector<CatChainBlockHeight> vt)
    : src_(src)
    , fork_(fork)
    , src_hash_(src_hash)
    , height_(height)
    , hash_(hash)
    , payload_(std::move(payload))
    , prev_(prev)
    , deps_(std::move(deps))
    , vt_(std::move(vt)) {
}

bool CatChainBlockImpl::is_descendant_of(CatChainBlock *block) {
  td::uint32 fork = block->fork();
  if (fork >= vt_.size()) {
    return false;
  }
  return block->height() <= vt_[fork];
}

}  // namespace catchain

}  // namespace ton
