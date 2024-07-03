#include "td/utils/tests.h"

#include "block/block-auto.h"
#include "block/block.h"
#include "block/block-parse.h"

#include "crypto/vm/boc.h"

#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/Time.h"

#include "smc-envelope/WalletV3.h"

#include "emulator/emulator-extern.h"

// testnet config as of 27.06.24
const char *config_boc = "te6cckICAl8AAQAANecAAAIBIAABAAICAtgAAwAEAgL1AA0ADgIBIAAFAAYCAUgCPgI/AgEgAAcACAIBSAAJAAoCASAAHgAfAgEgAGUAZgIBSAALAAwCAWoA0gDTAQFIAJIBAUgAsgEDpDMADwIBbgAQABEAQDPAueB1cC0DTaIjG28I/scJsoxoIScEE9LNtuiQoYa2AgOuIAASABMBA7LwABoBASAAFAEBIAAYAQHAABUCAWoAFgAXAIm/VzGVo387z8N7BhdH91LBHMMhBLu7nv21jwo9wtTSXQIBABvI0aFLnw2QbZgjMPCLRdtRHxhUyinQudg6sdiohIwgwCAAQ79oJ47o6vzJDO5wV60LQESEyBcI3zuSSKtFQIlzhk86tAMBg+mbgbrrZVY0qEWL8HxF+gYzy9t5jLO50+QkJ2DWbWFHj0Qaw5TPlNDYOnY0A2VNeAnS9bZ98W8X7FTvgVqStlmABAAZAIOgCYiOTH0TnIIa0oSKjkT3CsgHNUU1Iy/5E472ortANeCAAAAAAAAAAAAAAAAROiXXYZuWf8AAi5Oy+xV/i+2JL9ABA6BgABsCASAAHAAdAFur4AAAAAAHGv1JjQAAEeDul1fav9HZ8+939/IsLGZ46E5h3qjR13yIrB8mcfbBAFur/////8AHGv1JjQAAEeDul1fav9HZ8+939/IsLGZ46E5h3qjR13yIrB8mcfbBAgEgACAAIQIBIAAzADQCASAAIgAjAgEgACkAKgIBIAAkACUBAUgAKAEBIAAmAQEgACcAQFVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVAEAzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMwBAAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQECASAAKwAsAQFYAC8BASAALQEBIAAuAEDv5x0Thgr6pq6ur2NvkWhIf4DxAxsL+Nk5rknT6n99oABTAf//////////////////////////////////////////gAAAAIAAAAFAAQHAADACASAAMQAyABW+AAADvLNnDcFVUAAVv////7y9GpSiABACASAANQA2AgEgADcAOAIBIABCAEMCASAATgBPAgEgADkAOgIBIAA+AD8BASAAOwEBIAA9AQHAADwAt9BTLudOzwABAnAA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";


constexpr td::int64 Ton = 1000000000;

TEST(Emulator, wallet_int_and_ext_msg) {
  td::Ed25519::PrivateKey priv_key = td::Ed25519::generate_private_key().move_as_ok();
  auto pub_key = priv_key.get_public_key().move_as_ok();
  ton::WalletV3::InitData init_data;
  init_data.public_key = pub_key.as_octet_string();
  init_data.wallet_id = 239;
  auto wallet = ton::WalletV3::create(init_data, 2);

  auto address = wallet->get_address();

  void *emulator = transaction_emulator_create(config_boc, 3);
  const uint64_t lt = 42000000000;
  CHECK(transaction_emulator_set_lt(emulator, lt));
  const uint32_t utime = 1337;
  transaction_emulator_set_unixtime(emulator, utime);

  std::string shard_account_after_boc_b64;

  // emulate internal message with init state on uninit account
  {
    td::Ref<vm::Cell> account_root;
    block::gen::Account().cell_pack_account_none(account_root);
    auto none_shard_account_cell = vm::CellBuilder().store_ref(account_root).store_bits(td::Bits256::zero().as_bitslice()).store_long(0).finalize();
    auto none_shard_account_boc = td::base64_encode(std_boc_serialize(none_shard_account_cell).move_as_ok());

    td::Ref<vm::Cell> int_msg;
    {
      block::gen::Message::Record message;
      block::gen::CommonMsgInfo::Record_int_msg_info msg_info;
      msg_info.ihr_disabled = true;
      msg_info.bounce = false;
      msg_info.bounced = false;
      {
        block::gen::MsgAddressInt::Record_addr_std src;
        src.anycast = vm::CellBuilder().store_zeroes(1).as_cellslice_ref();
        src.workchain_id = 0;
        src.address = td::Bits256();;
        tlb::csr_pack(msg_info.src, src);
      }
      {
        block::gen::MsgAddressInt::Record_addr_std dest;
        dest.anycast = vm::CellBuilder().store_zeroes(1).as_cellslice_ref();
        dest.workchain_id = address.workchain;
        dest.address =  address.addr;
        tlb::csr_pack(msg_info.dest, dest);
      }
      {
        block::CurrencyCollection cc{10 * Ton};
        cc.pack_to(msg_info.value);
      }
      {
        vm::CellBuilder cb;
        block::tlb::t_Grams.store_integer_value(cb, td::BigInt256(int(0.03 * Ton)));
        msg_info.fwd_fee = cb.as_cellslice_ref();
      }
      {
        vm::CellBuilder cb;
        block::tlb::t_Grams.store_integer_value(cb, td::BigInt256(0));
        msg_info.ihr_fee = cb.as_cellslice_ref();
      }
      msg_info.created_lt = 0;
      msg_info.created_at = static_cast<uint32_t>(td::Time::now());
      tlb::csr_pack(message.info, msg_info);
      message.init = vm::CellBuilder()
                            .store_ones(1)
                            .store_zeroes(1)
                            .append_cellslice(vm::load_cell_slice(ton::GenericAccount::get_init_state(wallet->get_state())))
                            .as_cellslice_ref();
      message.body = vm::CellBuilder().store_zeroes(1).as_cellslice_ref();

      tlb::type_pack_cell(int_msg, block::gen::t_Message_Any, message);
    }

    CHECK(int_msg.not_null());

    auto int_msg_boc = td::base64_encode(std_boc_serialize(int_msg).move_as_ok());

    std::string int_emu_res = transaction_emulator_emulate_transaction(emulator, none_shard_account_boc.c_str(), int_msg_boc.c_str());

    auto int_result_json = td::json_decode(td::MutableSlice(int_emu_res));
    CHECK(int_result_json.is_ok());
    auto int_result_value = int_result_json.move_as_ok();
    auto& int_result_obj = int_result_value.get_object();

    auto success_field = td::get_json_object_field(int_result_obj, "success", td::JsonValue::Type::Boolean, false);
    CHECK(success_field.is_ok());
    auto success = success_field.move_as_ok().get_boolean();
    CHECK(success);

    auto transaction_field = td::get_json_object_field(int_result_obj, "transaction", td::JsonValue::Type::String, false);
    CHECK(transaction_field.is_ok());
    auto transaction_boc_b64 = transaction_field.move_as_ok().get_string();
    auto transaction_boc = td::base64_decode(transaction_boc_b64);
    CHECK(transaction_boc.is_ok());
    auto trans_cell = vm::std_boc_deserialize(transaction_boc.move_as_ok());
    CHECK(trans_cell.is_ok());
    auto trans_hash = trans_cell.ok()->get_hash().bits();
    block::gen::Transaction::Record trans;
    block::gen::TransactionDescr::Record_trans_ord trans_descr;
    CHECK(tlb::unpack_cell(trans_cell.move_as_ok(), trans) && tlb::unpack_cell(trans.description, trans_descr));
    CHECK(trans.outmsg_cnt == 0);
    CHECK(trans.account_addr == wallet->get_address().addr);
    CHECK(trans_descr.aborted == false);
    CHECK(trans_descr.destroyed == false);
    CHECK(trans.lt == lt);
    CHECK(trans.now == utime);

    auto shard_account_field = td::get_json_object_field(int_result_obj, "shard_account", td::JsonValue::Type::String, false);
    CHECK(shard_account_field.is_ok());
    auto shard_account_boc_b64 = shard_account_field.move_as_ok().get_string();
    shard_account_after_boc_b64 = shard_account_boc_b64.str();
    auto shard_account_boc = td::base64_decode(shard_account_boc_b64);
    CHECK(shard_account_boc.is_ok());
    auto shard_account_cell = vm::std_boc_deserialize(shard_account_boc.move_as_ok());
    CHECK(shard_account_cell.is_ok());
    block::gen::ShardAccount::Record shard_account;
    block::gen::Account::Record_account account;
    CHECK(tlb::unpack_cell(shard_account_cell.move_as_ok(), shard_account) && tlb::unpack_cell(shard_account.account, account));
    CHECK(shard_account.last_trans_hash == trans_hash);
    CHECK(shard_account.last_trans_lt == lt);
    ton::WorkchainId wc;
    ton::StdSmcAddress addr;
    CHECK(block::tlb::t_MsgAddressInt.extract_std_address(account.addr, wc, addr));
    CHECK(address.workchain == wc);
    CHECK(address.addr == addr);
  }
  
  // emulate external message
  {
    auto ext_body = wallet->make_a_gift_message(priv_key, static_cast<uint32_t>(td::Time::now()) + 60, {ton::WalletV3::Gift{block::StdAddress(0, ton::StdSmcAddress()), 1 * Ton}});
    CHECK(ext_body.is_ok());
    auto ext_msg = ton::GenericAccount::create_ext_message(address, {}, ext_body.move_as_ok());
    auto ext_msg_boc = td::base64_encode(std_boc_serialize(ext_msg).move_as_ok());
    std::string ext_emu_res = transaction_emulator_emulate_transaction(emulator, shard_account_after_boc_b64.c_str(), ext_msg_boc.c_str());

    auto ext_result_json = td::json_decode(td::MutableSlice(ext_emu_res));
    CHECK(ext_result_json.is_ok());
    auto ext_result = ext_result_json.move_as_ok();
    auto &ext_result_obj = ext_result.get_object();
    auto ext_success_field = td::get_json_object_field(ext_result_obj, "success", td::JsonValue::Type::Boolean, false);
    CHECK(ext_success_field.is_ok());
    auto ext_success = ext_success_field.move_as_ok().get_boolean();
    CHECK(ext_success);

    auto ext_transaction_field = td::get_json_object_field(ext_result_obj, "transaction", td::JsonValue::Type::String, false);
    CHECK(ext_transaction_field.is_ok());
    auto ext_transaction_boc_b64 = ext_transaction_field.move_as_ok().get_string();
    auto ext_transaction_boc = td::base64_decode(ext_transaction_boc_b64);
    CHECK(ext_transaction_boc.is_ok());
    auto ext_trans_cell = vm::std_boc_deserialize(ext_transaction_boc.move_as_ok());
    CHECK(ext_trans_cell.is_ok());
    auto ext_trans_hash = ext_trans_cell.ok()->get_hash().bits();
    block::gen::Transaction::Record ext_trans;
    block::gen::TransactionDescr::Record_trans_ord ext_trans_descr;
    CHECK(tlb::unpack_cell(ext_trans_cell.move_as_ok(), ext_trans) && tlb::unpack_cell(ext_trans.description, ext_trans_descr));
    CHECK(ext_trans.outmsg_cnt == 1);
    CHECK(ext_trans.account_addr == wallet->get_address().addr);
    CHECK(ext_trans_descr.aborted == false);
    CHECK(ext_trans_descr.destroyed == false);

    auto ext_shard_account_field = td::get_json_object_field(ext_result_obj, "shard_account", td::JsonValue::Type::String, false);
    CHECK(ext_shard_account_field.is_ok());
    auto ext_shard_account_boc_b64 = ext_shard_account_field.move_as_ok().get_string();
    auto ext_shard_account_boc = td::base64_decode(ext_shard_account_boc_b64);
    CHECK(ext_shard_account_boc.is_ok());
    auto ext_shard_account_cell = vm::std_boc_deserialize(ext_shard_account_boc.move_as_ok());
    CHECK(ext_shard_account_cell.is_ok());
    block::gen::ShardAccount::Record ext_shard_account;
    block::gen::Account::Record_account ext_account;
    CHECK(tlb::unpack_cell(ext_shard_account_cell.move_as_ok(), ext_shard_account) && tlb::unpack_cell(ext_shard_account.account, ext_account));
    CHECK(ext_shard_account.last_trans_hash == ext_trans_hash);
    CHECK(ext_shard_account.last_trans_lt == ext_trans.lt);
    ton::WorkchainId wc;
    ton::StdSmcAddress addr;
    CHECK(block::tlb::t_MsgAddressInt.extract_std_address(ext_account.addr, wc, addr));
    CHECK(address.workchain == wc);
    CHECK(address.addr == addr);
  }
}

TEST(Emulator, tvm_emulator) {
  td::Ed25519::PrivateKey priv_key = td::Ed25519::generate_private_key().move_as_ok();
  auto pub_key = priv_key.get_public_key().move_as_ok();
  ton::WalletV3::InitData init_data;
  init_data.public_key = pub_key.as_octet_string();
  init_data.wallet_id = 239;
  init_data.seqno = 1337;
  auto wallet = ton::WalletV3::create(init_data, 2);

  auto code = ton::SmartContractCode::get_code(ton::SmartContractCode::Type::WalletV3, 2);
  auto code_boc_b64 = td::base64_encode(std_boc_serialize(code).move_as_ok());
  auto data = ton::WalletV3::get_init_data(init_data);
  auto data_boc_b64 = td::base64_encode(std_boc_serialize(data).move_as_ok());

  void *tvm_emulator = tvm_emulator_create(code_boc_b64.c_str(), data_boc_b64.c_str(), 1);
  unsigned method_crc = td::crc16("seqno");
  unsigned method_id = (method_crc & 0xffff) | 0x10000;
  auto stack = td::make_ref<vm::Stack>();
  vm::CellBuilder stack_cb;
  CHECK(stack->serialize(stack_cb));
  auto stack_cell = stack_cb.finalize();
  auto stack_boc = td::base64_encode(std_boc_serialize(stack_cell).move_as_ok());

  char addr_buffer[49] = {0};
  CHECK(wallet->get_address().rserialize_to(addr_buffer));

  auto rand_seed = std::string(64, 'F');
  CHECK(tvm_emulator_set_c7(tvm_emulator, addr_buffer, static_cast<uint32_t>(td::Time::now()), 10 * Ton, rand_seed.c_str(), config_boc));
  std::string tvm_res = tvm_emulator_run_get_method(tvm_emulator, method_id, stack_boc.c_str());
  
  auto result_json = td::json_decode(td::MutableSlice(tvm_res));
  CHECK(result_json.is_ok());
  auto result = result_json.move_as_ok();
  auto& result_obj = result.get_object();

  auto success_field = td::get_json_object_field(result_obj, "success", td::JsonValue::Type::Boolean, false);
  CHECK(success_field.is_ok());
  auto success = success_field.move_as_ok().get_boolean();
  CHECK(success);

  auto stack_field = td::get_json_object_field(result_obj, "stack", td::JsonValue::Type::String, false);
  CHECK(stack_field.is_ok());
  auto stack_val = stack_field.move_as_ok();
  auto& stack_obj = stack_val.get_string();
  auto stack_res_boc = td::base64_decode(stack_obj);
  CHECK(stack_res_boc.is_ok());
  auto stack_res_cell = vm::std_boc_deserialize(stack_res_boc.move_as_ok());
  CHECK(stack_res_cell.is_ok());
  td::Ref<vm::Stack> stack_res;
  auto stack_res_cs = vm::load_cell_slice(stack_res_cell.move_as_ok());
  CHECK(vm::Stack::deserialize_to(stack_res_cs, stack_res));
  CHECK(stack_res->depth() == 1);
  CHECK(stack_res.write().pop_int()->to_long() == init_data.seqno);
}
