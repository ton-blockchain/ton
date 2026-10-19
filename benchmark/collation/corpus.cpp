#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/collation/corpus.h"
#include "block/block-auto.h"
#include "block/block-db.h"
#include "block/block-parse.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/crypto.h"
#include "td/utils/filesystem.h"
#include "td/utils/misc.h"
#include "td/utils/port/Stat.h"
#include "td/utils/port/path.h"
#include "validator/fabric.h"
#include "validator/impl/external-message.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/dict.h"

namespace bench::collation {
namespace {

constexpr td::Slice kJettonWorkloadName = "wallet-v5-jetton-transfer";
constexpr td::Slice kTransferWorkloadName = "wallet-v5-transfer";
constexpr td::uint32 kWorkloadVersion = 1;
constexpr td::Slice kExternalPattern = "externals/%04d.boc";

td::Result<Workload> parse_workload_name(td::Slice name) {
  if (name == kJettonWorkloadName) {
    return Workload::Jetton;
  }
  if (name == kTransferWorkloadName) {
    return Workload::Transfer;
  }
  return td::Status::Error("unsupported workload description");
}

struct ArtifactInfo {
  std::string path;
  td::uint64 size{0};
  td::Bits256 sha256{};
  std::vector<td::Bits256> root_hashes;
};

struct LoadedArtifact {
  td::BufferSlice data;
  std::vector<td::Ref<vm::Cell>> roots;
};

struct CandidateFacts {
  td::Ref<vm::Cell> root;
  block::gen::Block::Record block;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  ton::ShardIdFull shard;
  std::vector<ton::BlockIdExt> previous;
  ton::BlockIdExt masterchain;
  td::uint64 gen_utime_ms{0};
  td::Bits256 successor_state_root{};
};

struct CandidateWorkloadStats {
  td::uint64 transactions{0};
  td::uint64 gas_used{0};
};

std::string hex256(const td::Bits256& value) {
  return td::hex_encode(value.as_slice());
}

td::Result<td::Bits256> parse_hex256(td::Slice value, td::Slice field) {
  if (value.size() != 64 || std::any_of(value.begin(), value.end(),
                                        [](char c) { return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); })) {
    return td::Status::Error(PSLICE() << field << " must be 64 lowercase hexadecimal characters");
  }
  TRY_RESULT(decoded, td::hex_decode(value));
  td::Bits256 result;
  result.as_slice().copy_from(decoded);
  return result;
}

std::string shard_hex(ton::ShardId shard) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(shard));
  return buffer;
}

td::Result<ton::ShardId> parse_shard_hex(td::Slice value, td::Slice field) {
  if (value.size() != 16) {
    return td::Status::Error(PSLICE() << field << " must be 16 lowercase hexadecimal characters");
  }
  td::uint64 result = 0;
  for (char c : value) {
    unsigned digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<unsigned>(c - 'a' + 10);
    } else {
      return td::Status::Error(PSLICE() << field << " must be lowercase hexadecimal");
    }
    result = (result << 4) | digit;
  }
  if (result == 0) {
    return td::Status::Error(PSLICE() << field << " must be a valid non-zero shard id");
  }
  return static_cast<ton::ShardId>(result);
}

std::string decimal(td::uint64 value) {
  return std::to_string(value);
}

td::Result<td::uint64> parse_decimal(td::Slice value, td::Slice field) {
  if (value.empty() || (value.size() > 1 && value[0] == '0')) {
    return td::Status::Error(PSLICE() << field << " must be a canonical unsigned decimal string");
  }
  td::uint64 result = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return td::Status::Error(PSLICE() << field << " must be a canonical unsigned decimal string");
    }
    const auto digit = static_cast<td::uint64>(c - '0');
    if (result > (std::numeric_limits<td::uint64>::max() - digit) / 10) {
      return td::Status::Error(PSLICE() << field << " exceeds uint64");
    }
    result = result * 10 + digit;
  }
  return result;
}

const td::JsonValue* find_field(const td::JsonObject& object, td::Slice name) {
  for (const auto& field : object.field_values_) {
    if (field.first == name) {
      return &field.second;
    }
  }
  return nullptr;
}

td::Status require_fields(const td::JsonObject& object, std::initializer_list<td::Slice> names, td::Slice description) {
  if (object.field_count() != names.size()) {
    return td::Status::Error(PSLICE() << description << " has an unknown, duplicate, or missing field");
  }
  for (auto name : names) {
    if (!object.has_field(name)) {
      return td::Status::Error(PSLICE() << description << " is missing field " << name);
    }
  }
  return td::Status::OK();
}

td::Result<const td::JsonObject*> required_object(const td::JsonObject& parent, td::Slice name) {
  auto value = find_field(parent, name);
  if (value == nullptr || value->type() != td::JsonValue::Type::Object) {
    return td::Status::Error(PSLICE() << name << " must be an object");
  }
  return &value->get_object();
}

td::Result<td::uint32> required_u32(const td::JsonObject& object, td::Slice name) {
  TRY_RESULT(value, object.get_required_long_field(name));
  if (value < 0 || static_cast<td::uint64>(value) > std::numeric_limits<td::uint32>::max()) {
    return td::Status::Error(PSLICE() << name << " exceeds uint32");
  }
  return static_cast<td::uint32>(value);
}

td::Result<td::uint64> required_decimal(const td::JsonObject& object, td::Slice name) {
  TRY_RESULT(value, object.get_required_string_field(name));
  return parse_decimal(value, name);
}

td::Result<td::Bits256> required_hex256(const td::JsonObject& object, td::Slice name) {
  TRY_RESULT(value, object.get_required_string_field(name));
  return parse_hex256(value, name);
}

td::Result<ton::BlockIdExt> parse_block(const td::JsonObject& object, td::Slice description) {
  TRY_STATUS(require_fields(object, {"workchain", "shard", "seqno", "root_hash", "file_hash"}, description));
  TRY_RESULT(workchain64, object.get_required_long_field("workchain"));
  if (workchain64 < std::numeric_limits<ton::WorkchainId>::min() ||
      workchain64 > std::numeric_limits<ton::WorkchainId>::max()) {
    return td::Status::Error(PSLICE() << description << ".workchain exceeds int32");
  }
  TRY_RESULT(shard_string, object.get_required_string_field("shard"));
  TRY_RESULT(shard, parse_shard_hex(shard_string, PSLICE() << description << ".shard"));
  TRY_RESULT(seqno, required_u32(object, "seqno"));
  TRY_RESULT(root_hash, required_hex256(object, "root_hash"));
  TRY_RESULT(file_hash, required_hex256(object, "file_hash"));
  ton::BlockIdExt result{static_cast<ton::WorkchainId>(workchain64), shard, seqno, ton::RootHash{root_hash},
                         ton::FileHash{file_hash}};
  if (!result.is_valid_full()) {
    return td::Status::Error(PSLICE() << description << " is not a valid full block id");
  }
  return result;
}

auto block_json(const ton::BlockIdExt& id) {
  return td::json_object([&](auto& object) {
    object("workchain", static_cast<td::int64>(id.id.workchain));
    object("shard", shard_hex(id.id.shard));
    object("seqno", static_cast<td::int64>(id.id.seqno));
    object("root_hash", hex256(id.root_hash));
    object("file_hash", hex256(id.file_hash));
  });
}

td::Result<ArtifactInfo> inspect_artifact(td::Slice path, td::Slice bytes) {
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(bytes));
  if (roots.empty() || roots.size() > std::numeric_limits<td::uint32>::max()) {
    return td::Status::Error(PSLICE() << path << " has an invalid root count");
  }
  ArtifactInfo result;
  result.path = path.str();
  result.size = bytes.size();
  result.sha256 = td::sha256_bits256(bytes);
  result.root_hashes.reserve(roots.size());
  for (const auto& root : roots) {
    if (root.is_null()) {
      return td::Status::Error(PSLICE() << path << " has a null root occurrence");
    }
    result.root_hashes.emplace_back(root->get_hash().bits());
  }
  return result;
}

td::Status write_inspected_artifact(const std::string& directory, td::Slice key, td::Slice relative_path,
                                    td::Slice bytes, std::map<std::string, ArtifactInfo>& artifacts) {
  TRY_RESULT(info, inspect_artifact(relative_path, bytes));
  TRY_STATUS(td::atomic_write_file(PSTRING() << directory << '/' << relative_path, bytes));
  auto [it, inserted] = artifacts.emplace(key.str(), std::move(info));
  if (!inserted) {
    return td::Status::Error(PSLICE() << "duplicate corpus artifact key " << key);
  }
  return td::Status::OK();
}

td::Status write_single_root_artifact(const std::string& directory, td::Slice key, td::Slice relative_path,
                                      td::Slice bytes, const td::Ref<vm::Cell>& root,
                                      std::map<std::string, ArtifactInfo>& artifacts) {
  if (root.is_null()) {
    return td::Status::Error(PSLICE() << relative_path << " has a null known root occurrence");
  }
  ArtifactInfo info;
  info.path = relative_path.str();
  info.size = bytes.size();
  info.sha256 = td::sha256_bits256(bytes);
  info.root_hashes.emplace_back(root->get_hash().bits());
  TRY_STATUS(td::atomic_write_file(PSTRING() << directory << '/' << relative_path, bytes));
  if (!artifacts.emplace(key.str(), std::move(info)).second) {
    return td::Status::Error(PSLICE() << "duplicate corpus artifact key " << key);
  }
  return td::Status::OK();
}

std::string external_key(td::uint32 index) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "external_%04u", index);
  return buffer;
}

std::string external_path(td::uint32 index) {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "externals/%04u.boc", index);
  return buffer;
}

td::Result<td::uint64> extract_gen_utime_ms(td::Slice collated_data, bool require_full) {
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(collated_data));
  td::optional<td::uint64> result;
  bool have_proof = false;
  for (const auto& root : roots) {
    bool is_special = false;
    auto slice = vm::load_cell_slice_special(root, is_special);
    if (!slice.is_valid()) {
      return td::Status::Error("cannot inspect collated-data root");
    }
    if (is_special) {
      if (slice.special_type() != vm::Cell::SpecialType::MerkleProof) {
        return td::Status::Error("collated data has an unexpected special root");
      }
      have_proof = true;
      continue;
    }
    have_proof |= block::gen::t_AccountStorageDictProof.has_valid_tag(slice);
    if (!block::gen::t_ConsensusExtraData.has_valid_tag(slice)) {
      continue;
    }
    if (result || !block::gen::t_ConsensusExtraData.validate_ref(10000, root)) {
      return td::Status::Error("collated data has duplicate or invalid ConsensusExtraData");
    }
    block::gen::ConsensusExtraData::Record record;
    if (!tlb::unpack_cell(root, record)) {
      return td::Status::Error("cannot unpack ConsensusExtraData");
    }
    result = record.gen_utime_ms;
  }
  if (!result) {
    return td::Status::Error("collated data has no ConsensusExtraData");
  }
  if (have_proof != require_full) {
    if (require_full) {
      return td::Status::Error("full collated data has no proof roots");
    }
    return td::Status::Error("preloaded collated data still has proof roots");
  }
  return result.value();
}

td::Status verify_canonical_full_shard_ident(td::Ref<vm::CellSlice> encoded, ton::WorkchainId workchain,
                                             td::Slice description) {
  block::gen::ShardIdent::Record raw;
  if (encoded.is_null() || !block::gen::t_ShardIdent.unpack(encoded.write(), raw) || raw.shard_pfx_bits != 0 ||
      raw.workchain_id != workchain || raw.shard_prefix != 0) {
    return td::Status::Error(PSLICE() << description << " is not a canonical full-shard ShardIdent");
  }
  return td::Status::OK();
}

td::Status verify_canonical_full_state(const ton::BlockIdExt& id, const td::Ref<vm::Cell>& root,
                                       td::Slice description) {
  block::gen::ShardStateUnsplit::Record state;
  if (root.is_null() || !tlb::unpack_cell(root, state) || state.seq_no != id.seqno()) {
    return td::Status::Error(PSLICE() << description << " identity does not reproduce its block id");
  }
  return verify_canonical_full_shard_ident(std::move(state.shard_id), id.id.workchain, description);
}

td::Result<CandidateFacts> inspect_candidate(const ton::BlockCandidate& candidate,
                                             const td::Ref<vm::Cell>& previous_state_root,
                                             const ton::BlockIdExt& expected_previous,
                                             const ton::BlockIdExt& expected_masterchain, bool full_collated) {
  if (block::compute_file_hash(candidate.data) != candidate.id.file_hash ||
      block::compute_file_hash(candidate.collated_data) != candidate.collated_file_hash) {
    return td::Status::Error("candidate raw file hash mismatch");
  }
  TRY_RESULT(root, vm::std_boc_deserialize(candidate.data.as_slice()));
  if (td::Bits256{root->get_hash().bits()} != candidate.id.root_hash) {
    return td::Status::Error("candidate block root hash mismatch");
  }

  CandidateFacts facts;
  facts.root = root;
  if (!tlb::unpack_cell(root, facts.block) || !tlb::unpack_cell(facts.block.info, facts.info) ||
      facts.info.version != 0 || !tlb::unpack_cell(facts.block.extra, facts.extra)) {
    return td::Status::Error("cannot unpack candidate block/header/extra");
  }
  auto semantic_shard = facts.info.shard;
  if (semantic_shard.is_null() || !block::tlb::t_ShardIdent.unpack(semantic_shard.write(), facts.shard)) {
    return td::Status::Error("cannot unpack candidate header shard identity");
  }
  TRY_STATUS(verify_canonical_full_shard_ident(facts.info.shard, candidate.id.id.workchain, "candidate header"));
  if (facts.info.seq_no != candidate.id.seqno()) {
    return td::Status::Error("candidate header sequence number does not reproduce its id");
  }
  TRY_RESULT(header, block::get_block_header_info(root, candidate.id));
  facts.previous = std::move(header.prev);
  facts.masterchain = header.mc_blkid;
  if (facts.shard != candidate.id.shard_full() || facts.previous.size() != 1 ||
      facts.previous.front() != expected_previous || facts.masterchain != expected_masterchain || header.after_split ||
      facts.info.before_split || facts.info.after_split || facts.info.after_merge) {
    return td::Status::Error("candidate violates the single-predecessor unsplit corpus contract");
  }
  if (previous_state_root.is_null()) {
    return td::Status::Error("candidate previous state root is null");
  }
  TRY_STATUS(vm::MerkleUpdate::validate(facts.block.state_update));
  TRY_STATUS(vm::MerkleUpdate::may_apply(previous_state_root, facts.block.state_update));
  TRY_RESULT(successor, vm::MerkleUpdate::apply(previous_state_root, facts.block.state_update));
  TRY_STATUS(verify_canonical_full_state(candidate.id, successor, "candidate successor state"));
  facts.successor_state_root = successor->get_hash().bits();
  TRY_RESULT_ASSIGN(facts.gen_utime_ms, extract_gen_utime_ms(candidate.collated_data.as_slice(), full_collated));
  if (facts.gen_utime_ms / 1000 != facts.info.gen_utime) {
    return td::Status::Error("candidate gen_utime and ConsensusExtraData gen_utime_ms disagree");
  }
  return facts;
}

td::Result<CandidateWorkloadStats> inspect_candidate_workload(const CandidateFacts& facts) {
  CandidateWorkloadStats result;
  td::Status status = td::Status::OK();
  try {
    vm::AugmentedDictionary account_blocks{vm::load_cell_slice_ref(facts.extra.account_blocks), 256,
                                           block::tlb::aug_ShardAccountBlocks};
    if (!account_blocks.is_valid()) {
      return td::Status::Error("candidate has an invalid ShardAccountBlocks dictionary");
    }
    const bool account_blocks_ok = account_blocks.check_for_each_extra(
        [&](td::Ref<vm::CellSlice> value, td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
          block::gen::AccountBlock::Record account_block;
          if (!tlb::csr_unpack_safe(std::move(value), account_block)) {
            status = td::Status::Error("cannot unpack candidate AccountBlock");
            return false;
          }
          vm::AugmentedDictionary transactions{vm::DictNonEmpty(), account_block.transactions, 64,
                                               block::tlb::aug_AccountTransactions};
          if (!transactions.is_valid() ||
              !transactions.check_for_each_extra(
                  [&](td::Ref<vm::CellSlice> transaction_value, td::Ref<vm::CellSlice>, td::ConstBitPtr, int) {
                    if (transaction_value.is_null() || transaction_value->size_ext() != 0x10000) {
                      status = td::Status::Error("candidate transaction dictionary has an invalid value");
                      return false;
                    }
                    auto transaction_root = transaction_value->prefetch_ref();
                    block::gen::Transaction::Record transaction;
                    block::gen::TransactionDescr::Record_trans_ord description;
                    if (transaction_root.is_null() || !tlb::unpack_cell(transaction_root, transaction) ||
                        !tlb::unpack_cell(transaction.description, description)) {
                      status = td::Status::Error("cannot unpack candidate ordinary transaction");
                      return false;
                    }
                    vm::CellSlice compute_phase{*description.compute_ph};
                    block::gen::TrComputePhase::Record_tr_phase_compute_vm compute;
                    if (!block::gen::t_TrComputePhase.unpack(compute_phase, compute) || !compute_phase.empty_ext()) {
                      status = td::Status::Error("candidate transaction has no VM compute phase");
                      return false;
                    }
                    const auto gas_used = block::tlb::t_VarUInteger_7.as_uint(*compute.r1.gas_used);
                    if (result.transactions == std::numeric_limits<td::uint64>::max() ||
                        result.gas_used > std::numeric_limits<td::uint64>::max() - gas_used) {
                      status = td::Status::Error("candidate transaction or gas count overflows uint64");
                      return false;
                    }
                    ++result.transactions;
                    result.gas_used += gas_used;
                    return true;
                  })) {
            if (status.is_ok()) {
              status = td::Status::Error("cannot traverse candidate transaction dictionary");
            }
            return false;
          }
          return true;
        });
    if (!account_blocks_ok && status.is_ok()) {
      status = td::Status::Error("cannot traverse candidate AccountBlock dictionary");
    }
  } catch (vm::VmError& error) {
    return error.as_status("cannot inspect candidate transaction dictionaries: ");
  } catch (vm::VmVirtError& error) {
    return error.as_status("cannot inspect virtualized candidate transaction dictionaries: ");
  }
  TRY_STATUS(std::move(status));
  return result;
}

td::Status verify_empty_previous_state(const ton::BlockIdExt& id, const td::Ref<vm::Cell>& root) {
  TRY_STATUS(verify_canonical_full_state(id, root, "previous shard state"));
  block::ShardState state;
  TRY_STATUS(state.unpack_state(id, root));
  if (state.before_split_ || !state.out_msg_queue_ || !state.out_msg_queue_->is_empty() || !state.ihr_pending_ ||
      !state.ihr_pending_->is_empty() || (state.out_msg_queue_size_ && state.out_msg_queue_size_.value() != 0)) {
    return td::Status::Error("previous shard state violates the empty unsplit out-queue contract");
  }
  return td::Status::OK();
}

td::Status verify_block_state_target(const ton::BlockIdExt& block_id, const td::Ref<vm::Cell>& block_root,
                                     const td::Ref<vm::Cell>& state_root) {
  if (block_root.is_null() || state_root.is_null()) {
    return td::Status::Error("cannot bind a null block/state root");
  }
  block::gen::Block::Record block_record;
  block::gen::BlockInfo::Record info;
  ton::ShardIdFull shard;
  if (!tlb::unpack_cell(block_root, block_record) || !tlb::unpack_cell(block_record.info, info) || info.version != 0) {
    return td::Status::Error("cannot unpack block artifact header");
  }
  auto semantic_shard = info.shard;
  if (semantic_shard.is_null() || !block::tlb::t_ShardIdent.unpack(semantic_shard.write(), shard) ||
      shard != block_id.shard_full() || info.seq_no != block_id.seqno()) {
    return td::Status::Error("block artifact header does not reproduce its id");
  }
  TRY_STATUS(verify_canonical_full_shard_ident(info.shard, block_id.id.workchain, "block artifact header"));
  TRY_STATUS(vm::MerkleUpdate::validate(block_record.state_update));
  vm::CellSlice update{vm::NoVm(), block_record.state_update};
  if (!update.is_valid() || update.special_type() != vm::Cell::SpecialType::MerkleUpdate || update.size_refs() != 2 ||
      update.prefetch_ref(1)->get_hash(0) != state_root->get_hash()) {
    return td::Status::Error("block Merkle update target does not match the supplied state root");
  }
  return td::Status::OK();
}

td::Status verify_portable_masterchain_context(const ton::BlockIdExt& block_id, const td::Ref<vm::Cell>& block_root,
                                               const td::Ref<vm::Cell>& state_root) {
  try {
    block::gen::ShardStateUnsplit::Record state;
    block::gen::McStateExtra::Record extra;
    block::gen::ConfigParams::Record params;
    block::gen::ValidatorInfo::Record validator_info;
    if (!tlb::unpack_cell(state_root, state) || state.global_id != -777 || state.custom.is_null() ||
        state.custom->prefetch_ulong(1) != 1 || !tlb::unpack_cell(state.custom->prefetch_ref(), extra) ||
        !tlb::csr_unpack(extra.config, params) || !tlb::csr_unpack(extra.r1.validator_info, validator_info) ||
        !extra.r1.after_key_block || !validator_info.nx_cc_updated) {
      return td::Status::Error("masterchain state is not a self-seeding portable benchmark key state");
    }

    vm::Dictionary effective_config{params.config, 32};
    auto param19 = effective_config.lookup_ref(td::BitArray<32>{19});
    int config_global_id = 0;
    if (param19.is_null() || !block::gen::ConfigParam{19}.cell_unpack_cons19(param19, config_global_id) ||
        config_global_id != state.global_id) {
      return td::Status::Error("masterchain state ConfigParam 19 does not match its global_id");
    }

    vm::AugmentedDictionary accounts{vm::load_cell_slice_ref(state.accounts), 256, block::tlb::aug_ShardAccounts};
    if (!accounts.is_valid()) {
      return td::Status::Error("masterchain state has an invalid ShardAccounts dictionary");
    }
    TRY_RESULT(contract_config, block::get_config_data_from_smc(accounts.lookup(params.config_addr)));
    if (contract_config->get_hash() != params.config->get_hash()) {
      return td::Status::Error("masterchain effective and configuration-contract roots differ");
    }

    block::gen::Block::Record block;
    block::gen::BlockInfo::Record info;
    if (!tlb::unpack_cell(block_root, block) || !tlb::unpack_cell(block.info, info) ||
        block.global_id != state.global_id || !info.key_block || info.seq_no != block_id.seqno()) {
      return td::Status::Error("masterchain block is not the key block that produced its portable state");
    }
    return td::Status::OK();
  } catch (vm::VmError& error) {
    return error.as_status("cannot inspect portable masterchain account/config dictionaries: ");
  } catch (vm::VmVirtError& error) {
    return error.as_status("cannot inspect virtualized portable masterchain account/config dictionaries: ");
  }
}

std::string render_meta(const FixtureConfig& config, const Fixture& fixture, const ton::BlockCandidate& full_candidate,
                        const ton::BlockCandidate& preloaded_candidate, const CandidateFacts& facts,
                        const std::map<std::string, ArtifactInfo>& artifacts, td::uint64 transactions,
                        td::uint64 gas_used) {
  td::JsonBuilder builder;
  auto root = builder.enter_object();
  root("format", kCollationCorpusFormat);
  root("version", static_cast<td::int64>(kCollationCorpusVersion));
  root("workload", td::json_object([&](auto& object) {
         object("name", collation_corpus_workload_name(config.workload));
         object("version", static_cast<td::int64>(kWorkloadVersion));
         object("accounts", static_cast<td::int64>(config.accounts));
         object("transfers", static_cast<td::int64>(config.transfers));
         object("seed", decimal(config.seed));
       }));
  root("contract", td::json_object([&](auto& object) {
         object("workchain", static_cast<td::int64>(fixture.shard.workchain));
         object("shard", shard_hex(fixture.shard.shard));
         object("previous_count", static_cast<td::int64>(1));
         object("before_split", td::JsonBool(false));
         object("after_split", td::JsonBool(false));
         object("after_merge", td::JsonBool(false));
         object("internals_count", static_cast<td::int64>(0));
         object("internals_complete", td::JsonBool(true));
         object("neighbors_count", static_cast<td::int64>(0));
         object("top_blocks_count", static_cast<td::int64>(0));
         object("out_queue_size", decimal(0));
         object("full_collated_data", td::JsonBool(true));
         object("max_external_attempts", static_cast<td::int64>(config.transfers));
         object("storage_stat_cache", td::JsonBool(false));
         object("queue_cleanup_deadline", td::JsonBool(false));
         object("internal_msg_deadline", td::JsonBool(false));
       }));
  root("header", td::json_object([&](auto& object) {
         object("gen_utime", static_cast<td::int64>(facts.info.gen_utime));
         object("gen_utime_ms", decimal(facts.gen_utime_ms));
         object("rand_seed", hex256(td::Bits256{facts.extra.rand_seed.bits()}));
         object("created_by", hex256(td::Bits256{facts.extra.created_by.bits()}));
       }));
  root("previous", block_json(fixture.prev_id));
  root("masterchain", block_json(fixture.mc_id));
  root("candidate", td::json_object([&](auto& object) {
         object("block", block_json(full_candidate.id));
         object("collated_file_hash", hex256(full_candidate.collated_file_hash));
         object("preloaded_collated_file_hash", hex256(preloaded_candidate.collated_file_hash));
         object("state_root_hash", hex256(facts.successor_state_root));
       }));
  root("externals", td::json_object([&](auto& object) {
         object("count", static_cast<td::int64>(fixture.messages.size()));
         object("pattern", kExternalPattern);
       }));
  root("files", td::json_object([&](auto& object) {
         for (const auto& [key, artifact] : artifacts) {
           object(key, td::json_object([&](auto& file) {
                    file("path", artifact.path);
                    file("size", decimal(artifact.size));
                    file("sha256", hex256(artifact.sha256));
                    file("root_count", static_cast<td::int64>(artifact.root_hashes.size()));
                    file("root_hashes",
                         td::json_array(artifact.root_hashes, [](const auto& hash) { return hex256(hash); }));
                  }));
         }
       }));
  root("expected", td::json_object([&](auto& object) {
         object("transactions", static_cast<td::int64>(transactions));
         object("gas_used", decimal(gas_used));
         object("block_bytes", decimal(full_candidate.data.size()));
         object("collated_full_bytes", decimal(full_candidate.collated_data.size()));
         object("collated_preloaded_bytes", decimal(preloaded_candidate.collated_data.size()));
       }));
  root.leave();
  return builder.string_builder().as_cslice().str();
}

td::Result<ArtifactInfo> parse_artifact_info(const td::JsonObject& object, td::Slice key) {
  TRY_STATUS(
      require_fields(object, {"path", "size", "sha256", "root_count", "root_hashes"}, PSLICE() << "files." << key));
  ArtifactInfo result;
  TRY_RESULT_ASSIGN(result.path, object.get_required_string_field("path"));
  TRY_RESULT_ASSIGN(result.size, required_decimal(object, "size"));
  TRY_RESULT_ASSIGN(result.sha256, required_hex256(object, "sha256"));
  TRY_RESULT(root_count, required_u32(object, "root_count"));
  auto hashes = find_field(object, "root_hashes");
  if (hashes == nullptr || hashes->type() != td::JsonValue::Type::Array || hashes->get_array().size() != root_count ||
      root_count == 0) {
    return td::Status::Error(PSLICE() << "files." << key << ".root_hashes has the wrong length");
  }
  result.root_hashes.reserve(root_count);
  for (const auto& hash : hashes->get_array()) {
    if (hash.type() != td::JsonValue::Type::String) {
      return td::Status::Error(PSLICE() << "files." << key << ".root_hashes must contain strings");
    }
    TRY_RESULT(parsed, parse_hex256(hash.get_string(), PSLICE() << "files." << key << ".root_hashes"));
    result.root_hashes.push_back(parsed);
  }
  return result;
}

td::Result<LoadedArtifact> load_artifact(const std::string& directory, td::Slice key, td::Slice required_path,
                                         const std::map<std::string, ArtifactInfo>& artifacts) {
  auto it = artifacts.find(key.str());
  if (it == artifacts.end()) {
    return td::Status::Error(PSLICE() << "missing artifact " << key);
  }
  const auto& expected = it->second;
  if (expected.path != required_path) {
    return td::Status::Error(PSLICE() << "artifact " << key << " must use path " << required_path);
  }
  TRY_RESULT(data, td::read_file(PSTRING() << directory << '/' << required_path));
  if (data.size() != expected.size || td::sha256_bits256(data.as_slice()) != expected.sha256) {
    return td::Status::Error(PSLICE() << "artifact " << key << " byte size or SHA256 mismatch");
  }
  TRY_RESULT(roots, vm::std_boc_deserialize_multi(data.as_slice()));
  if (roots.size() != expected.root_hashes.size()) {
    return td::Status::Error(PSLICE() << "artifact " << key << " root occurrence count mismatch");
  }
  for (size_t i = 0; i < roots.size(); ++i) {
    if (roots[i].is_null() || td::Bits256{roots[i]->get_hash().bits()} != expected.root_hashes[i]) {
      return td::Status::Error(PSLICE() << "artifact " << key << " root occurrence #" << i << " hash mismatch");
    }
  }
  return LoadedArtifact{std::move(data), std::move(roots)};
}

td::Result<std::string> require_foreign_candidate_directory(td::CSlice directory) {
  if (directory.empty()) {
    return td::Status::Error("foreign candidate directory must not be empty");
  }
  TRY_RESULT(canonical, td::realpath(directory));
  TRY_RESULT(stat, td::stat(canonical));
  if (!stat.is_dir_) {
    return td::Status::Error("foreign candidate input is not a directory");
  }

  const std::string block_path = canonical + TD_DIR_SLASH + "block.boc";
  const std::string collated_path = canonical + TD_DIR_SLASH + "collated.boc";
  std::set<std::string> files;
  std::error_code error;
  std::filesystem::directory_iterator iterator{canonical, error};
  const std::filesystem::directory_iterator end;
  if (error) {
    return td::Status::Error(PSLICE() << "cannot enumerate foreign candidate directory: " << error.message());
  }
  while (iterator != end) {
    const auto entry_path = iterator->path();
    auto entry_status = iterator->symlink_status(error);
    if (error) {
      return td::Status::Error(PSLICE() << "cannot inspect foreign candidate directory entry: " << error.message());
    }
    const auto path = entry_path.string();
    if (!std::filesystem::is_regular_file(entry_status)) {
      return td::Status::Error("foreign candidate directory contains a symlink, directory, or special entry");
    }
    if (path != block_path && path != collated_path) {
      return td::Status::Error("foreign candidate directory contains an unexpected file");
    }
    if (!files.insert(path).second) {
      return td::Status::Error("foreign candidate directory contains a duplicate file occurrence");
    }
    iterator.increment(error);
    if (error) {
      return td::Status::Error(PSLICE() << "cannot enumerate foreign candidate directory: " << error.message());
    }
  }
  if (files.size() != 2 || !files.count(block_path) || !files.count(collated_path)) {
    return td::Status::Error("foreign candidate directory must contain exactly block.boc and collated.boc");
  }
  return canonical;
}

td::Status create_corpus_output_directory(td::CSlice directory_slice) {
  std::filesystem::path directory{directory_slice.str()};
  while (directory.has_relative_path() && directory.filename().empty()) {
    auto parent = directory.parent_path();
    if (parent == directory) {
      break;
    }
    directory = std::move(parent);
  }

  std::error_code error;
  const auto parent = directory.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) {
      return td::Status::Error(PSLICE() << "cannot create corpus output parent directory: " << error.message());
    }
  }

  if (!std::filesystem::create_directory(directory, error)) {
    if (!error || error == std::errc::file_exists) {
      return td::Status::Error(PSLICE() << "corpus output path already exists: " << directory_slice);
    }
    return td::Status::Error(PSLICE() << "cannot create corpus output directory: " << error.message());
  }
  return td::Status::OK();
}

}  // namespace

td::Slice collation_corpus_workload_name(Workload workload) {
  return workload == Workload::Transfer ? kTransferWorkloadName : kJettonWorkloadName;
}

td::Result<std::string> write_collation_corpus(td::CSlice directory_slice, const FixtureConfig& config,
                                               const Fixture& fixture, const ton::BlockCandidate& full_candidate,
                                               const ton::BlockCandidate& preloaded_candidate, td::uint64 transactions,
                                               td::uint64 gas_used) {
  const auto directory = directory_slice.str();
  if (directory.empty() || config.accounts == 0 || config.accounts > std::numeric_limits<td::uint32>::max() ||
      config.transfers == 0 || config.transfers > 9999 || fixture.messages.size() != config.transfers ||
      transactions > std::numeric_limits<td::uint32>::max()) {
    return td::Status::Error("corpus writer received unsupported workload cardinality");
  }
  if (fixture.shard.workchain != 0 || fixture.shard.shard != ton::shardIdAll ||
      fixture.prev_id.shard_full() != fixture.shard || fixture.prev_id.seqno() != 0 || fixture.mc_state.is_null() ||
      fixture.mc_block_data.is_null() || fixture.state_root.is_null() || fixture.prev_state.is_null() ||
      fixture.validator_set.is_null() || full_candidate.id != preloaded_candidate.id ||
      full_candidate.pubkey != preloaded_candidate.pubkey || full_candidate.pubkey != fixture.creator ||
      full_candidate.data.as_slice() != preloaded_candidate.data.as_slice() ||
      full_candidate.collated_file_hash == preloaded_candidate.collated_file_hash) {
    return td::Status::Error("fixture/candidates do not satisfy the portable v1 corpus contract");
  }
  if (full_candidate.id.shard_full() != fixture.shard || full_candidate.id.seqno() != fixture.prev_id.seqno() + 1) {
    return td::Status::Error("candidate is not the next full-wc0 block");
  }
  const auto transaction_multiplier = transactions_per_transfer(config.workload);
  if (config.transfers > std::numeric_limits<td::uint32>::max() / transaction_multiplier ||
      transactions != config.transfers * transaction_multiplier) {
    return td::Status::Error("expected transaction count must be exactly the workload multiple of the transfer count");
  }
  TRY_STATUS(verify_empty_previous_state(fixture.prev_id, fixture.state_root));
  TRY_RESULT(facts, inspect_candidate(full_candidate, fixture.state_root, fixture.prev_id, fixture.mc_id, true));
  TRY_RESULT(preloaded_facts,
             inspect_candidate(preloaded_candidate, fixture.state_root, fixture.prev_id, fixture.mc_id, false));
  if (td::Bits256{facts.extra.rand_seed.bits()}.is_zero() ||
      facts.successor_state_root != preloaded_facts.successor_state_root ||
      facts.info.gen_utime != preloaded_facts.info.gen_utime || facts.gen_utime_ms != preloaded_facts.gen_utime_ms ||
      facts.extra.rand_seed != preloaded_facts.extra.rand_seed ||
      facts.extra.created_by != preloaded_facts.extra.created_by ||
      td::Bits256{facts.extra.created_by.bits()} != fixture.creator.as_bits256()) {
    return td::Status::Error("full and preloaded candidates disagree on golden block facts");
  }

  TRY_STATUS(create_corpus_output_directory(directory_slice));
  TRY_STATUS(td::mkdir(PSTRING() << directory << "/states"));
  TRY_STATUS(td::mkdir(PSTRING() << directory << "/blocks"));
  TRY_STATUS(td::mkdir(PSTRING() << directory << "/externals"));
  TRY_STATUS(td::mkdir(PSTRING() << directory << "/candidate"));

  std::map<std::string, ArtifactInfo> artifacts;
  TRY_RESULT(shard_state, vm::std_boc_serialize(fixture.state_root, 31));
  if (block::compute_file_hash(shard_state) != fixture.prev_id.file_hash) {
    return td::Status::Error("serialized previous state does not reproduce previous block file hash");
  }
  TRY_STATUS(write_single_root_artifact(directory, "shard_prev_state", "states/shard-prev.boc", shard_state.as_slice(),
                                        fixture.state_root, artifacts));
  shard_state.clear();

  TRY_RESULT(masterchain_state, vm::std_boc_serialize(fixture.mc_state->root_cell(), 31));
  TRY_STATUS(write_single_root_artifact(directory, "masterchain_state", "states/masterchain.boc",
                                        masterchain_state.as_slice(), fixture.mc_state->root_cell(), artifacts));
  masterchain_state.clear();

  auto masterchain_block = fixture.mc_block_data->data();
  if (fixture.mc_block_data->block_id() != fixture.mc_id ||
      block::compute_file_hash(masterchain_block) != fixture.mc_id.file_hash) {
    return td::Status::Error("masterchain block data does not reproduce the masterchain block id");
  }
  TRY_STATUS(verify_canonical_full_state(fixture.mc_id, fixture.mc_state->root_cell(), "masterchain state"));
  TRY_STATUS(
      verify_block_state_target(fixture.mc_id, fixture.mc_block_data->root_cell(), fixture.mc_state->root_cell()));
  TRY_STATUS(verify_portable_masterchain_context(fixture.mc_id, fixture.mc_block_data->root_cell(),
                                                 fixture.mc_state->root_cell()));
  TRY_STATUS(write_single_root_artifact(directory, "masterchain_block", "blocks/masterchain.boc",
                                        masterchain_block.as_slice(), fixture.mc_block_data->root_cell(), artifacts));

  for (td::uint32 i = 0; i < fixture.messages.size(); ++i) {
    auto data = fixture.messages[i]->serialize();
    TRY_STATUS(write_single_root_artifact(directory, external_key(i), external_path(i), data.as_slice(),
                                          fixture.messages[i]->root_cell(), artifacts));
  }
  TRY_STATUS(write_single_root_artifact(directory, "candidate_block", "candidate/block.boc",
                                        full_candidate.data.as_slice(), facts.root, artifacts));
  TRY_STATUS(write_inspected_artifact(directory, "candidate_collated_full", "candidate/collated-full.boc",
                                      full_candidate.collated_data.as_slice(), artifacts));
  TRY_STATUS(write_inspected_artifact(directory, "candidate_collated_preloaded", "candidate/collated-preloaded.boc",
                                      preloaded_candidate.collated_data.as_slice(), artifacts));

  auto meta =
      render_meta(config, fixture, full_candidate, preloaded_candidate, facts, artifacts, transactions, gas_used);
  auto corpus_id = hex256(td::sha256_bits256(meta));
  auto checksum = PSTRING() << corpus_id << "  meta.json\n";
  TRY_STATUS(td::atomic_write_file(PSTRING() << directory << "/meta.json", meta));
  TRY_STATUS(td::atomic_write_file(PSTRING() << directory << "/manifest.sha256", checksum));
  return corpus_id;
}

td::Result<LoadedCorpus> load_collation_corpus(td::CSlice directory_slice) {
  const auto directory = directory_slice.str();
  if (directory.empty()) {
    return td::Status::Error("corpus directory must not be empty");
  }
  TRY_RESULT(meta_data, td::read_file(PSTRING() << directory << "/meta.json"));
  const auto corpus_hash = td::sha256_bits256(meta_data.as_slice());
  const auto corpus_id = hex256(corpus_hash);
  TRY_RESULT(checksum_data, td::read_file(PSTRING() << directory << "/manifest.sha256"));
  const std::string expected_checksum = PSTRING() << corpus_id << "  meta.json\n";
  if (checksum_data.as_slice() != expected_checksum) {
    return td::Status::Error("manifest.sha256 does not authenticate the exact meta.json bytes");
  }

  std::string json_copy = meta_data.as_slice().str();
  TRY_RESULT(json, td::json_decode(json_copy));
  if (json.type() != td::JsonValue::Type::Object) {
    return td::Status::Error("meta.json must contain one JSON object");
  }
  const auto& root = json.get_object();
  TRY_STATUS(require_fields(root,
                            {"format", "version", "workload", "contract", "header", "previous", "masterchain",
                             "candidate", "externals", "files", "expected"},
                            "meta.json"));
  TRY_RESULT(format, root.get_required_string_field("format"));
  TRY_RESULT(version, required_u32(root, "version"));
  if (format != kCollationCorpusFormat || version != kCollationCorpusVersion) {
    return td::Status::Error("unsupported collation corpus format/version");
  }

  LoadedCorpus loaded;
  TRY_RESULT(workload, required_object(root, "workload"));
  TRY_STATUS(require_fields(*workload, {"name", "version", "accounts", "transfers", "seed"}, "workload"));
  TRY_RESULT(workload_name, workload->get_required_string_field("name"));
  TRY_RESULT(workload_version, required_u32(*workload, "version"));
  TRY_RESULT(accounts, required_u32(*workload, "accounts"));
  TRY_RESULT(transfers, required_u32(*workload, "transfers"));
  TRY_RESULT(seed, required_decimal(*workload, "seed"));
  TRY_RESULT(workload_kind, parse_workload_name(workload_name));
  if (workload_version != kWorkloadVersion || accounts == 0 || transfers == 0 || transfers > 9999) {
    return td::Status::Error("unsupported workload description");
  }
  loaded.config.accounts = accounts;
  loaded.config.transfers = transfers;
  loaded.config.seed = seed;
  loaded.config.workload = workload_kind;

  TRY_RESULT(contract, required_object(root, "contract"));
  TRY_STATUS(require_fields(
      *contract,
      {"workchain", "shard", "previous_count", "before_split", "after_split", "after_merge", "internals_count",
       "internals_complete", "neighbors_count", "top_blocks_count", "out_queue_size", "full_collated_data",
       "max_external_attempts", "storage_stat_cache", "queue_cleanup_deadline", "internal_msg_deadline"},
      "contract"));
  TRY_RESULT(contract_workchain, contract->get_required_long_field("workchain"));
  TRY_RESULT(contract_shard_text, contract->get_required_string_field("shard"));
  TRY_RESULT(contract_shard, parse_shard_hex(contract_shard_text, "contract.shard"));
  TRY_RESULT(previous_count, required_u32(*contract, "previous_count"));
  TRY_RESULT(before_split, contract->get_required_bool_field("before_split"));
  TRY_RESULT(after_split, contract->get_required_bool_field("after_split"));
  TRY_RESULT(after_merge, contract->get_required_bool_field("after_merge"));
  TRY_RESULT(internals_count, required_u32(*contract, "internals_count"));
  TRY_RESULT(internals_complete, contract->get_required_bool_field("internals_complete"));
  TRY_RESULT(neighbors_count, required_u32(*contract, "neighbors_count"));
  TRY_RESULT(top_blocks_count, required_u32(*contract, "top_blocks_count"));
  TRY_RESULT(out_queue_size, required_decimal(*contract, "out_queue_size"));
  TRY_RESULT(full_collated_data, contract->get_required_bool_field("full_collated_data"));
  TRY_RESULT(max_external_attempts, required_u32(*contract, "max_external_attempts"));
  TRY_RESULT(storage_stat_cache, contract->get_required_bool_field("storage_stat_cache"));
  TRY_RESULT(queue_cleanup_deadline, contract->get_required_bool_field("queue_cleanup_deadline"));
  TRY_RESULT(internal_msg_deadline, contract->get_required_bool_field("internal_msg_deadline"));
  if (contract_workchain != 0 || contract_shard != ton::shardIdAll || previous_count != 1 || before_split ||
      after_split || after_merge || internals_count != 0 || !internals_complete || neighbors_count != 0 ||
      top_blocks_count != 0 || out_queue_size != 0 || !full_collated_data || max_external_attempts != transfers ||
      storage_stat_cache || queue_cleanup_deadline || internal_msg_deadline) {
    return td::Status::Error("corpus contract is not the supported external-only full-wc0 profile");
  }
  loaded.fixture.shard = ton::ShardIdFull{0, contract_shard};

  TRY_RESULT(previous_object, required_object(root, "previous"));
  TRY_RESULT_ASSIGN(loaded.fixture.prev_id, parse_block(*previous_object, "previous"));
  TRY_RESULT(masterchain_object, required_object(root, "masterchain"));
  TRY_RESULT_ASSIGN(loaded.fixture.mc_id, parse_block(*masterchain_object, "masterchain"));
  if (loaded.fixture.prev_id.shard_full() != loaded.fixture.shard || loaded.fixture.prev_id.seqno() != 0 ||
      !loaded.fixture.mc_id.is_masterchain() || loaded.fixture.mc_id.seqno() == 0) {
    return td::Status::Error("previous/masterchain ids violate the v1 corpus profile");
  }

  TRY_RESULT(header, required_object(root, "header"));
  TRY_STATUS(require_fields(*header, {"gen_utime", "gen_utime_ms", "rand_seed", "created_by"}, "header"));
  TRY_RESULT(header_gen_utime, required_u32(*header, "gen_utime"));
  TRY_RESULT(header_gen_utime_ms, required_decimal(*header, "gen_utime_ms"));
  TRY_RESULT(header_rand_seed, required_hex256(*header, "rand_seed"));
  TRY_RESULT(header_created_by, required_hex256(*header, "created_by"));
  if (header_gen_utime_ms / 1000 != header_gen_utime || header_gen_utime == 0 || header_rand_seed.is_zero() ||
      header_created_by.is_zero()) {
    return td::Status::Error("header time, random seed, or creator is invalid");
  }

  TRY_RESULT(candidate_object, required_object(root, "candidate"));
  TRY_STATUS(require_fields(*candidate_object,
                            {"block", "collated_file_hash", "preloaded_collated_file_hash", "state_root_hash"},
                            "candidate"));
  TRY_RESULT(candidate_block_object, required_object(*candidate_object, "block"));
  TRY_RESULT(candidate_id, parse_block(*candidate_block_object, "candidate.block"));
  TRY_RESULT(full_collated_hash, required_hex256(*candidate_object, "collated_file_hash"));
  TRY_RESULT(preloaded_collated_hash, required_hex256(*candidate_object, "preloaded_collated_file_hash"));
  TRY_RESULT(expected_state_root, required_hex256(*candidate_object, "state_root_hash"));

  TRY_RESULT(externals, required_object(root, "externals"));
  TRY_STATUS(require_fields(*externals, {"count", "pattern"}, "externals"));
  TRY_RESULT(external_count, required_u32(*externals, "count"));
  TRY_RESULT(pattern, externals->get_required_string_field("pattern"));
  if (external_count != transfers || pattern != kExternalPattern) {
    return td::Status::Error("external-message count/pattern does not match the workload");
  }

  TRY_RESULT(files_object, required_object(root, "files"));
  std::map<std::string, ArtifactInfo> artifacts;
  for (const auto& field : files_object->field_values_) {
    if (field.second.type() != td::JsonValue::Type::Object) {
      return td::Status::Error(PSLICE() << "files." << field.first << " must be an object");
    }
    TRY_RESULT(info, parse_artifact_info(field.second.get_object(), field.first));
    if (!artifacts.emplace(field.first.str(), std::move(info)).second) {
      return td::Status::Error(PSLICE() << "duplicate artifact key " << field.first);
    }
  }
  std::set<std::string> required_keys{"shard_prev_state", "masterchain_state",       "masterchain_block",
                                      "candidate_block",  "candidate_collated_full", "candidate_collated_preloaded"};
  for (td::uint32 i = 0; i < external_count; ++i) {
    required_keys.insert(external_key(i));
  }
  if (artifacts.count("shard_prev_block") != 0) {
    return td::Status::Error("v1 fixes the shard predecessor at seqno 0 and forbids shard_prev_block");
  }
  if (artifacts.size() != required_keys.size() ||
      std::any_of(required_keys.begin(), required_keys.end(), [&](const auto& key) { return !artifacts.count(key); })) {
    return td::Status::Error("files contains an unknown or missing artifact key");
  }

  TRY_RESULT(expected, required_object(root, "expected"));
  TRY_STATUS(require_fields(
      *expected, {"transactions", "gas_used", "block_bytes", "collated_full_bytes", "collated_preloaded_bytes"},
      "expected"));
  TRY_RESULT_ASSIGN(loaded.expected_transactions, required_u32(*expected, "transactions"));
  TRY_RESULT_ASSIGN(loaded.expected_gas_used, required_decimal(*expected, "gas_used"));
  TRY_RESULT(expected_block_bytes, required_decimal(*expected, "block_bytes"));
  TRY_RESULT(expected_full_bytes, required_decimal(*expected, "collated_full_bytes"));
  TRY_RESULT(expected_preloaded_bytes, required_decimal(*expected, "collated_preloaded_bytes"));
  const auto transaction_multiplier = transactions_per_transfer(loaded.config.workload);
  if (transfers > std::numeric_limits<td::uint32>::max() / transaction_multiplier ||
      loaded.expected_transactions != transfers * transaction_multiplier) {
    return td::Status::Error("expected transaction count must be exactly the workload multiple of the transfer count");
  }

  TRY_RESULT(shard_state_artifact, load_artifact(directory, "shard_prev_state", "states/shard-prev.boc", artifacts));
  if (shard_state_artifact.roots.size() != 1 ||
      td::Bits256{shard_state_artifact.roots[0]->get_hash().bits()} != loaded.fixture.prev_id.root_hash ||
      block::compute_file_hash(shard_state_artifact.data) != loaded.fixture.prev_id.file_hash) {
    return td::Status::Error("previous state artifact does not reproduce the previous block id");
  }
  loaded.fixture.state_root = shard_state_artifact.roots[0];
  TRY_STATUS(verify_empty_previous_state(loaded.fixture.prev_id, loaded.fixture.state_root));
  TRY_RESULT_ASSIGN(loaded.fixture.prev_state,
                    ton::validator::create_shard_state(loaded.fixture.prev_id, loaded.fixture.state_root));

  TRY_RESULT(masterchain_state_artifact,
             load_artifact(directory, "masterchain_state", "states/masterchain.boc", artifacts));
  if (masterchain_state_artifact.roots.size() != 1) {
    return td::Status::Error("masterchain state artifact must have one root occurrence");
  }
  TRY_STATUS(
      verify_canonical_full_state(loaded.fixture.mc_id, masterchain_state_artifact.roots[0], "masterchain state"));
  TRY_RESULT(masterchain_shard_state,
             ton::validator::create_shard_state(loaded.fixture.mc_id, masterchain_state_artifact.roots[0]));
  loaded.fixture.mc_state = td::Ref<ton::validator::MasterchainState>{std::move(masterchain_shard_state)};
  if (loaded.fixture.mc_state.is_null()) {
    return td::Status::Error("masterchain state artifact has the wrong dynamic type");
  }
  auto registered_shard = loaded.fixture.mc_state->get_shard_from_config(loaded.fixture.shard);
  if (registered_shard.is_null() || registered_shard->top_block_id() != loaded.fixture.prev_id) {
    return td::Status::Error("masterchain state does not register the corpus predecessor");
  }
  loaded.fixture.validator_set = loaded.fixture.mc_state->get_validator_set(loaded.fixture.shard);
  loaded.fixture.mc_validator_set = loaded.fixture.mc_state->get_validator_set(loaded.fixture.mc_id.shard_full());
  if (loaded.fixture.validator_set.is_null() || loaded.fixture.validator_set->export_vector().empty()) {
    return td::Status::Error("masterchain state yields no wc0 validator set");
  }

  TRY_RESULT(masterchain_block_artifact,
             load_artifact(directory, "masterchain_block", "blocks/masterchain.boc", artifacts));
  if (masterchain_block_artifact.roots.size() != 1 ||
      td::Bits256{masterchain_block_artifact.roots[0]->get_hash().bits()} != loaded.fixture.mc_id.root_hash ||
      block::compute_file_hash(masterchain_block_artifact.data) != loaded.fixture.mc_id.file_hash) {
    return td::Status::Error("masterchain block artifact does not reproduce its id");
  }
  TRY_STATUS(verify_block_state_target(loaded.fixture.mc_id, masterchain_block_artifact.roots[0],
                                       loaded.fixture.mc_state->root_cell()));
  TRY_STATUS(verify_portable_masterchain_context(loaded.fixture.mc_id, masterchain_block_artifact.roots[0],
                                                 loaded.fixture.mc_state->root_cell()));
  TRY_RESULT_ASSIGN(loaded.fixture.mc_block_data,
                    ton::validator::create_block(loaded.fixture.mc_id, masterchain_block_artifact.data.clone()));

  loaded.fixture.messages.reserve(external_count);
  const auto external_limits = loaded.fixture.mc_state->get_ext_msg_limits();
  for (td::uint32 i = 0; i < external_count; ++i) {
    const auto key = external_key(i);
    const auto path = external_path(i);
    TRY_RESULT(message_artifact, load_artifact(directory, key, path, artifacts));
    if (message_artifact.roots.size() != 1) {
      return td::Status::Error(PSLICE() << key << " must have one root occurrence");
    }
    TRY_RESULT(message,
               ton::validator::ExtMessageQ::create_ext_message(message_artifact.data.clone(), external_limits));
    if (message->root_cell().is_null() ||
        td::Bits256{message->root_cell()->get_hash().bits()} !=
            td::Bits256{message_artifact.roots[0]->get_hash().bits()} ||
        message->wc() != loaded.fixture.shard.workchain) {
      return td::Status::Error(PSLICE() << key << " does not decode as the recorded wc0 external message");
    }
    loaded.fixture.messages.emplace_back(std::move(message));
  }

  TRY_RESULT(candidate_block_artifact, load_artifact(directory, "candidate_block", "candidate/block.boc", artifacts));
  TRY_RESULT(full_collated_artifact,
             load_artifact(directory, "candidate_collated_full", "candidate/collated-full.boc", artifacts));
  TRY_RESULT(preloaded_collated_artifact,
             load_artifact(directory, "candidate_collated_preloaded", "candidate/collated-preloaded.boc", artifacts));
  if (candidate_block_artifact.roots.size() != 1 ||
      td::Bits256{candidate_block_artifact.roots[0]->get_hash().bits()} != candidate_id.root_hash ||
      block::compute_file_hash(candidate_block_artifact.data) != candidate_id.file_hash ||
      block::compute_file_hash(full_collated_artifact.data) != full_collated_hash ||
      block::compute_file_hash(preloaded_collated_artifact.data) != preloaded_collated_hash ||
      candidate_block_artifact.data.size() != expected_block_bytes ||
      full_collated_artifact.data.size() != expected_full_bytes ||
      preloaded_collated_artifact.data.size() != expected_preloaded_bytes ||
      full_collated_hash == preloaded_collated_hash) {
    return td::Status::Error("candidate artifacts disagree with candidate/expected metadata");
  }
  loaded.fixture.creator = ton::Ed25519_PublicKey{header_created_by};
  loaded.full_candidate =
      ton::BlockCandidate{loaded.fixture.creator, candidate_id, full_collated_hash,
                          candidate_block_artifact.data.clone(), full_collated_artifact.data.clone()};
  loaded.preloaded_candidate =
      ton::BlockCandidate{loaded.fixture.creator, candidate_id, preloaded_collated_hash,
                          candidate_block_artifact.data.clone(), preloaded_collated_artifact.data.clone()};
  TRY_RESULT(full_facts, inspect_candidate(loaded.full_candidate, loaded.fixture.state_root, loaded.fixture.prev_id,
                                           loaded.fixture.mc_id, true));
  TRY_RESULT(preloaded_facts, inspect_candidate(loaded.preloaded_candidate, loaded.fixture.state_root,
                                                loaded.fixture.prev_id, loaded.fixture.mc_id, false));
  if (full_facts.successor_state_root != expected_state_root ||
      preloaded_facts.successor_state_root != expected_state_root || full_facts.info.gen_utime != header_gen_utime ||
      preloaded_facts.info.gen_utime != header_gen_utime || full_facts.gen_utime_ms != header_gen_utime_ms ||
      preloaded_facts.gen_utime_ms != header_gen_utime_ms ||
      td::Bits256{full_facts.extra.rand_seed.bits()} != header_rand_seed ||
      td::Bits256{preloaded_facts.extra.rand_seed.bits()} != header_rand_seed ||
      td::Bits256{full_facts.extra.created_by.bits()} != header_created_by ||
      td::Bits256{preloaded_facts.extra.created_by.bits()} != header_created_by) {
    return td::Status::Error("candidate facts disagree with header/golden successor metadata");
  }
  const auto validators = loaded.fixture.validator_set->export_vector();
  if (validators.front().key != loaded.fixture.creator) {
    return td::Status::Error("header creator is not the deterministic first wc0 validator");
  }

  const auto setup_utime =
      std::max(loaded.fixture.prev_state->get_unix_time(), loaded.fixture.mc_state->get_unix_time());
  if (setup_utime == std::numeric_limits<td::uint32>::max() || header_gen_utime != setup_utime + 1) {
    return td::Status::Error("candidate time is not the next deterministic fixture time");
  }
  loaded.fixture.gen_utime = setup_utime;
  loaded.fixture.corpus_id = corpus_id;
  loaded.fixture.expected_successor_state_root = expected_state_root;
  loaded.fixture.expected_rand_seed = header_rand_seed;
  loaded.fixture.target_gen_utime_ms = header_gen_utime_ms;
  loaded.fixture.injected_accounts = accounts;
  loaded.fixture.preserved_accounts = 0;
  return loaded;
}

td::Result<ton::BlockCandidate> load_foreign_collation_candidate(td::CSlice directory_slice, const Fixture& fixture,
                                                                 CandidateSidecarKind sidecar_kind,
                                                                 td::uint64 expected_transactions,
                                                                 td::uint64 expected_gas_used) {
  TRY_RESULT(directory, require_foreign_candidate_directory(directory_slice));
  if (fixture.state_root.is_null() || fixture.prev_state.is_null() || fixture.mc_state.is_null() ||
      fixture.validator_set.is_null() || fixture.corpus_id.empty() || !fixture.expected_successor_state_root ||
      !fixture.expected_rand_seed || !fixture.target_gen_utime_ms || fixture.creator.as_bits256().is_zero()) {
    return td::Status::Error("foreign candidate requires a complete loaded corpus fixture");
  }

  TRY_RESULT(block_data, td::read_file(PSTRING() << directory << "/block.boc"));
  TRY_RESULT(collated_data, td::read_file(PSTRING() << directory << "/collated.boc"));
  TRY_RESULT(block_root, vm::std_boc_deserialize(block_data.as_slice()));
  block::gen::Block::Record block_record;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  ton::ShardIdFull shard;
  if (!tlb::unpack_cell(block_root, block_record) || !tlb::unpack_cell(block_record.info, info) || info.version != 0 ||
      !tlb::unpack_cell(block_record.extra, extra) || info.shard.is_null()) {
    return td::Status::Error("cannot derive a foreign candidate id from its block header");
  }
  auto semantic_shard = info.shard;
  if (!block::tlb::t_ShardIdent.unpack(semantic_shard.write(), shard)) {
    return td::Status::Error("cannot derive a foreign candidate id from its block header");
  }
  TRY_STATUS(verify_canonical_full_shard_ident(info.shard, shard.workchain, "foreign candidate header"));
  ton::BlockIdExt id{shard.workchain, shard.shard, info.seq_no, ton::RootHash{block_root->get_hash().bits()},
                     block::compute_file_hash(block_data)};
  if (!id.is_valid_full() || fixture.prev_id.seqno() == std::numeric_limits<ton::BlockSeqno>::max() ||
      id.shard_full() != fixture.shard || id.seqno() != fixture.prev_id.seqno() + 1) {
    return td::Status::Error("foreign candidate is not the next block in the corpus shard");
  }

  const ton::Ed25519_PublicKey creator{td::Bits256{extra.created_by.bits()}};
  ton::BlockCandidate candidate{creator, id, block::compute_file_hash(collated_data), std::move(block_data),
                                std::move(collated_data)};
  const bool require_full = sidecar_kind == CandidateSidecarKind::Full;
  TRY_RESULT(facts, inspect_candidate(candidate, fixture.state_root, fixture.prev_id, fixture.mc_id, require_full));

  block::gen::ShardStateUnsplit::Record previous_state;
  block::gen::ShardStateUnsplit::Record masterchain_state;
  if (!tlb::unpack_cell(fixture.state_root, previous_state) ||
      !tlb::unpack_cell(fixture.mc_state->root_cell(), masterchain_state) ||
      block_record.global_id != previous_state.global_id || block_record.global_id != masterchain_state.global_id ||
      facts.successor_state_root != fixture.expected_successor_state_root.value() ||
      facts.gen_utime_ms != fixture.target_gen_utime_ms.value() ||
      facts.info.gen_utime != fixture.target_gen_utime_ms.value() / 1000 ||
      td::Bits256{facts.extra.rand_seed.bits()} != fixture.expected_rand_seed.value() ||
      td::Bits256{facts.extra.created_by.bits()} != fixture.creator.as_bits256() ||
      candidate.pubkey != fixture.creator) {
    return td::Status::Error("foreign candidate facts differ from the loaded corpus contract");
  }
  TRY_RESULT(workload, inspect_candidate_workload(facts));
  if (workload.transactions != expected_transactions || workload.gas_used != expected_gas_used) {
    return td::Status::Error(PSLICE() << "foreign candidate workload statistics differ from the corpus: transactions="
                                      << workload.transactions << " gas=" << workload.gas_used
                                      << " expected_transactions=" << expected_transactions
                                      << " expected_gas=" << expected_gas_used);
  }
  return candidate;
}

}  // namespace bench::collation
