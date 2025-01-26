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
    args_.set_address(std::move(address));
    args_.set_now(unixtime);
    args_.set_balance(balance);
    args_.set_rand_seed(std::move(rand_seed));
    if (config) {
      args_.set_config(std::move(config));
    }
  }

  void set_extra_currencies(td::Ref<vm::Cell> extra_currencies) {
    args_.set_extra_currencies(std::move(extra_currencies));
  }

  void set_c7_raw(td::Ref<vm::Tuple> c7) {
    args_.set_c7(std::move(c7));
  }

  void set_config(std::shared_ptr<const block::Config> config) {
    args_.set_config(std::move(config));
  }

  void set_prev_blocks_info(td::Ref<vm::Tuple> tuple) {
    args_.set_prev_blocks_info(std::move(tuple));
  }

  void set_debug_enabled(bool debug_enabled) {
    args_.set_debug_enabled(debug_enabled);
  }

  Answer run_get_method(int method_id, td::Ref<vm::Stack> stack) {
    ton::SmartContract::Args args = args_;
    return smc_.run_get_method(args.set_stack(stack).set_method_id(method_id));
  }

  Answer send_external_message(td::Ref<vm::Cell> message_body) {
    return smc_.send_external_message(message_body, args_);
  }

  Answer send_internal_message(td::Ref<vm::Cell> message_body, uint64_t amount) {
    ton::SmartContract::Args args = args_;
    return smc_.send_internal_message(message_body, args.set_amount(amount));
  }
};
}