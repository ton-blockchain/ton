#include "emulator-extern.h"
#include "td/utils/base64.h"
#include "td/utils/Status.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/Variant.h"
#include "td/utils/overloaded.h"
#include "transaction-emulator.h"
#include "tvm-emulator.hpp"
#include "crypto/vm/stack.hpp"

td::Result<td::Ref<vm::Cell>> boc_b64_to_cell(const char *boc) {
  TRY_RESULT_PREFIX(boc_decoded, td::base64_decode(td::Slice(boc)), "Can't decode base64 boc: ");
  return vm::std_boc_deserialize(boc_decoded);
}

td::Result<std::string> cell_to_boc_b64(td::Ref<vm::Cell> cell) {
  TRY_RESULT_PREFIX(boc, vm::std_boc_serialize(std::move(cell), vm::BagOfCells::Mode::WithCRC32C), "Can't serialize cell: ");
  return td::base64_encode(boc.as_slice());
}

const char *success_response(std::string&& transaction, std::string&& new_shard_account, std::string&& vm_log, 
                             td::optional<std::string>&& actions, double elapsed_time) {
  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonTrue());
  json_obj("transaction", std::move(transaction));
  json_obj("shard_account", std::move(new_shard_account));
  json_obj("vm_log", std::move(vm_log));
  if (actions) {
    json_obj("actions", actions.unwrap());
  } else {
    json_obj("actions", td::JsonNull());
  }
  json_obj("elapsed_time", elapsed_time);
  json_obj.leave();
  return strdup(jb.string_builder().as_cslice().c_str());
}

const char *error_response(std::string&& error) {
  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonFalse());
  json_obj("error", std::move(error));
  json_obj.leave();
  return strdup(jb.string_builder().as_cslice().c_str());
}

const char *external_not_accepted_response(std::string&& vm_log, int vm_exit_code, double elapsed_time) {
  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonFalse());
  json_obj("error", "External message not accepted by smart contract");
  json_obj("vm_log", std::move(vm_log));
  json_obj("vm_exit_code", vm_exit_code);
  json_obj("elapsed_time", elapsed_time);
  json_obj.leave();
  return strdup(jb.string_builder().as_cslice().c_str());
}

#define ERROR_RESPONSE(error) return error_response(error)

td::Result<block::Config> decode_config(const char* config_boc) {
  TRY_RESULT_PREFIX(config_params_cell, boc_b64_to_cell(config_boc), "Can't deserialize config params boc: ");
  auto global_config = block::Config(config_params_cell, td::Bits256::zero(), block::Config::needWorkchainInfo | block::Config::needSpecialSmc | block::Config::needCapabilities);
  TRY_STATUS_PREFIX(global_config.unpack(), "Can't unpack config params: ");
  return global_config;
}

void *transaction_emulator_create(const char *config_params_boc, int vm_log_verbosity) {
  auto global_config_res = decode_config(config_params_boc);
  if (global_config_res.is_error()) {
    LOG(ERROR) << global_config_res.move_as_error().message();
    return nullptr;
  }

  return new emulator::TransactionEmulator(global_config_res.move_as_ok(), vm_log_verbosity);
}

const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *shard_account_boc, const char *message_boc) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);
  
  auto message_cell_r = boc_b64_to_cell(message_boc);
  if (message_cell_r.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't deserialize message boc: " << message_cell_r.move_as_error());
  }
  auto message_cell = message_cell_r.move_as_ok();
  auto message_cs = vm::load_cell_slice(message_cell);
  int msg_tag = block::gen::t_CommonMsgInfo.get_tag(message_cs);

  auto shard_account_cell = boc_b64_to_cell(shard_account_boc);
  if (shard_account_cell.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't deserialize shard account boc: " << shard_account_cell.move_as_error());
  }
  auto shard_account_slice = vm::load_cell_slice(shard_account_cell.ok_ref());
  block::gen::ShardAccount::Record shard_account;
  if (!tlb::unpack(shard_account_slice, shard_account)) {
    ERROR_RESPONSE(PSTRING() << "Can't unpack shard account cell");
  }

  td::Ref<vm::CellSlice> addr_slice;
  auto account_slice = vm::load_cell_slice(shard_account.account);
  if (block::gen::t_Account.get_tag(account_slice) == block::gen::Account::account_none) {
    if (msg_tag == block::gen::CommonMsgInfo::ext_in_msg_info) {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(message_cs, info)) {
        ERROR_RESPONSE(PSTRING() <<  "Can't unpack inbound external message");
      }
      addr_slice = std::move(info.dest);
    }
    else if (msg_tag == block::gen::CommonMsgInfo::int_msg_info) {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(message_cs, info)) {
          ERROR_RESPONSE(PSTRING() << "Can't unpack inbound internal message");
      }
      addr_slice = std::move(info.dest);
    } else {
      ERROR_RESPONSE(PSTRING() << "Only ext in and int message are supported");
    }
  } else {
    block::gen::Account::Record_account account_record;
    if (!tlb::unpack(account_slice, account_record)) {
      ERROR_RESPONSE(PSTRING() << "Can't unpack account cell");
    }
    addr_slice = std::move(account_record.addr);
  }
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(addr_slice, wc, addr)) {
    ERROR_RESPONSE(PSTRING() << "Can't extract account address");
  }

  auto account = block::Account(wc, addr.bits());
  ton::UnixTime now = (unsigned)std::time(nullptr);
  bool is_special = wc == ton::masterchainId && emulator->get_config().is_special_smartcontract(addr);
  if (!account.unpack(vm::load_cell_slice_ref(shard_account_cell.move_as_ok()), td::Ref<vm::CellSlice>(), now, is_special)) {
    ERROR_RESPONSE(PSTRING() << "Can't unpack shard account");
  }

  auto result = emulator->emulate_transaction(std::move(account), message_cell, 0, 0, block::transaction::Transaction::tr_ord);
  if (result.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Emulate transaction failed: " << result.move_as_error());
  }
  auto emulation_result = result.move_as_ok();

  auto external_not_accepted = dynamic_cast<emulator::TransactionEmulator::EmulationExternalNotAccepted *>(emulation_result.get());
  if (external_not_accepted) {
    return external_not_accepted_response(std::move(external_not_accepted->vm_log), external_not_accepted->vm_exit_code, 
                                          external_not_accepted->elapsed_time);
  }

  auto emulation_success = dynamic_cast<emulator::TransactionEmulator::EmulationSuccess&>(*emulation_result);
  auto trans_boc_b64 = cell_to_boc_b64(std::move(emulation_success.transaction));
  if (trans_boc_b64.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't serialize Transaction to boc " << trans_boc_b64.move_as_error());
  }

  auto new_shard_account_cell = vm::CellBuilder().store_ref(emulation_success.account.total_state)
                               .store_bits(emulation_success.account.last_trans_hash_.as_bitslice())
                               .store_long(emulation_success.account.last_trans_lt_).finalize();
  auto new_shard_account_boc_b64 = cell_to_boc_b64(std::move(new_shard_account_cell));
  if (new_shard_account_boc_b64.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't serialize ShardAccount to boc " << new_shard_account_boc_b64.move_as_error());
  }

  td::optional<td::string> actions_boc_b64;
  if (emulation_success.actions.not_null()) {
    auto actions_boc_b64_result = cell_to_boc_b64(std::move(emulation_success.actions));
    if (actions_boc_b64_result.is_error()) {
      ERROR_RESPONSE(PSTRING() << "Can't serialize actions list cell to boc " << actions_boc_b64_result.move_as_error());
    }
    actions_boc_b64 = actions_boc_b64_result.move_as_ok();
  }

  return success_response(trans_boc_b64.move_as_ok(), new_shard_account_boc_b64.move_as_ok(), std::move(emulation_success.vm_log), 
                          std::move(actions_boc_b64), emulation_success.elapsed_time);
}

bool transaction_emulator_set_unixtime(void *transaction_emulator, uint32_t unixtime) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  emulator->set_unixtime(unixtime);

  return true;
}

bool transaction_emulator_set_lt(void *transaction_emulator, uint64_t lt) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  emulator->set_lt(lt);

  return true;
}

bool transaction_emulator_set_rand_seed(void *transaction_emulator, const char* rand_seed_hex) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  auto rand_seed_hex_slice = td::Slice(rand_seed_hex);
  if (rand_seed_hex_slice.size() != 64) {
    LOG(ERROR) << "Rand seed expected as 64 characters hex string";
    return false;
  }
  auto rand_seed_bytes = td::hex_decode(rand_seed_hex_slice);
  if (rand_seed_bytes.is_error()) {
    LOG(ERROR) << "Can't decode hex rand seed";
    return false;
  }
  td::BitArray<256> rand_seed;
  rand_seed.as_slice().copy_from(rand_seed_bytes.move_as_ok());

  emulator->set_rand_seed(rand_seed);
  return true;
}

bool transaction_emulator_set_ignore_chksig(void *transaction_emulator, bool ignore_chksig) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  emulator->set_ignore_chksig(ignore_chksig);

  return true;
}

bool transaction_emulator_set_config(void *transaction_emulator, const char* config_boc) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  auto global_config_res = decode_config(config_boc);
  if (global_config_res.is_error()) {
    LOG(ERROR) << global_config_res.move_as_error().message();
    return false;
  }

  emulator->set_config(global_config_res.move_as_ok());

  return true;
}

bool transaction_emulator_set_libs(void *transaction_emulator, const char* shardchain_libs_boc) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  if (shardchain_libs_boc != nullptr) {
    auto shardchain_libs_cell = boc_b64_to_cell(shardchain_libs_boc);
    if (shardchain_libs_cell.is_error()) {
      LOG(ERROR) << "Can't deserialize shardchain libraries boc: " << shardchain_libs_cell.move_as_error();
      return false;
    }
    emulator->set_libs(vm::Dictionary(shardchain_libs_cell.move_as_ok(), 256));
  }

  return true;
}

bool transaction_emulator_set_debug_enabled(void *transaction_emulator, bool debug_enabled) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);

  emulator->set_debug_enabled(debug_enabled);

  return true;
}

void transaction_emulator_destroy(void *transaction_emulator) {
  delete static_cast<emulator::TransactionEmulator *>(transaction_emulator);
}

bool emulator_set_verbosity_level(int verbosity_level) {
  if (0 <= verbosity_level && verbosity_level <= VERBOSITY_NAME(NEVER)) {
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity_level);
    return true;
  }
  return false;
}

void *tvm_emulator_create(const char *code, const char *data, int vm_log_verbosity) {
  auto code_cell = boc_b64_to_cell(code);
  if (code_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize code boc: " << code_cell.move_as_error();
    return nullptr;
  }
  auto data_cell = boc_b64_to_cell(data);
  if (data_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize code boc: " << data_cell.move_as_error();
    return nullptr;
  }

  auto emulator = new emulator::TvmEmulator(code_cell.move_as_ok(), data_cell.move_as_ok());
  emulator->set_vm_verbosity_level(vm_log_verbosity);
  return emulator;
}

bool tvm_emulator_set_libraries(void *tvm_emulator, const char *libs_boc) {
  vm::Dictionary libs{256};
  auto libs_cell = boc_b64_to_cell(libs_boc);
  if (libs_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize libraries boc: " << libs_cell.move_as_error();
    return false;
  }
  libs = vm::Dictionary(libs_cell.move_as_ok(), 256);

  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  emulator->set_libraries(std::move(libs));

  return true;
}

bool tvm_emulator_set_c7(void *tvm_emulator, const char *address, uint32_t unixtime, uint64_t balance, const char *rand_seed_hex, const char *config_boc) {
  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  auto std_address = block::StdAddress::parse(td::Slice(address));
  if (std_address.is_error()) {
    LOG(ERROR) << "Can't parse address: " << std_address.move_as_error();
    return false;
  }
  
  auto config_params_cell = boc_b64_to_cell(config_boc);
  if (config_params_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize config params boc: " << config_params_cell.move_as_error();
    return false;
  }
  auto global_config = std::make_shared<block::Config>(config_params_cell.move_as_ok(), td::Bits256::zero(), block::Config::needWorkchainInfo | block::Config::needSpecialSmc);
  auto unpack_res = global_config->unpack();
  if (unpack_res.is_error()) {
    LOG(ERROR) << "Can't unpack config params";
    return false;
  }

  auto rand_seed_hex_slice = td::Slice(rand_seed_hex);
  if (rand_seed_hex_slice.size() != 64) {
    LOG(ERROR) << "Rand seed expected as 64 characters hex string";
    return false;
  }
  auto rand_seed_bytes = td::hex_decode(rand_seed_hex_slice);
  if (rand_seed_bytes.is_error()) {
    LOG(ERROR) << "Can't decode hex rand seed";
    return false;
  }
  td::BitArray<256> rand_seed;
  rand_seed.as_slice().copy_from(rand_seed_bytes.move_as_ok());

  emulator->set_c7(std_address.move_as_ok(), unixtime, balance, rand_seed, std::const_pointer_cast<const block::Config>(global_config));
  
  return true;
}

bool tvm_emulator_set_gas_limit(void *tvm_emulator, int64_t gas_limit) {
  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  emulator->set_gas_limit(gas_limit);
  return true;
}

bool tvm_emulator_set_debug_enabled(void *tvm_emulator, bool debug_enabled) {
  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  emulator->set_debug_enabled(debug_enabled);
  return true;
}

const char *tvm_emulator_run_get_method(void *tvm_emulator, int method_id, const char *stack_boc) {
  auto stack_cell = boc_b64_to_cell(stack_boc);
  if (stack_cell.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Couldn't deserialize stack cell: " << stack_cell.move_as_error().to_string());
  }
  auto stack_cs = vm::load_cell_slice(stack_cell.move_as_ok());
  td::Ref<vm::Stack> stack;
  if (!vm::Stack::deserialize_to(stack_cs, stack)) {
     ERROR_RESPONSE(PSTRING() << "Couldn't deserialize stack");
  }

  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  auto result = emulator->run_get_method(method_id, stack);
  
  vm::CellBuilder stack_cb;
  if (!result.stack->serialize(stack_cb)) {
    ERROR_RESPONSE(PSTRING() << "Couldn't serialize stack");
  }
  auto result_stack_boc = cell_to_boc_b64(stack_cb.finalize());
  if (result_stack_boc.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Couldn't serialize stack cell: " << result_stack_boc.move_as_error().to_string());
  }

  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonTrue());
  json_obj("stack", result_stack_boc.move_as_ok());
  json_obj("gas_used", std::to_string(result.gas_used));
  json_obj("vm_exit_code", result.code);
  json_obj("vm_log", result.vm_log);
  if (result.missing_library.is_null()) {
    json_obj("missing_library", td::JsonNull());
  } else {
    json_obj("missing_library", td::Bits256(result.missing_library).to_hex());
  }
  json_obj.leave();

  return strdup(jb.string_builder().as_cslice().c_str());
}

const char *tvm_emulator_send_external_message(void *tvm_emulator, const char *message_body_boc) {
  auto message_body_cell = boc_b64_to_cell(message_body_boc);
  if (message_body_cell.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't deserialize message body boc: " << message_body_cell.move_as_error());
  }

  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  auto result = emulator->send_external_message(message_body_cell.move_as_ok());

  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonTrue());
  json_obj("gas_used", std::to_string(result.gas_used));
  json_obj("vm_exit_code", result.code);
  json_obj("accepted", td::JsonBool(result.accepted));
  json_obj("vm_log", result.vm_log);
  if (result.missing_library.is_null()) {
    json_obj("missing_library", td::JsonNull());
  } else {
    json_obj("missing_library", td::Bits256(result.missing_library).to_hex());
  }
  if (result.actions.is_null()) {
    json_obj("actions", td::JsonNull());
  } else {
    json_obj("actions", cell_to_boc_b64(result.actions).move_as_ok());
  }
  json_obj("new_code", cell_to_boc_b64(result.new_state.code).move_as_ok());
  json_obj("new_data", cell_to_boc_b64(result.new_state.data).move_as_ok());
  json_obj.leave();

  return strdup(jb.string_builder().as_cslice().c_str());
}

const char *tvm_emulator_send_internal_message(void *tvm_emulator, const char *message_body_boc, uint64_t amount) {
  auto message_body_cell = boc_b64_to_cell(message_body_boc);
  if (message_body_cell.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't deserialize message body boc: " << message_body_cell.move_as_error());
  }

  auto emulator = static_cast<emulator::TvmEmulator *>(tvm_emulator);
  auto result = emulator->send_internal_message(message_body_cell.move_as_ok(), amount);

  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonTrue());
  json_obj("gas_used", std::to_string(result.gas_used));
  json_obj("vm_exit_code", result.code);
  json_obj("accepted", td::JsonBool(result.accepted));
  json_obj("vm_log", result.vm_log);
  if (result.missing_library.is_null()) {
    json_obj("missing_library", td::JsonNull());
  } else {
    json_obj("missing_library", td::Bits256(result.missing_library).to_hex());
  }
  if (result.actions.is_null()) {
    json_obj("actions", td::JsonNull());
  } else {
    json_obj("actions", cell_to_boc_b64(result.actions).move_as_ok());
  }
  json_obj("new_code", cell_to_boc_b64(result.new_state.code).move_as_ok());
  json_obj("new_data", cell_to_boc_b64(result.new_state.data).move_as_ok());
  json_obj.leave();

  return strdup(jb.string_builder().as_cslice().c_str());
}

void tvm_emulator_destroy(void *tvm_emulator) {
  delete static_cast<emulator::TvmEmulator *>(tvm_emulator);
}
