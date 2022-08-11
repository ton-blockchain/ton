#include "emulator-extern.h"
#include "td/utils/base64.h"
#include "td/utils/Status.h"
#include "transaction-emulator.h"

/**
 * Creates TransactionEmulator object
 * @param shard_account_boc Base64 encoded BoC serialized ShardAccount
 * @param config_params_boc Base64 encoded BoC serialized Config hashmap: (Hashmap 32 ^Cell)
 * @return Pointer to TransactionEmulator
 */
void *transaction_emulator_create(const char *shard_account_boc, const char *config_params_boc) {
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
  auto global_config = block::Config(config_params_cell.move_as_ok(), td::Bits256::zero(), block::Config::needWorkchainInfo);
  auto unpack_res = global_config.unpack();
  if (unpack_res.is_error()) {
    LOG(ERROR) << "Can't unpack config params";
    return nullptr;
  }

  auto shard_account_decoded = td::base64_decode(td::Slice(shard_account_boc));
  if (shard_account_decoded.is_error()) {
    LOG(ERROR) << "Can't decode base64 shard account boc: " << shard_account_decoded.move_as_error();
    return nullptr;
  }
  auto shard_account_cell = vm::std_boc_deserialize(shard_account_decoded.move_as_ok());
  if (shard_account_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize shard account boc: " << shard_account_cell.move_as_error();
    return nullptr;
  }
  auto shard_account_slice = vm::load_cell_slice(shard_account_cell.ok_ref());
  block::gen::ShardAccount::Record shard_account;
  if (!tlb::unpack(shard_account_slice, shard_account)) {
    LOG(ERROR) << "Can't unpack shard account";
    return nullptr;
  }
  block::gen::Account::Record_account account_record;
  auto account_slice = vm::load_cell_slice(shard_account.account);
  if (!tlb::unpack(account_slice, account_record)) {
    LOG(ERROR) << "Can't unpack shard account";
    return nullptr;
  }
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(account_record.addr, wc, addr)) {
    LOG(ERROR) << "Can't extract account address";
    return nullptr;
  }
  auto account = block::Account(wc, addr.bits());
  ton::UnixTime now = (unsigned)std::time(nullptr); // TODO: check this param 
  if (!account.unpack(vm::load_cell_slice_ref(shard_account_cell.move_as_ok()), td::Ref<vm::CellSlice>(), now,
                      wc == ton::masterchainId && global_config.is_special_smartcontract(addr))) {
      LOG(ERROR) << "Can't unpack shard account";
      return nullptr;
  }

  return new emulator::TransactionEmulator(std::move(account), std::move(global_config), vm::Dictionary{256}); // TODO: add libraries as input
}

/**
 * Getter for ShardAccount object
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @return Base64 encodedBoC serialized ShardAccount object
 */
const char *transaction_emulator_get_shard_account(void *transaction_emulator) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);
  auto account = emulator->get_account();
  auto cell = vm::CellBuilder().store_ref(account.total_state).store_bits(account.last_trans_hash_.as_bitslice()).store_long(account.last_trans_lt_).finalize();
  auto boc = vm::std_boc_serialize(std::move(cell), vm::BagOfCells::Mode::WithCRC32C);
  if (boc.is_error()) {
    LOG(ERROR) << "Can't serialize ShardAccount to boc" << boc.move_as_error();
    return nullptr;
  }
  auto res = td::base64_encode(boc.move_as_ok().as_slice());
  auto heap_res = new std::string(res);
  return heap_res->c_str();
}

/**
 * Getter for Config object. 
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @return Base64 encoded BoC serialized Config object.
 */
const char *transaction_emulator_get_config(void *transaction_emulator) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);
  auto config = emulator->get_config();
  auto boc = vm::std_boc_serialize(config->get_root_cell(), vm::BagOfCells::Mode::WithCRC32C);
  if (boc.is_error()) {
    LOG(ERROR) << "Can't serialize Config to boc" << boc.move_as_error();
    return nullptr;
  }
  auto res = td::base64_encode(boc.move_as_ok().as_slice());
  auto heap_res = new std::string(res);
  return heap_res->c_str();
}

/**
 * Emulate transaction
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param message_boc Base64 encoded BoC serialized inbound Message (internal or external)
 * @return Base64 encoded BoC serialized Transaction object
 */
const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *message_boc) {
  auto emulator = static_cast<emulator::TransactionEmulator *>(transaction_emulator);
  
  auto message_decoded = td::base64_decode(td::Slice(message_boc));
  if (message_decoded.is_error()) {
    LOG(ERROR) << "Can't decode base64 message boc: " << message_decoded.move_as_error();
    return nullptr;
  }
  auto message_cell = vm::std_boc_deserialize(message_decoded.move_as_ok());
  if (message_cell.is_error()) {
    LOG(ERROR) << "Can't deserialize message boc: " << message_cell.move_as_error();
    return nullptr;
  }

  auto transaction = emulator->emulate_transaction(message_cell.move_as_ok());
  if (transaction.is_error()) {
    LOG(ERROR) << "Emulate transaction failed: " << transaction.move_as_error();
    return nullptr;
  }

  auto boc = vm::std_boc_serialize(transaction.move_as_ok(), vm::BagOfCells::Mode::WithCRC32C);
  if (boc.is_error()) {
    LOG(ERROR) << "Can't serialize Config to boc" << boc.move_as_error();
    return nullptr;
  }
  auto res = td::base64_encode(boc.move_as_ok().as_slice());
  auto heap_res = new std::string(res);
  return heap_res->c_str();
}

void transaction_emulator_destroy(void *transaction_emulator) {
  delete static_cast<emulator::TransactionEmulator *>(transaction_emulator);
}
