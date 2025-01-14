#pragma once
#include "td/actor/PromiseFuture.h"
#include "ton/ton-types.h"

void run_contest_solution(ton::BlockIdExt block_id, td::BufferSlice block_data, td::BufferSlice colldated_data,
                          td::Promise<td::BufferSlice> promise);
