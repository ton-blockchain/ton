#pragma once

#include "base.hpp"

#include "td/utils/JsonBuilder.h"

namespace test::fisherman {

class HeaderCorrupter : public BaseManipulator {
 public:
  struct Config {
    bool distort_timestamp = false;
    td::int32 time_offset = 1'000'000'000;

    bool mark_subshard_of_master = false;
    bool invert_lt = false;
    bool mark_keyblock_on_shard = false;

    bool force_after_merge_for_mc = false;
    bool force_before_split_for_mc = false;
    bool force_after_split_for_mc = false;
    bool allow_both_after_merge_and_split = false;

    bool shard_pfx_zero_yet_after_split = false;

    bool set_vert_seqno_incr = false;

    static Config fromJson(td::JsonValue jv);
  };

  explicit HeaderCorrupter(Config config);
  void modify(block::gen::Block::Record &block) override;

 private:
  Config config_;
};

}  // namespace test::fisherman
