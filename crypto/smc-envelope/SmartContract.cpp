/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "SmartContract.h"

#include "GenericAccount.h"

#include "block/block.h"
#include "block/block-auto.h"
#include "vm/cellslice.h"
#include "vm/cp0.h"
#include "vm/memo.h"
#include "vm/vm.h"

#include "td/utils/crypto.h"

namespace ton {
int SmartContract::Answer::output_actions_count(td::Ref<vm::Cell> list) {
  int i = -1;
  do {
    ++i;
    list = load_cell_slice(std::move(list)).prefetch_ref();
  } while (list.not_null());
  return i;
}
namespace {

td::Ref<vm::Cell> build_internal_message(td::RefInt256 amount, td::Ref<vm::CellSlice> body, SmartContract::Args args) {
  vm::CellBuilder cb;
  if (args.address) {
    td::BigInt256 dest_addr;
    dest_addr.import_bits((*args.address).addr.as_bitslice());
    cb.store_ones(1)
        .store_zeroes(2)
        .store_long((*args.address).workchain, 8)
        .store_int256(dest_addr, 256);
  }
  auto address = cb.finalize();
  
  vm::CellBuilder b;
  b.store_long(0b0110, 4);                      // 0 ihr_disabled:Bool bounce:Bool bounced:Bool
  // use -1:00..00 as src:MsgAddressInt
  // addr_std$10 anycast:(Maybe Anycast)  workchain_id:int8 address:bits256  = MsgAddressInt;
  b.store_long(0b100, 3); b.store_ones(8); b.store_zeroes(256);
  b.append_cellslice(address);  // dest:MsgAddressInt
  unsigned len = (((unsigned)amount->bit_size(false) + 7) >> 3);
  b.store_long_bool(len, 4) && b.store_int256_bool(*amount, len * 8, false); // grams:Grams
  b.store_zeroes(1 + 4 + 4 + 64 + 32 + 1);      // extre, ihr_fee, fwd_fee, created_lt, created_at, init
  // body:(Either X ^X)
  if (b.remaining_bits() >= 1 + (*body).size() && b.remaining_refs() >= (*body).size_refs()) {
      b.store_zeroes(1);
      b.append_cellslice(body);
  } else {
      b.store_ones(1);
      b.store_ref(vm::CellBuilder().append_cellslice(body).finalize_novm());
  }
  return b.finalize_novm();
}

td::Ref<vm::Cell> build_external_message(td::RefInt256 amount, td::Ref<vm::CellSlice> body, SmartContract::Args args) {
  vm::CellBuilder cb;
  if (args.address) {
    td::BigInt256 dest_addr;
    dest_addr.import_bits((*args.address).addr.as_bitslice());
    cb.store_ones(1)
        .store_zeroes(2)
        .store_long((*args.address).workchain, 8)
        .store_int256(dest_addr, 256);
  }
  auto address = cb.finalize();
  
  vm::CellBuilder b;
  b.store_long(0b1000, 4);                      // ext_in_msg_info$10 src:MsgAddressExt
  b.append_cellslice(address);                  // dest:MsgAddressInt
  b.store_zeroes(4);                            //import_fee:Grams
  b.store_zeroes(1);                            // init
  // body:(Either X ^X)
  if (b.remaining_bits() >= 1 + (*body).size() && b.remaining_refs() >= (*body).size_refs()) {
      b.store_zeroes(1);
      b.append_cellslice(body);
  } else {
      b.store_ones(1);
      b.store_ref(vm::CellBuilder().append_cellslice(body).finalize_novm());
  }
  return b.finalize_novm();
}

td::Ref<vm::Stack> prepare_vm_stack(td::RefInt256 amount, td::Ref<vm::CellSlice> body, SmartContract::Args args, int selector) {
  td::Ref<vm::Stack> stack_ref{true};
  td::RefInt256 acc_addr{true};
  //CHECK(acc_addr.write().import_bits(account.addr.cbits(), 256));
  vm::Stack& stack = stack_ref.write();
  if(args.balance) {
    stack.push_int(td::make_refint(args.balance));
  } else {
    stack.push_int(td::make_refint(10000000000));
  }
  stack.push_int(amount);
  if(selector == 0) {
    stack.push_cell(build_internal_message(amount, body, args));
  } else {
    stack.push_cell(build_external_message(amount, body, args));
  }
  stack.push_cellslice(std::move(body));
  return stack_ref;
}

td::Ref<vm::Tuple> prepare_vm_c7(SmartContract::Args args) {
  td::BitArray<256> rand_seed;
  if (args.rand_seed) {
    rand_seed = args.rand_seed.unwrap();
  } else {
    rand_seed.as_slice().fill(0);
  }
  td::RefInt256 rand_seed_int{true};
  rand_seed_int.unique_write().import_bits(rand_seed.cbits(), 256, false);

  td::uint32 now = 0;
  if (args.now) {
    now = args.now.unwrap();
  }

  vm::CellBuilder cb;
  if (args.address) {
    td::BigInt256 dest_addr;
    dest_addr.import_bits((*args.address).addr.as_bitslice());
    cb.store_ones(1)
        .store_zeroes(2)
        .store_long((*args.address).workchain, 8)
        .store_int256(dest_addr, 256);
  }
  auto address = cb.finalize();
  auto config = td::Ref<vm::Cell>();

  if (args.config) {
    config = (*args.config)->get_root_cell();
  }

  auto tuple = vm::make_tuple_ref(
      td::make_refint(0x076ef1ea),                           // [ magic:0x076ef1ea
      td::make_refint(0),                                    //   actions:Integer
      td::make_refint(0),                                    //   msgs_sent:Integer
      td::make_refint(now),                                  //   unixtime:Integer
      td::make_refint(0),             //TODO:                //   block_lt:Integer
      td::make_refint(0),             //TODO:                //   trans_lt:Integer
      std::move(rand_seed_int),                              //   rand_seed:Integer
      block::CurrencyCollection(args.balance).as_vm_tuple(),      //   balance_remaining:[Integer (Maybe Cell)]
      vm::load_cell_slice_ref(address),  //  myself:MsgAddressInt
      vm::StackEntry::maybe(config)       //vm::StackEntry::maybe(td::Ref<vm::Cell>())
  );                                                         //  global_config:(Maybe Cell) ] = SmartContractInfo;
  //LOG(DEBUG) << "SmartContractInfo initialized with " << vm::StackEntry(tuple).to_string();
  return vm::make_tuple_ref(std::move(tuple));
}

SmartContract::Answer run_smartcont(SmartContract::State state, td::Ref<vm::Stack> stack, td::Ref<vm::Tuple> c7,
                                    vm::GasLimits gas, bool ignore_chksig, td::Ref<vm::Cell> libraries, int vm_log_verbosity, bool debug_enabled) {
  auto gas_credit = gas.gas_credit;
  vm::init_op_cp0(debug_enabled);
  vm::DictionaryBase::get_empty_dictionary();

  class Logger : public td::LogInterface {
   public:
    void append(td::CSlice slice) override {
      res.append(slice.data(), slice.size());
    }
    std::string res;
  };
  Logger logger;
  vm::VmLog log{&logger, td::LogOptions(VERBOSITY_NAME(DEBUG), true, false)};
  if (vm_log_verbosity > 1) {
    log.log_mask |= vm::VmLog::ExecLocation;
    if (vm_log_verbosity > 2) {
      log.log_mask |= vm::VmLog::DumpStack | vm::VmLog::GasRemaining;
    }
  }

  SmartContract::Answer res;
  if (GET_VERBOSITY_LEVEL() >= VERBOSITY_NAME(DEBUG)) {
    std::ostringstream os;
    stack->dump(os, 2);
    LOG(DEBUG) << "VM stack:\n" << os.str();
  }
  vm::VmState vm{state.code, std::move(stack), gas, 1, state.data, log};
  vm.set_c7(std::move(c7));
  vm.set_chksig_always_succeed(ignore_chksig);
  if (!libraries.is_null()) {
    vm.register_library_collection(libraries);
  }
  try {
    res.code = ~vm.run();
  } catch (...) {
    LOG(FATAL) << "catch unhandled exception";
  }
  res.new_state = std::move(state);
  res.stack = vm.get_stack_ref();
  gas = vm.get_gas_limits();
  res.gas_used = gas.gas_consumed();
  res.accepted = gas.gas_credit == 0;
  res.success = (res.accepted && vm.committed());
  res.vm_log = logger.res;
  if (GET_VERBOSITY_LEVEL() >= VERBOSITY_NAME(DEBUG)) {
    LOG(DEBUG) << "VM log\n" << logger.res;
    std::ostringstream os;
    res.stack->dump(os, 2);
    LOG(DEBUG) << "VM stack:\n" << os.str();
    LOG(DEBUG) << "VM exit code: " << res.code;
    LOG(DEBUG) << "VM accepted: " << res.accepted;
    LOG(DEBUG) << "VM success: " << res.success;
  }
  td::ConstBitPtr mlib = vm.get_missing_library();
  if (!mlib.is_null()) {
    LOG(DEBUG) << "Missing library: " << mlib.to_hex(256);
    res.missing_library = mlib;
  }
  if (res.success) {
    res.new_state.data = vm.get_c4();
    res.actions = vm.get_d(5);
    LOG(DEBUG) << "output actions:\n"
               << block::gen::OutList{res.output_actions_count(res.actions)}.as_string_ref(res.actions);
  }
  LOG_IF(ERROR, gas_credit != 0 && (res.accepted && !res.success) && mlib.is_null())
      << "Accepted but failed with code " << res.code << "\n"
      << res.gas_used << "\n";
  return res;
}
}  // namespace

td::Result<td::BufferSlice> SmartContract::Args::get_serialized_stack() {
  if (!stack) {
    return td::Status::Error("Args has no stack");
  }
  vm::FakeVmStateLimits fstate(1000);  // limit recursive (de)serialization calls
  vm::VmStateInterface::Guard guard(&fstate);
  // serialize parameters
  vm::CellBuilder cb;
  td::Ref<vm::Cell> cell;
  if (!(stack.value()->serialize(cb) && cb.finalize_to(cell))) {
    return td::Status::Error("Cannot serialize stack in args");
  }
  return vm::std_boc_serialize(std::move(cell));
}

td::Ref<vm::CellSlice> SmartContract::empty_slice() {
  return vm::load_cell_slice_ref(vm::CellBuilder().finalize());
}

size_t SmartContract::code_size() const {
  return vm::std_boc_serialize(state_.code).ok().size();
}
size_t SmartContract::data_size() const {
  return vm::std_boc_serialize(state_.data).ok().size();
}

block::StdAddress SmartContract::get_address(WorkchainId workchain_id) const {
  return GenericAccount::get_address(workchain_id, get_init_state());
}

td::Ref<vm::Cell> SmartContract::get_init_state() const {
  return GenericAccount::get_init_state(get_state().code, get_state().data);
}

SmartContract::Answer SmartContract::run_method(Args args) {
  if (!args.c7) {
    args.c7 = prepare_vm_c7(args);
  }
  if (!args.limits) {
    bool is_internal = args.get_method_id().ok() == 0;

    args.limits = vm::GasLimits{is_internal ? (long long)args.amount * 1000 : (long long)0, (long long)1000000,
                                is_internal ? 0 : (long long)10000};
  }
  CHECK(args.stack);
  CHECK(args.method_id);
  args.stack.value().write().push_smallint(args.method_id.unwrap());
  auto res =
      run_smartcont(get_state(), args.stack.unwrap(), args.c7.unwrap(), args.limits.unwrap(), args.ignore_chksig,
                    args.libraries ? args.libraries.unwrap().get_root_cell() : td::Ref<vm::Cell>{}, args.vm_log_verbosity_level, args.debug_enabled);
  state_ = res.new_state;
  return res;
}

SmartContract::Answer SmartContract::run_get_method(Args args) const {
  if (!args.c7) {
    args.c7 = prepare_vm_c7(args);
  }
  if (!args.limits) {
    args.limits = vm::GasLimits{1000000, 1000000};
  }
  if (!args.stack) {
    args.stack = td::Ref<vm::Stack>(true);
  }
  CHECK(args.method_id);
  args.stack.value().write().push_smallint(args.method_id.unwrap());
  return run_smartcont(get_state(), args.stack.unwrap(), args.c7.unwrap(), args.limits.unwrap(), args.ignore_chksig,
                       args.libraries ? args.libraries.unwrap().get_root_cell() : td::Ref<vm::Cell>{}, args.vm_log_verbosity_level, args.debug_enabled);
}

SmartContract::Answer SmartContract::run_get_method(td::Slice method, Args args) const {
  return run_get_method(args.set_method_id(method));
}

SmartContract::Answer SmartContract::send_external_message(td::Ref<vm::Cell> cell, Args args) {
  return run_method(
      args.set_stack(prepare_vm_stack(td::make_refint(0), vm::load_cell_slice_ref(cell), args, -1)).set_method_id(-1));
}
SmartContract::Answer SmartContract::send_internal_message(td::Ref<vm::Cell> cell, Args args) {
  return run_method(
      args.set_stack(prepare_vm_stack(td::make_refint(args.amount), vm::load_cell_slice_ref(cell), args, 0)).set_method_id(0));
}
}  // namespace ton
