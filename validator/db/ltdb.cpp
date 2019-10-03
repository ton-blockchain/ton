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
#include "ltdb.hpp"
#include "rootdb.hpp"
#include "auto/tl/ton_api.h"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "td/db/RocksDb.h"

namespace ton {

namespace validator {

void LtDb::add_new_block(BlockIdExt block_id, LogicalTime lt, UnixTime ts, td::Promise<td::Unit> promise) {
  auto key = get_desc_key(block_id.shard_full());

  std::string value;
  auto R = kv_->get(key.as_slice(), value);
  R.ensure();
  tl_object_ptr<ton_api::db_lt_desc_value> v;
  bool add_shard = false;
  if (R.move_as_ok() == td::KeyValue::GetStatus::Ok) {
    auto F = fetch_tl_object<ton_api::db_lt_desc_value>(td::BufferSlice{value}, true);
    F.ensure();
    v = F.move_as_ok();
  } else {
    v = create_tl_object<ton_api::db_lt_desc_value>(1, 1, 0, 0, 0);
    add_shard = true;
  }
  if (block_id.id.seqno <= static_cast<td::uint32>(v->last_seqno_) || lt <= static_cast<LogicalTime>(v->last_lt_) ||
      ts <= static_cast<UnixTime>(v->last_ts_)) {
    promise.set_value(td::Unit());
    return;
  }
  auto db_value = create_serialize_tl_object<ton_api::db_lt_el_value>(create_tl_block_id(block_id), lt, ts);
  auto db_key = get_el_key(block_id.shard_full(), v->last_idx_++);
  auto status_key = create_serialize_tl_object<ton_api::db_lt_status_key>();
  v->last_seqno_ = block_id.id.seqno;
  v->last_lt_ = lt;
  v->last_ts_ = ts;

  td::uint32 idx = 0;
  if (add_shard) {
    auto G = kv_->get(status_key.as_slice(), value);
    G.ensure();
    if (G.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
      idx = 0;
    } else {
      auto F = fetch_tl_object<ton_api::db_lt_status_value>(value, true);
      F.ensure();
      auto f = F.move_as_ok();
      idx = f->total_shards_;
    }
  }

  kv_->begin_transaction().ensure();
  kv_->set(key, serialize_tl_object(v, true)).ensure();
  kv_->set(db_key, db_value.as_slice()).ensure();
  if (add_shard) {
    auto shard_key = create_serialize_tl_object<ton_api::db_lt_shard_key>(idx);
    auto shard_value = create_serialize_tl_object<ton_api::db_lt_shard_value>(block_id.id.workchain, block_id.id.shard);
    kv_->set(status_key.as_slice(), create_serialize_tl_object<ton_api::db_lt_status_value>(idx + 1)).ensure();
    kv_->set(shard_key.as_slice(), shard_value.as_slice()).ensure();
  }
  kv_->commit_transaction().ensure();

  promise.set_value(td::Unit());
}

void LtDb::get_block_common(AccountIdPrefixFull account_id,
                            std::function<td::int32(ton_api::db_lt_desc_value &)> compare_desc,
                            std::function<td::int32(ton_api::db_lt_el_value &)> compare, bool exact,
                            td::Promise<BlockIdExt> promise) {
  bool f = false;
  BlockIdExt block_id;
  td::uint32 ls = 0;
  for (td::uint32 len = 0; len <= 60; len++) {
    auto s = shard_prefix(account_id, len);
    auto key = get_desc_key(s);
    std::string value;
    auto F = kv_->get(key, value);
    F.ensure();
    if (F.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
      if (!f) {
        continue;
      } else {
        break;
      }
    }
    f = true;
    auto G = fetch_tl_object<ton_api::db_lt_desc_value>(td::BufferSlice{value}, true);
    G.ensure();
    auto g = G.move_as_ok();
    if (compare_desc(*g.get()) > 0) {
      continue;
    }
    td::uint32 l = g->first_idx_ - 1;
    BlockIdExt lseq;
    td::uint32 r = g->last_idx_;
    BlockIdExt rseq;
    while (r - l > 1) {
      auto x = (r + l) / 2;
      auto db_key = get_el_key(s, x);
      F = kv_->get(db_key, value);
      F.ensure();
      CHECK(F.move_as_ok() == td::KeyValue::GetStatus::Ok);
      auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
      E.ensure();
      auto e = E.move_as_ok();
      int cmp_val = compare(*e.get());

      if (cmp_val < 0) {
        rseq = create_block_id(e->id_);
        r = x;
      } else if (cmp_val > 0) {
        lseq = create_block_id(e->id_);
        l = x;
      } else {
        promise.set_value(create_block_id(e->id_));
        return;
      }
    }
    if (rseq.is_valid()) {
      if (!block_id.is_valid()) {
        block_id = rseq;
      } else if (block_id.id.seqno > rseq.id.seqno) {
        block_id = rseq;
      }
    }
    if (lseq.is_valid()) {
      if (ls < lseq.id.seqno) {
        ls = lseq.id.seqno;
      }
    }
    if (block_id.is_valid() && ls + 1 == block_id.id.seqno) {
      if (!exact) {
        promise.set_value(std::move(block_id));
      } else {
        promise.set_error(td::Status::Error(ErrorCode::notready, "ltdb: block not found"));
      }
      return;
    }
  }
  if (!exact && block_id.is_valid()) {
    promise.set_value(std::move(block_id));
  } else {
    promise.set_error(td::Status::Error(ErrorCode::notready, "ltdb: block not found"));
  }
}

void LtDb::get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<BlockIdExt> promise) {
  return get_block_common(
      account_id,
      [lt](ton_api::db_lt_desc_value &w) {
        return lt > static_cast<LogicalTime>(w.last_lt_) ? 1 : lt == static_cast<LogicalTime>(w.last_lt_) ? 0 : -1;
      },
      [lt](ton_api::db_lt_el_value &w) {
        return lt > static_cast<LogicalTime>(w.lt_) ? 1 : lt == static_cast<LogicalTime>(w.lt_) ? 0 : -1;
      },
      false, std::move(promise));
}

void LtDb::get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno, td::Promise<BlockIdExt> promise) {
  return get_block_common(
      account_id,
      [seqno](ton_api::db_lt_desc_value &w) {
        return seqno > static_cast<BlockSeqno>(w.last_seqno_)
                   ? 1
                   : seqno == static_cast<BlockSeqno>(w.last_seqno_) ? 0 : -1;
      },
      [seqno](ton_api::db_lt_el_value &w) {
        return seqno > static_cast<BlockSeqno>(w.id_->seqno_)
                   ? 1
                   : seqno == static_cast<BlockSeqno>(w.id_->seqno_) ? 0 : -1;
      },
      true, std::move(promise));
}

void LtDb::get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts, td::Promise<BlockIdExt> promise) {
  return get_block_common(
      account_id,
      [ts](ton_api::db_lt_desc_value &w) {
        return ts > static_cast<UnixTime>(w.last_ts_) ? 1 : ts == static_cast<UnixTime>(w.last_ts_) ? 0 : -1;
      },
      [ts](ton_api::db_lt_el_value &w) {
        return ts > static_cast<UnixTime>(w.ts_) ? 1 : ts == static_cast<UnixTime>(w.ts_) ? 0 : -1;
      },
      false, std::move(promise));
}

td::BufferSlice LtDb::get_desc_key(ShardIdFull shard) {
  return create_serialize_tl_object<ton_api::db_lt_desc_key>(shard.workchain, shard.shard);
}

td::BufferSlice LtDb::get_el_key(ShardIdFull shard, td::uint32 idx) {
  return create_serialize_tl_object<ton_api::db_lt_el_key>(shard.workchain, shard.shard, idx);
}

void LtDb::start_up() {
  kv_ = std::make_shared<td::RocksDb>(td::RocksDb::open(db_path_).move_as_ok());
}

void LtDb::truncate_workchain(ShardIdFull shard, td::Ref<MasterchainState> state) {
  auto key = get_desc_key(shard);
  std::string value;
  auto R = kv_->get(key, value);
  R.ensure();
  CHECK(R.move_as_ok() == td::KeyValue::GetStatus::Ok);
  auto F = fetch_tl_object<ton_api::db_lt_desc_value>(td::BufferSlice{value}, true);
  F.ensure();
  auto f = F.move_as_ok();

  auto shards = state->get_shards();
  BlockSeqno seqno = 0;
  if (shard.is_masterchain()) {
    seqno = state->get_seqno();
  } else {
    for (auto s : shards) {
      if (shard_intersects(s->shard(), shard)) {
        seqno = s->top_block_id().seqno();
        break;
      }
    }
  }

  while (f->last_idx_ > f->first_idx_) {
    auto db_key = get_el_key(shard, f->last_idx_ - 1);
    R = kv_->get(db_key, value);
    R.ensure();
    CHECK(R.move_as_ok() == td::KeyValue::GetStatus::Ok);
    auto E = fetch_tl_object<ton_api::db_lt_el_value>(td::BufferSlice{value}, true);
    E.ensure();
    auto e = E.move_as_ok();

    bool to_delete = static_cast<td::uint32>(e->id_->seqno_) > seqno;

    if (!to_delete) {
      break;
    } else {
      f->last_idx_--;
      kv_->erase(db_key).ensure();
    }
  }

  if (f->first_idx_ == f->last_idx_) {
    f->last_ts_ = 0;
    f->last_lt_ = 0;
    f->last_seqno_ = 0;
  }

  kv_->set(key, serialize_tl_object(f, true)).ensure();
}

void LtDb::truncate(td::Ref<MasterchainState> state, td::Promise<td::Unit> promise) {
  auto status_key = create_serialize_tl_object<ton_api::db_lt_status_key>();
  td::Result<td::KeyValue::GetStatus> R;
  td::uint32 total_shards = 0;
  {
    std::string value;
    R = kv_->get(status_key.as_slice(), value);
    R.ensure();
    if (R.move_as_ok() == td::KeyValue::GetStatus::NotFound) {
      promise.set_value(td::Unit());
      return;
    }
    auto F = fetch_tl_object<ton_api::db_lt_status_value>(value, true);
    F.ensure();
    auto f = F.move_as_ok();
    total_shards = f->total_shards_;
    if (total_shards == 0) {
      promise.set_value(td::Unit());
      return;
    }
  }
  kv_->begin_transaction().ensure();
  for (td::uint32 idx = 0; idx < total_shards; idx++) {
    auto shard_key = create_serialize_tl_object<ton_api::db_lt_shard_key>(idx);
    std::string value;
    R = kv_->get(shard_key.as_slice(), value);
    R.ensure();
    CHECK(R.move_as_ok() == td::KeyValue::GetStatus::Ok);
    auto F = fetch_tl_object<ton_api::db_lt_shard_value>(value, true);
    F.ensure();
    auto f = F.move_as_ok();

    truncate_workchain(ShardIdFull{f->workchain_, static_cast<td::uint64>(f->shard_)}, state);
  }
  kv_->commit_transaction().ensure();
  promise.set_value(td::Unit());
}

}  // namespace validator

}  // namespace ton
