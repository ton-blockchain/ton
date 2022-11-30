#include <ctime>
#include <iomanip>

#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/dict.h"
#include "fift/utils.h"
#include "common/bigint.hpp"

#include "td/utils/base64.h"
#include "td/utils/tests.h"
#include "td/utils/ScopeGuard.h"
#include "td/utils/StringBuilder.h"

td::Ref<vm::Cell> to_cell(const unsigned char *buff, int bits) {
  return vm::CellBuilder().store_bits(buff, bits, 0).finalize();
}

long double timingBaseline;

typedef struct {
  long double mean;
  long double stddev;
} stats;

struct runInfo {
  long double runtime;
  long long gasUsage;
  int vmReturnCode;

  runInfo() : runtime(0.0), gasUsage(0), vmReturnCode(0) {}
  runInfo(long double runtime, long long gasUsage, int vmReturnCode) :
      runtime(runtime), gasUsage(gasUsage), vmReturnCode(vmReturnCode) {}

  runInfo operator+(const runInfo& addend) const {
    return {runtime + addend.runtime, gasUsage + addend.gasUsage, vmReturnCode ? vmReturnCode : addend.vmReturnCode};
  }

  runInfo& operator+=(const runInfo& addend) {
    runtime += addend.runtime;
    gasUsage += addend.gasUsage;
    if(!vmReturnCode && addend.vmReturnCode) {
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

runInfo time_run_vm(td::Slice command) {
  unsigned char buff[128];
  const int bits = (int)td::bitstring::parse_bitstring_hex_literal(buff, sizeof(buff), command.begin(), command.end());
  CHECK(bits >= 0);

  const auto cell = to_cell(buff, bits);

  vm::init_op_cp0();
  vm::DictionaryBase::get_empty_dictionary();

  class Logger : public td::LogInterface {
   public:
    void append(td::CSlice slice) override {
      res.append(slice.data(), slice.size());
    }
    std::string res;
  };
  static Logger logger;
  logger.res = "";
  td::set_log_fatal_error_callback([](td::CSlice message) { td::default_log_interface->append(logger.res); });
  vm::VmLog log{&logger, td::LogOptions::plain()};
  log.log_options.level = 4;
  log.log_options.fix_newlines = true;
  log.log_mask |= vm::VmLog::DumpStack;

  vm::Stack stack;
  try {
    vm::GasLimits gas_limit(10000, 10000);

    std::clock_t cStart = std::clock();
    int ret = vm::run_vm_code(vm::load_cell_slice_ref(cell), stack, 0 /*flags*/, nullptr /*data*/,
                              std::move(log) /*VmLog*/, nullptr, &gas_limit);
    std::clock_t cEnd = std::clock();
    const auto time = (1000.0 * static_cast<long double>(cEnd - cStart) / CLOCKS_PER_SEC) - timingBaseline;
    return {time >= 0 ? time : 0, gas_limit.gas_consumed(), ret};
  } catch (...) {
    LOG(FATAL) << "catch unhandled exception";
    return {-1, -1, 1};
  }
}

runtimeStats averageRuntime(td::Slice command) {
  const size_t samples = 5000;
  runInfo total;
  std::vector<runInfo> values;
  values.reserve(samples);
  for(size_t i=0; i<samples; ++i) {
    const auto value = time_run_vm(command);
    values.push_back(value);
    total += value;
  }
  const auto runtimeMean = total.runtime / static_cast<long double>(samples);
  const auto gasMean = static_cast<long double>(total.gasUsage) / static_cast<long double>(samples);
  long double runtimeDiffSum = 0.0;
  long double gasDiffSum = 0.0;
  bool errored = false;
  for(const auto value : values) {
    const auto runtime = value.runtime - runtimeMean;
    const auto gasUsage = static_cast<long double>(value.gasUsage) - gasMean;
    runtimeDiffSum += runtime * runtime;
    gasDiffSum += gasUsage * gasUsage;
    errored = errored || value.errored();
  }
  return {
      {runtimeMean, sqrt(runtimeDiffSum / static_cast<long double>(samples))},
      {gasMean, sqrt(gasDiffSum / static_cast<long double>(samples))},
      errored
  };
}

runtimeStats timeInstruction(const std::string& setupCode, const std::string& toMeasure) {
  const auto setupCodeTime = averageRuntime(setupCode);
  const auto totalCodeTime = averageRuntime(setupCode + toMeasure);
  return {
      {totalCodeTime.runtime.mean - setupCodeTime.runtime.mean, totalCodeTime.runtime.stddev},
      {totalCodeTime.gasUsage.mean - setupCodeTime.gasUsage.mean, totalCodeTime.gasUsage.stddev},
      false
  };
}

int main(int argc, char** argv) {
  if(argc != 2 && argc != 3) {
    std::cerr <<
        "This utility compares the timing of VM execution against the gas used.\n"
        "It can be used to discover opcodes or opcode sequences that consume an "
        "inordinate amount of computational resources relative to their gas cost.\n"
        "\n"
        "The utility expects two command line arguments, each a hex string: \n"
        "The TVM code used to set up the stack and VM state followed by the TVM code to measure.\n"
        "For example, to test the DIVMODC opcode:\n"
        "\t$ " << argv[0] << " 80FF801C A90E 2>/dev/null\n"
        "\tOPCODE,runtime mean,runtime stddev,gas mean,gas stddev\n"
        "\tA90E,0.0066416,0.00233496,26,0\n"
        "\n"
        "Usage: " << argv[0] <<
        " [TVM_SETUP_BYTECODE_HEX] TVM_BYTECODE_HEX" << std::endl << std::endl;
    return 1;
  }
  std::cout << "OPCODE,runtime mean,runtime stddev,gas mean,gas stddev" << std::endl;
  timingBaseline = averageRuntime("").runtime.mean;
  std::string setup, code;
  if(argc == 2) {
    setup = "";
    code = argv[1];
  } else {
    setup = argv[1];
    code = argv[2];
  }
  const auto time = timeInstruction(setup, code);
  std::cout << code << "," << time.runtime.mean << "," << time.runtime.stddev << "," <<
      time.gasUsage.mean << "," << time.gasUsage.stddev << std::endl;
  return 0;
}
