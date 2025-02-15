#include "block_reader.hpp"

namespace test::fisherman {

BlockDataLoader::BlockDataLoader(const std::string &db_path) : scheduler_({1}) {
  auto opts = ton::validator::ValidatorManagerOptions::create(ton::BlockIdExt{}, ton::BlockIdExt{});
  scheduler_.run_in_context([&] {
    root_db_actor_ = td::actor::create_actor<ton::validator::RootDb>(
        "RootDbActor", td::actor::ActorId<ton::validator::ValidatorManager>(), db_path, opts);
  });
}

BlockDataLoader::~BlockDataLoader() {
  scheduler_.stop();
}

td::Result<td::Ref<ton::validator::BlockData>> BlockDataLoader::load_block_data(const ton::BlockIdExt &block_id) {
  std::atomic<bool> done{false};
  td::Result<td::Ref<ton::validator::BlockData>> block_data_result;

  scheduler_.run_in_context([&] {
    auto handle_promise = td::PromiseCreator::lambda([&](td::Result<ton::validator::ConstBlockHandle> handle_res) {
      if (handle_res.is_error()) {
        block_data_result = td::Result<td::Ref<ton::validator::BlockData>>(handle_res.move_as_error());
        done = true;
        return;
      }
      auto handle = handle_res.move_as_ok();

      auto data_promise = td::PromiseCreator::lambda([&](td::Result<td::Ref<ton::validator::BlockData>> data_res) {
        block_data_result = std::move(data_res);
        done = true;
      });

      td::actor::send_closure(root_db_actor_, &ton::validator::RootDb::get_block_data, handle, std::move(data_promise));
    });

    td::actor::send_closure(root_db_actor_, &ton::validator::RootDb::get_block_by_seqno,
                            ton::AccountIdPrefixFull{block_id.id.workchain, block_id.id.shard}, block_id.id.seqno,
                            std::move(handle_promise));
  });

  while (!done) {
    scheduler_.run(1);
  }

  return block_data_result;
}

}  // namespace test::fisherman
