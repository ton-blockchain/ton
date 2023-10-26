#pragma once
#include "smc-envelope/SmartContract.h"

namespace emulator {
class TvmEmulator {
  ton::SmartContract smc_;
  ton::SmartContract::Args args_;
public:
  using Answer = ton::SmartContract::Answer;

  TvmEmulator(td::Ref<vm::Cell> code, td::Ref<vm::Cell> data): smc_({code, data}) {
  }

  void set_vm_verbosity_level(int vm_log_verbosity) {
    args_.set_vm_verbosity_level(vm_log_verbosity);
  }

  void set_libraries(vm::Dictionary&& libraries) {
    args_.set_libraries(libraries);
  }

  void set_gas_limit(int64_t limit) {
    args_.set_limits(vm::GasLimits(limit));
  }

  void set_c7(block::StdAddress address, uint32_t unixtime, uint64_t balance, td::BitArray<256> rand_seed, std::shared_ptr<const block::Config> config) {
    args_.set_address(address);
    args_.set_now(unixtime);
    args_.set_balance(balance);
    args_.set_rand_seed(rand_seed);
    args_.set_config(config);
  }

  void set_debug_enabled(bool debug_enabled) {
    args_.set_debug_enabled(debug_enabled);
  }

  Answer run_get_method(int method_id, td::Ref<vm::Stack> stack) {
    return smc_.run_get_method(args_.set_stack(stack).set_method_id(method_id));
  }

  Answer send_external_message(td::Ref<vm::Cell> message_body) {
    return smc_.send_external_message(message_body, args_);
  }

  Answer send_internal_message(td::Ref<vm::Cell> message_body, uint64_t amount) {
    return smc_.send_internal_message(message_body, args_.set_amount(amount));
  }
};
}