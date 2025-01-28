#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "validator/db/rootdb.hpp"
#include "td/actor/actor.h"

namespace test::fisherman {

// TODO: Verify that the database does not get corrupted when reading while the validator is running
class BlockDataLoader {
 public:
  explicit BlockDataLoader(const std::string &db_path);
  ~BlockDataLoader();

  td::Result<td::Ref<ton::validator::BlockData>> load_block_data(const ton::BlockIdExt &block_id);

 private:
  td::actor::Scheduler scheduler_;
  td::actor::ActorOwn<ton::validator::RootDb> root_db_actor_;
};

}  // namespace test::fisherman
