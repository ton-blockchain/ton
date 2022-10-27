#include "emulator-extern.h"
#include "td/utils/base64.h"
#include "td/utils/Status.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "transaction-emulator.h"

void *transaction_emulator_create(const char *config_params_boc, const char *shardchain_libs_boc, int vm_log_verbosity) {
  auto config_params_decoded = td::base64_decode(td::Slice(config_params_boc));
  if (config_params_decoded.is_error()) {
    LOG(ERROR) << "Can't decode base64 config params boc: " << config_params_decoded.move_as_error();
    return nullptr;
  }
  auto config_params_cell = vm::std_boc_deserialize(config_params_decoded.move_as_ok());
  if (config_params_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize config params boc: " << config_params_cell.move_as_error();
    return nullptr;
  }
  auto global_config = block::Config(config_params_cell.move_as_ok(), td::Bits256::zero(), block::Config::needWorkchainInfo | block::Config::needSpecialSmc);
  auto unpack_res = global_config.unpack();
  if (unpack_res.is_error()) {
    LOG(ERROR) << "Can't unpack config params";
    return nullptr;
  }

  vm::Dictionary shardchain_libs{256};
  if (shardchain_libs_boc != nullptr) {
    auto shardchain_libs_decoded = td::base64_decode(td::Slice(shardchain_libs_boc));
    if (shardchain_libs_decoded.is_error()) {
      LOG(ERROR) << "Can't decode base64 shardchain libraries boc: " << shardchain_libs_decoded.move_as_error();
      return nullptr;
    }
    auto shardchain_libs_cell = vm::std_boc_deserialize(shardchain_libs_decoded.move_as_ok());
    if (shardchain_libs_cell.is_error()) {
      LOG(ERROR) << "Can't deserialize shardchain libraries boc: " << shardchain_libs_cell.move_as_error();
      return nullptr;
    }
    shardchain_libs = vm::Dictionary(shardchain_libs_cell.move_as_ok(), 256);
  }

  return new emulator::TransactionEmulator(std::move(global_config), std::move(shardchain_libs), vm_log_verbosity);
}

const char *success_response(std::string&& transaction, std::string&& new_shard_account, std::string&& vm_log) {
  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonTrue());
  json_obj("transaction", std::move(transaction));
  json_obj("shard_account", std::move(new_shard_account));
  json_obj("vm_log", std::move(vm_log));
  json_obj.leave();
  auto json_response = jb.string_builder().as_cslice().str();
  auto heap_response = new std::string(json_response);
  return heap_response->c_str();
}

const char *error_response(std::string&& error) {
  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonFalse());
  json_obj("error", std::move(error));
  json_obj.leave();
  auto json_response = jb.string_builder().as_cslice().str();
  auto heap_response = new std::string(json_response);
  return heap_response->c_str();
}

const char *external_not_accepted_response(std::string&& vm_log, int vm_exit_code) {
  td::JsonBuilder jb;
  auto json_obj = jb.enter_object();
  json_obj("success", td::JsonFalse());
  json_obj("error", "External message not accepted by smart contract");
  json_obj("vm_log", std::move(vm_log));
  json_obj("vm_exit_code", vm_exit_code);
  json_obj.leave();
  auto json_response = jb.string_builder().as_cslice().str();
  auto heap_response = new std::string(json_response);
  return heap_response->c_str();
}

#define ERROR_RESPONSE(error) return error_response(error)

const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *shard_account_boc, const char *message_boc) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);
  
  auto message_decoded = td::base64_decode(td::Slice(message_boc));
  if (message_decoded.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't decode base64 message boc: " << message_decoded.move_as_error());
  }
  auto message_cell_r = vm::std_boc_deserialize(message_decoded.move_as_ok());
  if (message_cell_r.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't deserialize message boc: " << message_cell_r.move_as_error());
  }
  auto message_cell = message_cell_r.move_as_ok();

  auto shard_account_decoded = td::base64_decode(td::Slice(shard_account_boc));
  if (shard_account_decoded.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't decode base64 shard account boc: " << shard_account_decoded.move_as_error());
  }
  auto shard_account_cell = vm::std_boc_deserialize(shard_account_decoded.move_as_ok());
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
    auto cs = vm::load_cell_slice(message_cell);
    td::Ref<vm::CellSlice> dest;
    int msg_tag = block::gen::t_CommonMsgInfo.get_tag(cs);
    if (msg_tag == block::gen::CommonMsgInfo::ext_in_msg_info) {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(cs, info)) {
        ERROR_RESPONSE(PSTRING() <<  "Can't unpack inbound external message");
      }
      addr_slice = std::move(info.dest);
    }
    else if (msg_tag == block::gen::CommonMsgInfo::int_msg_info) {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
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

  auto result = emulator->emulate_transaction(std::move(account), message_cell);
  if (result.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Emulate transaction failed: " << result.move_as_error());
  }
  auto emulation_result = result.move_as_ok();

  auto external_not_accepted = dynamic_cast<emulator::TransactionEmulator::EmulationExternalNotAccepted *>(emulation_result.get());
  if (external_not_accepted) {
    return external_not_accepted_response(std::move(external_not_accepted->vm_log), external_not_accepted->vm_exit_code);
  }

  auto emulation_success = dynamic_cast<emulator::TransactionEmulator::EmulationSuccess&>(*emulation_result);
  auto trans_boc = vm::std_boc_serialize(std::move(emulation_success.transaction), vm::BagOfCells::Mode::WithCRC32C);
  if (trans_boc.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't serialize Transaction to boc" << trans_boc.move_as_error());
  }
  auto trans_boc_b64 = td::base64_encode(trans_boc.move_as_ok().as_slice());

  auto new_shard_account_cell = vm::CellBuilder().store_ref(emulation_success.account.total_state)
                               .store_bits(emulation_success.account.last_trans_hash_.as_bitslice())
                               .store_long(emulation_success.account.last_trans_lt_).finalize();
  auto new_shard_account_boc = vm::std_boc_serialize(std::move(new_shard_account_cell), vm::BagOfCells::Mode::WithCRC32C);
  if (new_shard_account_boc.is_error()) {
    ERROR_RESPONSE(PSTRING() << "Can't serialize ShardAccount to boc" << new_shard_account_boc.move_as_error());
  }
  auto new_shard_account_boc_b64 = td::base64_encode(new_shard_account_boc.move_as_ok().as_slice());

  return success_response(std::move(trans_boc_b64), std::move(new_shard_account_boc_b64), std::move(emulation_success.vm_log));
}

void transaction_emulator_destroy(void *transaction_emulator) {
  delete static_cast<emulator::TransactionEmulator *>(transaction_emulator);
}

void transaction_emulator_set_verbosity_level(int verbosity_level) {
  if (0 <= verbosity_level && verbosity_level <= VERBOSITY_NAME(NEVER)) {
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity_level);
  }
}