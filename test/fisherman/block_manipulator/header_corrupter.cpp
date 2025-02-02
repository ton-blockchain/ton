#include "header_corrupter.hpp"

#include "block/block-auto.h"

namespace test::fisherman {

HeaderCorrupter::Config HeaderCorrupter::Config::fromJson(td::JsonValue jv) {
  Config cfg;
  CHECK(jv.type() == td::JsonValue::Type::Object);
  auto &obj = jv.get_object();

  cfg.distort_timestamp = td::get_json_object_bool_field(obj, "distort_timestamp", true, false).move_as_ok();
  cfg.time_offset = td::get_json_object_int_field(obj, "time_offset", true, 999999999).move_as_ok();

  cfg.mark_subshard_of_master =
      td::get_json_object_bool_field(obj, "mark_subshard_of_master", true, false).move_as_ok();
  cfg.invert_lt = td::get_json_object_bool_field(obj, "invert_lt", true, false).move_as_ok();
  cfg.mark_keyblock_on_shard = td::get_json_object_bool_field(obj, "mark_keyblock_on_shard", true, false).move_as_ok();

  cfg.force_after_merge_for_mc = td::get_json_object_bool_field(obj, "force_after_merge_for_mc", true, false).move_as_ok();
  cfg.force_before_split_for_mc = td::get_json_object_bool_field(obj, "force_before_split_for_mc", true, false).move_as_ok();
  cfg.force_after_split_for_mc = td::get_json_object_bool_field(obj, "force_after_split_for_mc", true, false).move_as_ok();
  cfg.allow_both_after_merge_and_split =
      td::get_json_object_bool_field(obj, "allow_both_after_merge_and_split", true, false).move_as_ok();

  cfg.shard_pfx_zero_yet_after_split =
      td::get_json_object_bool_field(obj, "shard_pfx_zero_yet_after_split", true, false).move_as_ok();

  cfg.set_vert_seqno_incr = td::get_json_object_bool_field(obj, "set_vert_seqno_incr", true, false).move_as_ok();
  return cfg;
}

HeaderCorrupter::HeaderCorrupter(Config config) : config_(std::move(config)) {
}

void HeaderCorrupter::modify(block::gen::Block::Record &block) {
  block::gen::BlockInfo::Record info_rec;
  CHECK(block::gen::t_BlockInfo.cell_unpack(block.info, info_rec));

  // 1) distort_timestamp => сдвигаем info_rec.gen_utime
  if (config_.distort_timestamp) {
    info_rec.gen_utime += config_.time_offset;
  }

  // 2) mark_subshard_of_master => если workchain == -1, делаем shard_pfx_bits != 0, то есть блок "подшард" MC
  if (config_.mark_subshard_of_master) {
    block::gen::ShardIdent::Record shard_rec;
    CHECK(block::gen::ShardIdent().unpack(info_rec.shard.write(), shard_rec));
    CHECK(shard_rec.workchain_id == -1 && !info_rec.not_master);
    if (shard_rec.shard_pfx_bits == 0) {
      shard_rec.shard_pfx_bits = 10;
      shard_rec.shard_prefix = 123456ULL;
    }
    vm::CellBuilder cb;
    CHECK(block::gen::t_ShardIdent.pack(cb, shard_rec));
    info_rec.shard.write() = cb.finalize();
  }

  // 3) invert_lt => start_lt >= end_lt
  if (config_.invert_lt) {
    if (info_rec.start_lt < info_rec.end_lt) {
      auto tmp = info_rec.start_lt;
      info_rec.start_lt = info_rec.end_lt;
      info_rec.end_lt = tmp;
    }
  }

  // 4) mark_keyblock_on_shard => если "not_master" = true, то проставим key_block = true
  if (config_.mark_keyblock_on_shard) {
    CHECK(info_rec.not_master);
    info_rec.key_block = true;
  }

  // 5) force_after_merge / force_before_split / force_after_split для MC
  if (config_.force_after_merge_for_mc) {
    CHECK(!info_rec.not_master);
    info_rec.after_merge = true;
  }
  if (config_.force_before_split_for_mc) {
    CHECK(!info_rec.not_master);
    info_rec.before_split = true;
  }
  if (config_.force_after_split_for_mc) {
    CHECK(!info_rec.not_master);
    info_rec.after_split = true;
  }

  // 6) allow_both_after_merge_and_split => ставим after_merge=1 и after_split=1
  if (config_.allow_both_after_merge_and_split) {
    info_rec.after_merge = true;
    info_rec.after_split = true;
  }

  // 7) shard_pfx_zero_yet_after_split => shard_pfx_bits=0, after_split=1
  if (config_.shard_pfx_zero_yet_after_split) {
    info_rec.after_split = true;
    block::gen::ShardIdent::Record shard_rec;
    CHECK(block::gen::ShardIdent().unpack(info_rec.shard.write(), shard_rec));
    shard_rec.shard_pfx_bits = 0;
    vm::CellBuilder cb;
    CHECK(block::gen::t_ShardIdent.pack(cb, shard_rec));
    info_rec.shard.write() = cb.finalize();
  }

  // 8) set_vert_seqno_incr => vert_seqno_incr != 0 => ставим true
  if (config_.set_vert_seqno_incr) {
    info_rec.vert_seq_no = 1;
    info_rec.vert_seqno_incr = true;
    info_rec.prev_vert_ref = info_rec.prev_ref;
  }

  CHECK(block::gen::t_BlockInfo.cell_pack(block.info, info_rec));
}

}  // namespace test::fisherman
