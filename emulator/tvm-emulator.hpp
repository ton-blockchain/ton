#pragma once
#include "smc-envelope/SmartContract.h"
#include "vm/continuation.h"
#include "vm/excno.hpp"

namespace emulator {
class TvmEmulator {
  ton::SmartContract smc_;
  ton::SmartContract::Args args_;
  /**
   * Instance of VM used in step by step mode, otherwise it is nullptr.
   */
  std::unique_ptr<vm::VmState> vm{};
  /**
   * Logger used in step by step mode, otherwise it is nullptr.
   */
  std::unique_ptr<ton::SmartContract::Logger> logger{};

 public:
  vm::ExtMethods ext_methods;
  vm::MissingLibraryHandler missing_library_handler;
  using Answer = ton::SmartContract::Answer;

  TvmEmulator(td::Ref<vm::Cell> code, td::Ref<vm::Cell> data) : smc_({code, data}) {
  }

  void register_ext_method(td::uint64 method_id, void* ctx, vm::ExtMethodCallback method, td::uint8 stack_items_count = 255) {
    ext_methods[method_id] = vm::ExtMethod{ctx, method, stack_items_count};
  }

  void register_missing_library_handler(void* ctx, vm::MissingLibraryCallback callback) {
    missing_library_handler = {ctx, callback};
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

  void set_c7(block::StdAddress address, uint32_t unixtime, uint64_t balance, td::BitArray<256> rand_seed,
              std::shared_ptr<const block::Config> config) {
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

  const vm::VmState& get_vm() const {
    return *vm;
  }

  td::optional<bool> debug_step() {
    return smc_.debug_step(vm, logger);
  }

  int run_get_method_debug(int method_id, td::Ref<vm::Stack> stack) {
    return smc_.run_get_method_debug(args_.set_stack(stack)
                                         .set_method_id(method_id)
                                         .set_ext_methods(ext_methods)
                                         .set_missing_library_handler(missing_library_handler),
                                     vm, logger);
  }

  Answer sbs_result() {
    auto result = smc_.get_result(*vm, logger.get());
    vm = nullptr;
    logger = nullptr;
    return result;
  }

  Answer run_get_method(int method_id, td::Ref<vm::Stack> stack) {
    ton::SmartContract::Args args = args_;
    return smc_.run_get_method(args.set_stack(stack)
                                   .set_method_id(method_id)
                                   .set_ext_methods(ext_methods)
                                   .set_missing_library_handler(missing_library_handler));
  }

  Answer run_continuation(td::Ref<vm::Continuation> cont, td::Ref<vm::Stack> stack) {
    vm::init_vm(args_.debug_enabled).ensure();
    vm::DictionaryBase::get_empty_dictionary();

    if (args_.vm_log_verbosity_level >= 0) {
      logger = std::make_unique<ton::SmartContract::Logger>();
      logger->clear();
    } else {
      logger.reset();
    }

    auto gas = args_.limits ? args_.limits.unwrap() : vm::GasLimits{1000000, 1000000};

    auto log = vm::make_vm_log(logger.get(), args_.vm_log_verbosity_level);

    auto state = smc_.get_state();
    int global_version = args_.config ? args_.config.value()->get_global_version() : ton::SUPPORTED_VERSION;

    // Use contract code so c3 (method dictionary) is available for CALLDICT.
    auto vm_state = vm::VmState(state.code, global_version, std::move(stack), gas, 1,
                                state.data, log);
    vm_state.ext_methods = ext_methods;
    vm_state.missing_library_handler = missing_library_handler;

    // Jump to the continuation
    vm_state.jump(std::move(cont));

    try {
      vm_state.run();
    } catch (...) {
      // VM execution failed
    }

    vm = std::make_unique<vm::VmState>(std::move(vm_state));
    return sbs_result();
  }

  Answer send_external_message(td::Ref<vm::Cell> message_body) {
    ton::SmartContract::Args args = args_;
    return smc_.send_external_message(
        message_body, args.set_ext_methods(ext_methods).set_missing_library_handler(missing_library_handler));
  }

  Answer send_internal_message(td::Ref<vm::Cell> message_body, uint64_t amount) {
    ton::SmartContract::Args args = args_;
    return smc_.send_internal_message(
        message_body,
        args.set_amount(amount).set_ext_methods(ext_methods).set_missing_library_handler(missing_library_handler));
  }
};
}  // namespace emulator
