#include "pybind11/pybind11.h"
#include <string>

#include "vm/vm.h"
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "td/db/utils/BlobView.h"
#include "vm/db/StaticBagOfCellsDb.h"
#include "vm/cellslice.h"
#include "vm/cells/MerkleUpdate.h"
#include "vm/cp0.h"
#include "pybind11/stl.h"
#include "pybind11/complex.h"
#include "pybind11/functional.h"
#include "pybind11/chrono.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "block/block-auto.h"
#include "td/utils/crypto.h"
#include "td/utils/base64.h"

namespace py = pybind11;
using namespace pybind11::literals;  // to bring in the `_a` literal

unsigned method_name_to_id(const std::string& method_name) {
  unsigned crc = td::crc16(method_name);
  const unsigned method_id = (crc & 0xffff) | 0x10000;
  return method_id;
}

// this can be slow :(
std::string dump_as_boc(td::Ref<vm::Cell> root_cell) {
  auto s = td::base64_encode(std_boc_serialize(std::move(root_cell), 31).move_as_ok());
  return s;
}
//
//td::Result<vm::StackEntry> from_tonlib_api(tonlib_api::tvm_StackEntry& entry) {
//  // TODO: error codes
//  // downcast_call
//  return downcast_call2<td::Result<vm::StackEntry>>(
//      entry,
//      td::overloaded(
//          [&](tonlib_api::tvm_stackEntryUnsupported& cell) { return td::Status::Error("Unsuppored stack entry"); },
//          [&](tonlib_api::tvm_stackEntrySlice& cell) -> td::Result<vm::StackEntry> {
//            TRY_RESULT(res, vm::std_boc_deserialize(cell.slice_->bytes_));
//            auto slice = vm::load_cell_slice_ref(std::move(res));
//            return vm::StackEntry{std::move(slice)};
//          },
//          [&](tonlib_api::tvm_stackEntryCell& cell) -> td::Result<vm::StackEntry> {
//            TRY_RESULT(res, vm::std_boc_deserialize(cell.cell_->bytes_));
//            return vm::StackEntry{std::move(res)};
//          },
//          [&](tonlib_api::tvm_stackEntryTuple& tuple) -> td::Result<vm::StackEntry> {
//            std::vector<vm::StackEntry> elements;
//            for (auto& element : tuple.tuple_->elements_) {
//              TRY_RESULT(new_element, from_tonlib_api(*element));
//              elements.push_back(std::move(new_element));
//            }
//            return td::Ref<vm::Tuple>(true, std::move(elements));
//          },
//          [&](tonlib_api::tvm_stackEntryList& tuple) -> td::Result<vm::StackEntry> {
//            vm::StackEntry tail;
//            for (auto& element : td::reversed(tuple.list_->elements_)) {
//              TRY_RESULT(new_element, from_tonlib_api(*element));
//              tail = vm::make_tuple_ref(std::move(new_element), std::move(tail));
//            }
//            return tail;
//          },
//          [&](tonlib_api::tvm_stackEntryNumber& number) -> td::Result<vm::StackEntry> {
//            auto& dec = *number.number_;
//            auto num = td::dec_string_to_int256(dec.number_);
//            if (num.is_null()) {
//              return td::Status::Error("Failed to parse dec string to int256");
//            }
//            return num;
//          }));
//}
//

// type converting utils
td::Ref<vm::Cell> parseStringToCell(const std::string& base64string) {
  auto base64decoded = td::base64_decode(td::Slice(base64string));

  if (base64decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid base64");
  }

  auto boc_decoded = vm::std_boc_deserialize(base64decoded.move_as_ok());

  if (boc_decoded.is_error()) {
    throw std::invalid_argument("Parse code error: invalid BOC");
  }

  return boc_decoded.move_as_ok();
}
vm::StackEntry cast_python_item_to_stack_entry(const py::handle item) {
  // TODO: maybe add tuple support?

  py::module builtins = py::module::import("builtins");
  py::object py_int = builtins.attr("int");
  py::object py_str = builtins.attr("str");
  py::object py_dict = builtins.attr("dict");
  py::object py_list = builtins.attr("list");

  if (item.get_type().is(py_int)) {
    std::string asStrValue = item.cast<py::str>().cast<std::string>();
    td::BigIntG<257, td::BigIntInfo> tmp;
    tmp.parse_dec(asStrValue);

    vm::StackEntry tmpEntry;
    tmpEntry.set_int(td::make_refint(tmp));

    return tmpEntry;
  } else if (item.get_type().is(py_list)) {
    std::vector<vm::StackEntry> tmp;

    auto iter = py::iter(item);
    while (iter != py::iterator::sentinel()) {
      auto value = *iter;

      tmp.push_back(cast_python_item_to_stack_entry(value));
      ++iter;
    }

    return tmp;
  } else if (item.get_type().is(py_dict)) {
    std::string tvmType;
    std::string tvmValue;
    vm::StackEntry s;

    auto dict = item.cast<py::dict>();

    for (auto dictItem : dict) {
      auto key = dictItem.first.cast<py::str>().cast<std::string>();
      auto value = dictItem.second.cast<py::str>().cast<std::string>();

      if (key == "type") {
        tvmType = value;
      } else if (key == "value") {
        tvmValue = value;
      } else {
        throw std::invalid_argument("Key should be neither type or value");
      }
    };

    if (tvmType == "cell") {
      auto cell = parseStringToCell(tvmValue);
      s = cell;
    } else if (tvmType == "cellSlice") {
      auto cell = parseStringToCell(tvmValue);
      auto slice = vm::load_cell_slice_ref(cell);
      s = slice;
    }

    return s;
  }

  throw std::invalid_argument("Not supported type: " + item.get_type().cast<py::str>().cast<std::string>());
}

py::object cast_stack_item_to_python_object(const vm::StackEntry& item) {
  if (item.is_null() || item.empty()) {
    return py::none();
  } else if (item.is_int()) {
    std::string bigInteger = item.as_int()->to_dec_string();
    auto pyStringInteger = py::str(bigInteger);
    return pyStringInteger.cast<py::int_>();
  } else if (item.is_cell()) {
    py::dict d("type"_a = "cell", "value"_a = py::str(dump_as_boc(item.as_cell())));
    return d;
  }

  // if slice
  auto slice_item = item.as_slice();

  if (slice_item.not_null()) {
    vm::CellBuilder cb;
    cb.append_cellslice(slice_item);
    auto body_cell = cb.finalize();

    py::dict d("type"_a = "cellSlice", "value"_a = py::str(dump_as_boc(body_cell)));
    return d;
  }

  // if tuple
  auto tuple_item = item.as_tuple();

  if (tuple_item.not_null()) {
    auto tuple_size = tuple_item->size();

    std::vector<py::object> pyStack;
    for (unsigned long idx = 0; idx < tuple_size; idx++) {
      pyStack.push_back(cast_stack_item_to_python_object(tuple_item->at(idx)));
    }

    py::list pyStackList = py::cast(pyStack);

    return pyStackList;
  }

  // if continuation
  auto cont_item = item.as_cont();

  if (cont_item.not_null()) {
    vm::CellBuilder cb;
    cont_item->serialize(cb);
    auto body_cell = cb.finalize();

    py::dict d("type"_a = "continuation", "value"_a = py::str(dump_as_boc(body_cell)));
    return d;
  }

  throw std::invalid_argument("Not supported type: " + std::to_string(item.type()));
}

// Vm logger
class PythonLogger : public td::LogInterface {
 public:
  void append(td::CSlice slice) override {
    py::print(slice.str());
  }
};

const int LOG_DEBUG = 2;
const int LOG_INFO = 1;

struct PyTVM {
  td::Ref<vm::Cell> code;
  td::Ref<vm::Cell> data;

  vm::GasLimits gas_limits;
  std::vector<td::Ref<vm::Cell>> lib_set;
  vm::Stack stackVm;
  bool allowDebug;
  bool sameC3;
  int log_level;

  long long c7_unixtime = 0;
  long long c7_blocklt = 0;
  long long c7_translt = 0;
  long long c7_randseed = 0;
  long long c7_balanceRemainingGrams = 100000;
  std::string c7_myaddress;
  std::string c7_globalConfig;

  int exit_code_out;
  long long vm_steps_out;
  long long gas_used_out;
  long long gas_credit_out;
  bool success_out;
  std::string vm_final_state_hash_out;
  std::string vm_init_state_hash_out;
  std::string new_data_out;
  std::string actions_out;

  void set_c7(int c7_unixtime_, int c7_blocklt_, int c7_translt_, int c7_randseed_, int c7_balanceRemainingGrams_,
              const std::string& c7_myaddress_, const std::string& c7_globalConfig_) {
    c7_unixtime = c7_unixtime_;
    c7_blocklt = c7_blocklt_;
    c7_translt = c7_translt_;
    c7_randseed = c7_randseed_;
    c7_balanceRemainingGrams = c7_balanceRemainingGrams_;
    c7_myaddress = c7_myaddress_;
    c7_globalConfig = c7_globalConfig_;
  }

  // constructor
  explicit PyTVM(int log_level_ = 0, const std::string& code_ = "", const std::string& data_ = "",
                 const bool allowDebug_ = false, const bool sameC3_ = true) {
    allowDebug = allowDebug_;
    sameC3 = sameC3_;

    this->log_level = log_level_;

    if (!code_.empty()) {
      set_code(code_);
    }

    if (!data_.empty()) {
      set_data(data_);
    }
  }

  // log utils
  void log(const std::string& log_string, int level = LOG_INFO) const {
    if (this->log_level >= level && level == LOG_INFO) {
      py::print("INFO: " + log_string);
    } else if (this->log_level >= level && level == LOG_DEBUG) {
      py::print("DEBUG: " + log_string);
    }
  }

  void log_debug(const std::string& log_string) const {
    log(log_string, LOG_DEBUG);
  }
  void log_info(const std::string& log_string) const {
    log(log_string, LOG_INFO);
  }

  void set_gasLimit(int gas_limit, int gas_max = -1) {
    if (gas_max == -1) {
      gas_limits = vm::GasLimits{gas_limit, gas_max};
    } else {
      gas_limits = vm::GasLimits{gas_limit, gas_max};
    }
  }

  // @prop Data
  void set_data(const std::string& data_) {
    log_debug("Start parse data");
    auto data_parsed = parseStringToCell(data_);
    log_debug("Data parsed success");

    data = data_parsed;

    if (log_level >= LOG_DEBUG) {
      std::stringstream log;
      py::print("Data loaded: " + data->get_hash().to_hex());
    }
  }

  std::string get_data() {
    return dump_as_boc(data);
  }

  // @prop Code
  void set_code(const std::string& code_) {
    log_debug("Start parse code");
    auto code_parsed = parseStringToCell(code_);
    log_debug("Code parsed success");

    if (code_parsed.is_null()) {
      throw std::invalid_argument("Code root need to have at least 1 root cell ;)");
    }

    code = code_parsed;

    if (log_level >= LOG_DEBUG) {
      std::stringstream log;
      py::print("Code loaded: " + code->get_hash().to_hex());
    }
  }

  std::string get_code() const {
    return dump_as_boc(code);
  }

  void set_stack(py::object stack) {
    stackVm.clear();

    auto iter = py::iter(std::move(stack));
    while (iter != py::iterator::sentinel()) {
      auto value = *iter;
      py::print("got value: ", value);
      auto parsedStackItem = cast_python_item_to_stack_entry(value);
      stackVm.push(parsedStackItem);
      ++iter;
    }
  }

  void set_libs(py::list cells) {
    lib_set.clear();  // remove old libs

    auto iter = py::iter(std::move(cells));
    while (iter != py::iterator::sentinel()) {
      auto value = *iter;
      auto stack_entry = cast_python_item_to_stack_entry(value);
      if (stack_entry.is_cell()) {
        lib_set.push_back(stack_entry.as_cell());
      } else {
        throw std::invalid_argument("All libs must be cells");
      }
      ++iter;
    }
  }

  void clear_stack() {
    stackVm.clear();
  }

  std::vector<py::object> run_vm() {
    if (code.is_null()) {
      throw std::invalid_argument("To run VM, please pass code");
    }

    auto stack_ = td::make_ref<vm::Stack>();

    std::vector<td::Ref<vm::Cell>> lib_set;

    vm::VmLog vm_log;

    if (log_level >= LOG_DEBUG) {
      vm_log = vm::VmLog();
      vm_log.log_interface = new PythonLogger();
    } else {
      vm_log = vm::VmLog::Null();
    }

    auto balance = block::CurrencyCollection{c7_balanceRemainingGrams};

    td::Ref<vm::CellSlice> my_addr;
    if (!c7_myaddress.empty()) {
      block::StdAddress tmp;
      tmp.parse_addr(c7_myaddress);
      my_addr = block::tlb::MsgAddressInt().pack_std_address(tmp);
    } else {
      vm::CellBuilder cb;
      cb.store_long(0);
      my_addr = vm::load_cell_slice_ref(cb.finalize());
    }

    td::Ref<vm::Cell> global_config;
    if (!c7_globalConfig.empty()) {
      global_config = parseStringToCell(c7_globalConfig);
    }

    auto init_c7 =
        vm::make_tuple_ref(td::make_refint(0x076ef1ea),            // [ magic:0x076ef1ea
                           td::make_refint(0),                     //   actions:Integer
                           td::make_refint(0),                     //   msgs_sent:Integer
                           td::make_refint(c7_unixtime),           //   unixtime:Integer
                           td::make_refint(c7_blocklt),            //   block_lt:Integer
                           td::make_refint(c7_translt),            //   trans_lt:Integer
                           td::make_refint(c7_randseed),           //   rand_seed:Integer
                           balance.as_vm_tuple(),                  //   balance_remaining:[Integer (Maybe Cell)]
                           std::move(my_addr),                     //  myself:MsgAddressInt
                           vm::StackEntry::maybe(global_config));  //  global_config:(Maybe Cell) ] = SmartContractInfo;

    log_debug("Use code: " + code->get_hash().to_hex());

    log_debug("Load cp0");
    vm::init_op_cp0(allowDebug);

    int flags = 0;
    if (sameC3) {
      flags += 1;
    }

    if (log_level > LOG_DEBUG) {
      flags += 4;  // dump stack
    }

    vm::VmState vm_local{code,
                         td::make_ref<vm::Stack>(stackVm),
                         gas_limits,
                         flags,
                         data,
                         vm_log,
                         std::move(lib_set),
                         std::move(init_c7)};

    vm_init_state_hash_out = vm_local.get_state_hash().to_hex();
    exit_code_out = vm_local.run();
    vm_final_state_hash_out = vm_local.get_final_state_hash(exit_code_out).to_hex();
    vm_steps_out = (int)vm_local.get_steps_count();

    auto gas = vm_local.get_gas_limits();
    gas_used_out = std::min<long long>(gas.gas_consumed(), gas.gas_limit);
    gas_credit_out = gas.gas_credit;
    success_out = (gas_credit_out == 0 && vm_local.committed());

    if (success_out) {
      new_data_out = dump_as_boc(vm_local.get_committed_state().c4);
      actions_out = dump_as_boc(vm_local.get_committed_state().c5);
    }

    log_debug("VM terminated with exit code " + std::to_string(exit_code_out));

    std::vector<py::object> pyStack;

    auto stack = vm_local.get_stack();
    for (auto idx = 0; idx < stack.depth(); idx++) {
      log_debug("Parse stack item #" + std::to_string(idx));
      pyStack.push_back(cast_stack_item_to_python_object(stack.at(idx)));
    }

    return pyStack;
  }

  int get_exit_code() const {
    return exit_code_out;
  }

  long long get_vm_steps() const {
    return vm_steps_out;
  }

  long long get_gas_used() const {
    return gas_used_out;
  }

  long long get_gas_credit() const {
    return gas_credit_out;
  }

  bool get_success() const {
    return success_out;
  }

  std::string get_new_data() const {
    return new_data_out;
  }

  std::string get_actions() const {
    return actions_out;
  }

  std::string get_vm_final_state_hash() const {
    return vm_final_state_hash_out;
  }

  std::string get_vm_init_state_hash() const {
    return vm_init_state_hash_out;
  }

  static void dummy_set() {
    throw std::invalid_argument("Not settable");
  }
};

PYBIND11_MODULE(tvm_python, m) {
  static py::exception<vm::VmError> exc(m, "VmError");
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p)
        std::rethrow_exception(p);
    } catch (const vm::VmError& e) {
      exc(e.get_msg());
    } catch (const vm::VmFatal& e) {
      exc("VMFatal error");
    } catch (std::exception& e) {
      PyErr_SetString(PyExc_RuntimeError, e.what());
    }
  });

  m.def("method_name_to_id", &method_name_to_id);

  py::class_<PyTVM>(m, "PyTVM")
      .def(py::init<int, std::string, std::string, bool, bool>(), py::arg("log_level") = 0, py::arg("code") = "",
           py::arg("data") = "", py::arg("allow_debug") = false, py::arg("same_c3") = true)
      .def_property("code", &PyTVM::get_code, &PyTVM::set_code)
      .def_property("data", &PyTVM::set_data, &PyTVM::get_data)
      .def("set_stack", &PyTVM::set_stack)
      .def("set_libs", &PyTVM::set_libs)
      .def("clear_stack", &PyTVM::clear_stack)
      .def("set_gasLimit", &PyTVM::set_gasLimit, py::arg("gas_limit") = 0, py::arg("gas_max") = -1)
      .def("run_vm", &PyTVM::run_vm)
      .def("set_c7", &PyTVM::set_c7, py::arg("unixtime") = 0, py::arg("blocklt") = 0, py::arg("translt") = 0,
           py::arg("randseed") = 0, py::arg("balanceGrams") = 0, py::arg("address") = "", py::arg("globalConfig") = "")

      .def_property("exit_code", &PyTVM::get_exit_code, &PyTVM::dummy_set)
      .def_property("vm_steps", &PyTVM::get_vm_steps, &PyTVM::dummy_set)
      .def_property("gas_used", &PyTVM::get_gas_used, &PyTVM::dummy_set)
      .def_property("gas_credit", &PyTVM::get_gas_credit, &PyTVM::dummy_set)
      .def_property("success", &PyTVM::get_success, &PyTVM::dummy_set)
      .def_property("vm_final_state_hash", &PyTVM::get_vm_final_state_hash, &PyTVM::dummy_set)
      .def_property("vm_init_state_hash", &PyTVM::get_vm_init_state_hash, &PyTVM::dummy_set)
      .def_property("new_data", &PyTVM::get_new_data, &PyTVM::dummy_set)
      .def_property("actions", &PyTVM::get_actions, &PyTVM::dummy_set)

      .def("__repr__", [](const PyTVM& a) { return "tvm_python.PyTVM"; });
}