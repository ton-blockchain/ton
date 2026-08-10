/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include <optional>

#include "collectors.h"

namespace ton::metrics {

struct ChainSnapshot {
  struct Results {
    td::uint64 ok{0};
    td::uint64 error{0};
  };
  struct Blocks {
    Results master;
    Results shard;
  };
  struct Groups {
    td::uint64 master{0};
    td::uint64 shard{0};
  };

  std::optional<td::uint64> masterchain_seqno;
  std::optional<double> masterchain_block_age_seconds;
  std::optional<td::uint64> shardclient_seqno;
  std::optional<td::uint64> active_shards;
  Blocks collated_blocks;
  Blocks validated_blocks;
  std::optional<Groups> validator_groups;

  void collect(Context ctx) const {
    auto masterchain = ctx.with_name("masterchain");
    {
      auto seqno = masterchain.with_name("seqno");
      seqno.open_family("gauge");
      if (masterchain_seqno) {
        seqno.push(double(*masterchain_seqno));
      }
    }
    {
      auto age = masterchain.with_name("block_age_seconds");
      age.open_family("gauge");
      if (masterchain_block_age_seconds) {
        age.push(*masterchain_block_age_seconds);
      }
    }
    {
      auto shardclient = ctx.with_name("shardclient");
      auto seqno = shardclient.with_name("seqno");
      seqno.open_family("gauge");
      if (shardclient_seqno) {
        seqno.push(double(*shardclient_seqno));
      }
    }
    {
      auto shards = ctx.with_name("active_shards");
      shards.open_family("gauge");
      if (active_shards) {
        shards.push(double(*active_shards));
      }
    }

    auto collect_blocks = [](Context family, const Blocks &blocks) {
      family.open_family("counter", "total");
      family.with_label("chain", "master").with_label("result", "ok").push(double(blocks.master.ok));
      family.with_label("chain", "master").with_label("result", "error").push(double(blocks.master.error));
      family.with_label("chain", "shard").with_label("result", "ok").push(double(blocks.shard.ok));
      family.with_label("chain", "shard").with_label("result", "error").push(double(blocks.shard.error));
    };
    collect_blocks(ctx.with_name("collated_blocks"), collated_blocks);
    collect_blocks(ctx.with_name("validated_blocks"), validated_blocks);

    auto groups = ctx.with_name("validator_groups");
    groups.open_family("gauge");
    if (validator_groups) {
      groups.with_label("chain", "master").push(double(validator_groups->master));
      groups.with_label("chain", "shard").push(double(validator_groups->shard));
    }
  }
};

}  // namespace ton::metrics
