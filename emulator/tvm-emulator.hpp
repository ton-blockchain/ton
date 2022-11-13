#pragma once
#include "smc-envelope/SmartContract.h"

namespace emulator {
class TvmEmulator {
  ton::SmartContract smc_;
  ton::SmartContract::Args args_;
public:
  struct GetMethodResult {
    int vm_exit_code;
    td::Ref<vm::Stack> stack;
    uint64_t gas_used;
    std::string vm_log;
    td::optional<td::Bits256> missing_library;
    
    GetMethodResult(int vm_exit_code_, td::Ref<vm::Stack> stack_, uint64_t gas_used_, std::string vm_log_, td::optional<td::Bits256> missing_library_) 
      : vm_exit_code(vm_exit_code_), stack(stack_), gas_used(gas_used_), vm_log(vm_log_), missing_library(missing_library_) {}
  };

  TvmEmulator(td::Ref<vm::Cell> code, td::Ref<vm::Cell> data): smc_({code, data}) {
  }

  void set_vm_verbosity_level(int vm_log_verbosity) {
    args_.set_vm_verbosity_level(vm_log_verbosity);
  }

  void set_libraries(vm::Dictionary&& libraries) {
    args_.set_libraries(libraries);
  }

  void set_c7(block::StdAddress address, uint32_t unixtime, uint64_t balance, std::shared_ptr<const block::Config> config) {
    args_.set_address(address);
    args_.set_now(unixtime);
    args_.set_balance(balance);
    args_.set_config(config);
  }

  GetMethodResult run_get_method(int method_id, td::Ref<vm::Stack> stack) {
    auto answer = smc_.run_get_method(args_.set_stack(stack).set_method_id(method_id));

    return GetMethodResult(answer.code, answer.stack, answer.gas_used, answer.vm_log, answer.missing_library.is_null() ? td::optional<td::Bits256>() : answer.missing_library);
  }
};
}