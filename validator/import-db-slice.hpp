#pragma once

#include "td/actor/actor.h"
#include "validator/interfaces/validator-manager.h"
#include "validator/db/package.hpp"

namespace ton {

namespace validator {

class ArchiveImporter : public td::actor::Actor {
 public:
  ArchiveImporter(std::string path, td::Ref<MasterchainState> state, BlockSeqno shard_client_seqno,
                  td::Ref<ValidatorManagerOptions> opts, td::actor::ActorId<ValidatorManager> manager,
                  td::Promise<std::vector<BlockSeqno>> promise);
  void start_up() override;

  void abort_query(td::Status error);
  void finish_query();

  void check_masterchain_block(BlockSeqno seqno);
  void checked_masterchain_proof(BlockHandle handle, td::Ref<BlockData> data);
  void applied_masterchain_block(BlockHandle handle);
  void got_new_materchain_state(td::Ref<MasterchainState> state);
  void checked_all_masterchain_blocks(BlockSeqno seqno);

  void check_next_shard_client_seqno(BlockSeqno seqno);
  void got_masterchain_state(td::Ref<MasterchainState> state);
  void apply_shard_block(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont1(BlockHandle handle, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont2(BlockHandle handle, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void apply_shard_block_cont3(BlockHandle handle, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise);
  void check_shard_block_applied(BlockIdExt block_id, td::Promise<td::Unit> promise);

 private:
  std::string path_;
  td::Ref<MasterchainState> state_;
  BlockSeqno shard_client_seqno_;
  BlockSeqno max_shard_client_seqno_;

  td::Ref<ValidatorManagerOptions> opts_;

  std::shared_ptr<Package> package_;

  td::actor::ActorId<ValidatorManager> manager_;
  td::Promise<std::vector<BlockSeqno>> promise_;

  std::map<BlockSeqno, BlockIdExt> masterchain_blocks_;
  std::map<BlockIdExt, std::array<td::uint64, 2>> blocks_;
};

}  // namespace validator

}  // namespace ton
