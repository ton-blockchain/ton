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
*/
#pragma once
#include <memory>

#include "../../crypto/block/block.h"
#include "common/bitstring.h"
#include "common/refcnt.hpp"
#include "td/actor/common.h"
#include "td/actor/core/Actor.h"
#include "ton/ton-shard.h"
#include "ton/ton-types.h"
#include "vm/db/CellHashTable.h"
#include "vm/dict.h"

namespace ton::validator {
using td::Ref;

class CollatedDataMerger : public td::actor::Actor {
 public:
  void get_cells(std::vector<vm::CellHash> hashes, td::Promise<td::HashMap<vm::CellHash, Ref<vm::Cell>>> promise);
  void add_cells(Ref<vm::Cell> cell);
  void add_block_candidate(BlockIdExt block_id, Ref<vm::Cell> root, std::vector<Ref<vm::Cell>> collated_roots,
                           td::Promise<td::RealCpuTimer::Time> promise);
  void add_block_candidate_data(BlockIdExt block_id, td::BufferSlice data, td::BufferSlice collated_data,
                                td::Promise<td::RealCpuTimer::Time> promise);

 private:
  struct CellInfo {
    Ref<vm::Cell> cell;
    bool prunned = true;
    bool visited = false;

    void set_cell(const Ref<vm::DataCell>& new_cell);
  };
  td::HashMap<vm::CellHash, CellInfo> cells_;
  std::set<BlockIdExt> blocks_;
};

class CollatedDataDeduplicator {
 public:
  td::Status add_block_candidate(BlockSeqno seqno, td::Slice block_data, td::Slice collated_data);
  bool cell_exists(const vm::CellHash& hash, BlockSeqno seqno);

 private:
  td::HashMap<vm::CellHash, BlockSeqno> cells_;
  std::mutex mutex_;  // TODO: other way to synchronize?
};

}  // namespace ton::validator