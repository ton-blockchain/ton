#include "solution.hpp"

#include "vm/boc.h"
#include "block-auto.h"
#include "contest-validate-query.hpp"

void run_contest_solution(ton::BlockIdExt block_id, td::BufferSlice block_data, td::BufferSlice colldated_data,
                          td::Promise<td::BufferSlice> promise) {
  TRY_RESULT_PROMISE(promise, root, vm::std_boc_deserialize(block_data));
  block::gen::Block::Record rec;
  if (!block::gen::t_Block.cell_unpack(root, rec)) {
    return promise.set_error(td::Status::Error("failed to unpack block"));
  }
  TRY_RESULT_PROMISE(promise, res, vm::std_boc_serialize(rec.state_update));
  td::actor::create_actor<solution::ContestValidateQuery>(
      "validate", block_id, std::move(block_data), std::move(colldated_data), std::move(promise)).release();
}
