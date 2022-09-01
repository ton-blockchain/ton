#pragma once
#include "ton/ton-types.h"
#include "crypto/vm/cells.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"

namespace emulator {
td::Status emulate_transactions(vm::Dictionary&& libraries, block::Config&& config, block::StdAddress address, ton::UnixTime now,
                                td::Ref<vm::CellSlice>&& shard_account_cell_slice, ton::BlockIdExt cur_block_id, ton::Bits256&& rand_seed,
                                std::vector<block::gen::Transaction::Record>&& transactions,
                                td::int64& balance, ton::UnixTime& storage_last_paid, vm::CellStorageStat& storage_stat,
                                td::Ref<vm::Cell>& code, td::Ref<vm::Cell>& data, td::Ref<vm::Cell>& state,
                                std::string& frozen_hash, ton::LogicalTime& last_trans_lt,
                                ton::Bits256& last_trans_hash, td::uint32& gen_utime, ton::BlockIdExt& block_id);
}  // namespace emulator

