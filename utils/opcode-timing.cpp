#include <ctime>
#include <iomanip>

#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/dict.h"
#include "fift/utils.h"
#include "common/bigint.hpp"

#include "td/utils/base64.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/Timer.h"
#include "block.h"
#include "td/utils/filesystem.h"
#include "mc-config.h"

td::Ref<vm::Tuple> c7;

void prepare_c7() {
  auto now = (td::uint32)td::Clocks::system();
  td::Ref<vm::Cell> config_root;
  auto config_data = td::read_file("config.boc");
  if (config_data.is_ok()) {
    LOG(WARNING) << "Reading config from config.boc";
    auto r_cell = vm::std_boc_deserialize(config_data.move_as_ok());
    r_cell.ensure();
    config_root = r_cell.move_as_ok();
  }

  vm::CellBuilder addr;
  addr.store_long(4, 3);
  addr.store_long(0, 8);
  addr.store_ones(256);
  std::vector<vm::StackEntry> tuple = {
      td::make_refint(0x076ef1ea),  // [ magic:0x076ef1ea
      td::zero_refint(),            //   actions:Integer
      td::zero_refint(),            //   msgs_sent:Integer
      td::make_refint(now),         //   unixtime:Integer
      td::make_refint(0),           //   block_lt:Integer
      td::make_refint(0),           //   trans_lt:Integer
      td::make_refint(123),         //   rand_seed:Integer
      block::CurrencyCollection(td::make_refint(10000LL * 1000000000))
          .as_vm_tuple(),                 //   balance_remaining:[Integer (Maybe Cell)]
      addr.as_cellslice_ref(),            //   myself:MsgAddressInt
      vm::StackEntry::maybe(config_root)  //   global_config:(Maybe Cell) ] = SmartContractInfo;
  };
  tuple.push_back({});  // code:Cell
  tuple.push_back(block::CurrencyCollection(td::make_refint(2000LL * 1000000000))
                      .as_vm_tuple());  // in_msg_value:[Integer (Maybe Cell)]
  tuple.push_back(td::make_refint(0));  // storage_fees:Integer
  tuple.push_back(vm::StackEntry());    // prev_blocks_info
  if (config_root.not_null()) {
    block::Config config{config_root};
    config.unpack().ensure();
    tuple.push_back(config.get_unpacked_config_tuple(now));  // unpacked_config_tuple
  } else {
    tuple.push_back(vm::StackEntry());
  }
  tuple.push_back(td::zero_refint());
  auto tuple_ref = td::make_cnt_ref<std::vector<vm::StackEntry>>(std::move(tuple));
  c7 = vm::make_tuple_ref(std::move(tuple_ref));
}

td::Ref<vm::Cell> to_cell(td::Slice s) {
  if (s.size() >= 4 && s.substr(0, 4) == "boc:") {
    s.remove_prefix(4);
    auto boc = td::base64_decode(s).move_as_ok();
    return vm::std_boc_deserialize(boc).move_as_ok();
  }
  unsigned char buff[128];
  const int bits = (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), s.begin(), s.end());
  CHECK(bits >= 0);
  return vm::CellBuilder().store_bits(buff, bits, 0).finalize();
}

typedef struct {
  long double mean;
  long double stddev;
} stats;

struct runInfo {
  long double runtime;
  long long gasUsage;
  int vmReturnCode;

  runInfo() : runtime(0.0), gasUsage(0), vmReturnCode(0) {
  }
  runInfo(long double runtime, long long gasUsage, int vmReturnCode)
      : runtime(runtime), gasUsage(gasUsage), vmReturnCode(vmReturnCode) {
  }

  runInfo operator+(const runInfo& addend) const {
    return {runtime + addend.runtime, gasUsage + addend.gasUsage, vmReturnCode ? vmReturnCode : addend.vmReturnCode};
  }

  runInfo& operator+=(const runInfo& addend) {
    runtime += addend.runtime;
    gasUsage += addend.gasUsage;
    if (!vmReturnCode && addend.vmReturnCode) {
      vmReturnCode = addend.vmReturnCode;
    }
    return *this;
  }

  bool errored() const {
    return vmReturnCode != 0;
  }
};

typedef struct {
  stats runtime;
  stats gasUsage;
  bool errored;
} runtimeStats;

vm::Stack prepare_stack(td::Slice command) {
  const auto cell = to_cell(command);
  vm::DictionaryBase::get_empty_dictionary();
  vm::Stack stack;
  try {
    vm::GasLimits gas_limit;
    int ret = vm::run_vm_code(vm::load_cell_slice_ref(cell), stack, 0 /*flags*/, nullptr /*data*/, vm::VmLog{}, nullptr,
                              &gas_limit, {}, c7, nullptr, ton::SUPPORTED_VERSION);
    CHECK(ret == 0);
  } catch (...) {
    LOG(FATAL) << "catch unhandled exception";
  }
  return stack;
}

runInfo time_run_vm(td::Slice command, td::Ref<vm::Stack> stack) {
  const auto cell = to_cell(command);
  vm::DictionaryBase::get_empty_dictionary();
  CHECK(stack.is_unique());
  try {
    vm::GasLimits gas_limit;
    vm::VmState vm{
        vm::load_cell_slice_ref(cell), ton::SUPPORTED_VERSION, std::move(stack), gas_limit, 0, {}, vm::VmLog{}, {}, c7};
    std::clock_t cStart = std::clock();
    int ret = ~vm.run();
    std::clock_t cEnd = std::clock();
    const auto time = (1000.0 * static_cast<long double>(cEnd - cStart) / CLOCKS_PER_SEC);
    return {time >= 0 ? time : 0, vm.gas_consumed(), ret};
  } catch (...) {
    LOG(FATAL) << "catch unhandled exception";
    return {-1, -1, 1};
  }
}

runtimeStats averageRuntime(td::Slice command, const vm::Stack& stack) {
  size_t samples = 100000;
  runInfo total;
  std::vector<runInfo> values;
  values.reserve(samples);
  td::Timer t0;
  for (size_t i = 0; i < samples; ++i) {
    const auto value_empty = time_run_vm(td::Slice(""), td::Ref<vm::Stack>(true, stack));
    const auto value_code = time_run_vm(command, td::Ref<vm::Stack>(true, stack));
    runInfo value{value_code.runtime - value_empty.runtime, value_code.gasUsage - value_empty.gasUsage,
                  value_code.vmReturnCode};
    values.push_back(value);
    total += value;
    if (t0.elapsed() > 2.0 && i + 1 >= 20) {
      samples = i + 1;
      values.resize(samples);
      break;
    }
  }
  const auto runtimeMean = total.runtime / static_cast<long double>(samples);
  const auto gasMean = static_cast<long double>(total.gasUsage) / static_cast<long double>(samples);
  long double runtimeDiffSum = 0.0;
  long double gasDiffSum = 0.0;
  bool errored = false;
  for (const auto value : values) {
    const auto runtime = value.runtime - runtimeMean;
    const auto gasUsage = static_cast<long double>(value.gasUsage) - gasMean;
    runtimeDiffSum += runtime * runtime;
    gasDiffSum += gasUsage * gasUsage;
    errored = errored || value.errored();
  }
  return {{runtimeMean, sqrtl(runtimeDiffSum / static_cast<long double>(samples))},
          {gasMean, sqrtl(gasDiffSum / static_cast<long double>(samples))},
          errored};
}

runtimeStats timeInstruction(const std::string& setupCode, const std::string& toMeasure) {
  vm::Stack stack = prepare_stack(setupCode);
  return averageRuntime(toMeasure, stack);
}

int main(int argc, char** argv) {
  SET_VERBOSITY_LEVEL(verbosity_ERROR);
  if (argc != 2 && argc != 3) {
    std::cerr << "This utility compares the timing of VM execution against the gas used.\n"
                 "It can be used to discover opcodes or opcode sequences that consume an "
                 "inordinate amount of computational resources relative to their gas cost.\n"
                 "\n"
                 "The utility expects two command line arguments: \n"
                 "The TVM code used to set up the stack and VM state followed by the TVM code to measure.\n"
                 "For example, to test the DIVMODC opcode:\n"
                 "\t$ "
              << argv[0]
              << " 80FF801C A90E 2>/dev/null\n"
                 "\tOPCODE,runtime mean,runtime stddev,gas mean,gas stddev\n"
                 "\tA90E,0.0066416,0.00233496,26,0\n"
                 "\n"
                 "Usage: "
              << argv[0]
              << " [TVM_SETUP_BYTECODE] TVM_BYTECODE\n"
                 "\tBYTECODE is either:\n"
                 "\t1. hex-encoded string (e.g. A90E for DIVMODC)\n"
                 "\t2. boc:<serialized boc in base64> (e.g. boc:te6ccgEBAgEABwABAogBAAJ7)"
              << std::endl
              << std::endl;
    return 1;
  }
  std::cout << "OPCODE,runtime mean,runtime stddev,gas mean,gas stddev,error" << std::endl;
  std::string setup, code;
  if (argc == 2) {
    setup = "";
    code = argv[1];
  } else {
    setup = argv[1];
    code = argv[2];
  }
  vm::init_vm().ensure();
  prepare_c7();
  const auto time = timeInstruction(setup, code);
  std::cout << std::fixed << std::setprecision(9) << code << "," << time.runtime.mean << "," << time.runtime.stddev
            << "," << time.gasUsage.mean << "," << time.gasUsage.stddev << "," << (int)time.errored << std::endl;
  return 0;
}
