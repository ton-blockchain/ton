#include "third-party/pybind11/include/pybind11/pybind11.h"
#include <string>
#include <cassert>
#include <codecvt>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>

#include "vm/vm.h"
#include "vm/vmstate.h"
#include "td/utils/base64.h"
#include <utility>
#include "vm/boc.h"
#include "vm/cellslice.h"
#include "vm/cp0.h"
#include "third-party/pybind11/include/pybind11/stl.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "td/utils/crypto.h"
#include "crypto/fift/Fift.h"
#include "crypto/fift/IntCtx.h"
#include "crypto/fift/words.h"
#include "td/utils/filesystem.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "crypto/common/refint.h"
#include "vm/dumper.hpp"

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
  } else if (item.get_type().is(py_str)) {
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

  // if builder
  auto builder_item = item.as_builder();

  if (builder_item.not_null()) {
    vm::CellBuilder cb;
    cb.append_builder(builder_item);
    auto body_cell = cb.finalize();

    py::dict d("type"_a = "builder", "value"_a = py::str(dump_as_boc(body_cell)));
    return d;
  }

  py::dict d("type"_a = "not supported");
  return d;
}

// Vm logger
class PythonLogger : public td::LogInterface {
 public:
  bool muted = false;
  vm::VmDumper* vm_dumper{0};

  void set_vm_dumper(vm::VmDumper* vm_dumper_) {
    vm_dumper = vm_dumper_;
  }

  void mute() {
    muted = true;
  }

  void append(td::CSlice slice) override {
    if (vm_dumper->enable) {
      if (slice.str().find("execute") != std::string::npos) {
        vm_dumper->dump_op(slice.str());
      }
    }

    if (!muted) {
      py::print(slice.str());
    }
  }
};

const int LOG_DEBUG = 2;
const int LOG_INFO = 1;

std::string code_disasseble(const std::string& code) {
  auto codeCell = parseStringToCell(code);

  fift::Fift::Config config;

  config.source_lookup = fift::SourceLookup(std::make_unique<fift::OsFileLoader>());
  config.source_lookup.add_include_path("./lib");

  fift::init_words_common(config.dictionary);
  fift::init_words_vm(config.dictionary);
  fift::init_words_ton(config.dictionary);

  fift::Fift fift(std::move(config));

  std::stringstream ss;
  std::stringstream output;

  // TODO: add custom path to lib dir
  const auto basePath = td::PathView(td::realpath(__FILE__).move_as_ok()).parent_dir().str() + "../crypto/fift/lib/";
  const auto fiftLib = td::read_file_str(basePath + "Fift.fif");
  const auto listsLib = td::read_file_str(basePath + "Lists.fif");
  const auto disasmLib = td::read_file_str(basePath + "Disasm.fif");

  // Fift.fif & Lists.fif & Disasm.fif
  ss << fiftLib.ok();
  ss << listsLib.ok();
  ss << disasmLib.ok();
  ss << "<s std-disasm disasm ";

  fift::IntCtx ctx{ss, "stdin", "./", 0};
  ctx.stack.push_cell(codeCell);

  ctx.ton_db = &fift.config().ton_db;
  ctx.source_lookup = &fift.config().source_lookup;
  ctx.dictionary = ctx.context = ctx.main_dictionary = fift.config().dictionary;
  ctx.output_stream = &output;
  ctx.error_stream = fift.config().error_stream;

  try {
    auto res = ctx.run(td::make_ref<fift::InterpretCont>());
    if (res.is_error()) {
      std::cerr << "Disasm error: " << res.move_as_error().to_string();
      throw std::invalid_argument("Error in disassembler");
    } else {
      auto disasm_out = output.str();
      // cheap no-brainer
      std::string_view pattern = " ok\n";
      std::string::size_type n = pattern.length();
      for (std::string::size_type i = disasm_out.find(pattern); i != std::string::npos; i = disasm_out.find(pattern)) {
        disasm_out.erase(i, n);
      }

      return disasm_out;
    }
  } catch (const std::exception& e) {
    std::cerr << "Disasm error: " << e.what();
    throw std::invalid_argument("Error in disassembler");
  }
}

struct PyTVM {
  td::Ref<vm::Cell> code;
  td::Ref<vm::Cell> data;

  vm::GasLimits gas_limits;
  std::vector<td::Ref<vm::Cell>> lib_set;
  vm::Stack stackVm;
  bool allowDebug;
  bool sameC3;
  int log_level;
  bool skip_c7 = false;

  long long c7_unixtime = 0;
  td::RefInt256 c7_blocklt = td::make_refint(0);
  td::RefInt256 c7_translt = td::make_refint(0);
  td::RefInt256 c7_randseed = td::make_refint(0);
  td::RefInt256 c7_balanceRemainingGrams = td::make_refint(101000000000);
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

  std::vector<std::vector<vm::StackEntry>> stacks;
  std::vector<std::string> vm_ops;

  void set_c7(int c7_unixtime_, const std::string& c7_blocklt_, const std::string& c7_translt_,
              const std::string& c7_randseed_, const std::string& c7_balanceRemainingGrams_,
              const std::string& c7_myaddress_, const std::string& c7_globalConfig_) {
    if (!skip_c7) {
      c7_unixtime = c7_unixtime_;
      c7_blocklt = td::dec_string_to_int256(c7_blocklt_);
      c7_translt = td::dec_string_to_int256(c7_translt_);
      c7_randseed = td::dec_string_to_int256(c7_randseed_);
      c7_balanceRemainingGrams = td::dec_string_to_int256(c7_balanceRemainingGrams_);
      c7_myaddress = c7_myaddress_;
      c7_globalConfig = c7_globalConfig_;
    } else {
      throw std::invalid_argument("C7 will be skipped, because skip_c7=true");
    }
  }

  // constructor
  explicit PyTVM(int log_level_ = 0, const std::string& code_ = "", const std::string& data_ = "",
                 const bool allowDebug_ = false, const bool sameC3_ = true, const bool skip_c7_ = false) {
    allowDebug = allowDebug_;
    sameC3 = sameC3_;
    skip_c7 = skip_c7_;

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
    vm::VmDumper vm_dumper{true, &stacks, &vm_ops};

    vm_log = vm::VmLog();

    auto pyLogger = new PythonLogger();
    pyLogger->set_vm_dumper(&vm_dumper);

    if (log_level < LOG_DEBUG) {
      pyLogger->mute();
    }

    vm_log.log_interface = pyLogger;

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

    td::Ref<vm::Tuple> init_c7;

    if (!skip_c7) {
      init_c7 = vm::make_tuple_ref(
          td::make_refint(0x076ef1ea),              // [ magic:0x076ef1ea
          td::make_refint(0),                       //   actions:Integer
          td::make_refint(0),                       //   msgs_sent:Integer
          td::make_refint(c7_unixtime),             //   unixtime:Integer
          td::make_refint(c7_blocklt->to_long()),   //   block_lt:Integer
          td::make_refint(c7_translt->to_long()),   //   trans_lt:Integer
          td::make_refint(c7_randseed->to_long()),  //   rand_seed:Integer
          balance.as_vm_tuple(),                    //   balance_remaining:[Integer (Maybe Cell)]
          std::move(my_addr),                       //  myself:MsgAddressInt
          vm::StackEntry::maybe(global_config));    //  global_config:(Maybe Cell) ] = SmartContractInfo;
    } else {
      init_c7 = vm::make_tuple_ref();
    }

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
                         &vm_dumper,
                         gas_limits,
                         flags,
                         data,
                         vm_log,
                         std::move(lib_set),
                         vm::make_tuple_ref(std::move(init_c7))};

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
    for (auto idx = stack.depth() - 1; idx >= 0; idx--) {
      log_debug("Parse stack item #" + std::to_string(idx));
      pyStack.push_back(cast_stack_item_to_python_object(stack.at(idx)));
    }

    return pyStack;
  }

  std::vector<std::string> get_ops() const {
    return vm_ops;
  }

  std::vector<std::vector<py::object>> get_stacks() const {
    std::vector<std::vector<py::object>> AllPyStack;

    for (const auto& stack : stacks) {
      std::vector<py::object> pyStack;
      for (const auto& stackEntry : stack) {
        pyStack.push_back(cast_stack_item_to_python_object(stackEntry));
      }

      AllPyStack.push_back(pyStack);
    }

    return AllPyStack;
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

py::object pack_address(const std::string& address) {
  auto paddr_parse = block::StdAddress::parse(address);

  if (paddr_parse.is_ok()) {
    auto paddr = paddr_parse.move_as_ok();
    td::BigInt256 dest_addr;
    vm::CellBuilder cb;

    dest_addr.import_bits(paddr.addr.as_bitslice());
    cb.store_ones(1).store_zeroes(2).store_long(paddr.workchain, 8).store_int256(dest_addr, 256);
    auto body_cell = cb.finalize();

    py::dict d("type"_a = "cellSlice", "value"_a = py::str(dump_as_boc(body_cell)));
    return d;
  } else {
    throw std::invalid_argument("Parse address error: not valid address");
  }
}

// todo: make cell & cell slice bindings
std::string load_address(const std::string& boc) {
  auto cell = parseStringToCell(boc);
  auto cs = load_cell_slice(cell);
  ton::StdSmcAddress addr;
  ton::WorkchainId workchain;
  if (!block::tlb::t_MsgAddressInt.extract_std_address(cs, workchain, addr)) {
    throw std::invalid_argument("Parse address error: not valid address");
  }
  auto friendlyAddr = block::StdAddress(workchain, addr);

  return friendlyAddr.rserialize(true);
}

std::string onchain_hash_key_to_string(const std::string& hash) {
  td::Bits256 uri;
  td::Bits256 name;
  td::Bits256 description;
  td::Bits256 image;
  td::Bits256 image_data;
  td::Bits256 symbol;
  td::Bits256 decimals;
  td::Bits256 amount_style;
  td::Bits256 render_type;
  td::Bits256 jetton;
  td::Bits256 master;
  td::Bits256 address;

  td::sha256(td::Slice("uri", strlen("uri")), uri.as_slice());
  td::sha256(td::Slice("name", strlen("name")), name.as_slice());
  td::sha256(td::Slice("description", strlen("description")), description.as_slice());
  td::sha256(td::Slice("image", strlen("image")), image.as_slice());
  td::sha256(td::Slice("image_data", strlen("image_data")), image_data.as_slice());
  td::sha256(td::Slice("symbol", strlen("symbol")), symbol.as_slice());
  td::sha256(td::Slice("decimals", strlen("decimals")), decimals.as_slice());
  td::sha256(td::Slice("amount_style", strlen("amount_style")), amount_style.as_slice());
  td::sha256(td::Slice("render_type", strlen("render_type")), render_type.as_slice());
  td::sha256(td::Slice("jetton", strlen("jetton")), jetton.as_slice());
  td::sha256(td::Slice("master", strlen("master")), master.as_slice());
  td::sha256(td::Slice("address", strlen("address")), address.as_slice());

  if (hash == uri.to_hex()) {
    return "uri";
  } else if (hash == name.to_hex()) {
    return "name";
  } else if (hash == description.to_hex()) {
    return "description";
  } else if (hash == image.to_hex()) {
    return "image";
  } else if (hash == image_data.to_hex()) {
    return "image_data";
  } else if (hash == symbol.to_hex()) {
    return "symbol";
  } else if (hash == decimals.to_hex()) {
    return "decimals";
  } else if (hash == amount_style.to_hex()) {
    return "amount_style";
  } else if (hash == render_type.to_hex()) {
    return "render_type";
  } else {
    return hash;
  }
}

std::string map_to_utf8(const long long val) {
  std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
  return converter.to_bytes(static_cast<char32_t>(val));
}

std::string parse_snake_data_string(vm::CellSlice& cs) {
  auto text_size = cs.size() / 8;
  bool has_next_ref = cs.have_refs();

  std::string text;

  while (text_size > 0) {
    auto text_tmp = map_to_utf8(cs.fetch_long(8));
    text += text_tmp;
    text_size -= 1;
  }

  auto rcf = cs;

  while (has_next_ref) {
    rcf = load_cell_slice(rcf.prefetch_ref());
    auto rtext_size = rcf.size() / 8;

    while (rtext_size > 0) {
      auto text_tmp = map_to_utf8(rcf.fetch_long(8));
      text += text_tmp;
      text_size -= 1;
    }

    has_next_ref = rcf.have_refs();
  }

  return text;
}

py::dict parse_token_data(const std::string& boc) {
  auto cell = parseStringToCell(boc);
  auto cs = load_cell_slice(cell);

  int content_type;
  cs.fetch_uint_to(8, content_type);

  if (content_type == 0) {
    auto data = cs.fetch_ref();

    vm::Dictionary data_dict{data, 256};
    py::dict py_dict;

    while (!data_dict.is_empty()) {
      td::BitArray<256> key{};
      data_dict.get_minmax_key(key);
      auto key_text = onchain_hash_key_to_string(key.to_hex());

      td::Ref<vm::Cell> value = data_dict.lookup_delete_ref(key);
      if (value.not_null()) {
        std::stringstream a;
        auto vs = load_cell_slice(value);

        int value_type;
        vs.fetch_uint_to(8, value_type);

        if (value_type == 0) {
          py::dict d("type"_a = "snake", "value"_a = parse_snake_data_string(vs));
          py_dict[py::str(key_text)] = d;
        } else if (value_type == 1) {
          py::dict d("type"_a = "chunks", "value"_a = "");
          py_dict[py::str(key_text)] = d;
        } else {
          py::dict d("type"_a = "unknown", "value"_a = "");
          py_dict[py::str(key_text)] = d;
        }
      }
    };

    py::dict d("type"_a = "onchain", "value"_a = py_dict);
    return d;
  } else if (content_type == 1) {
    py::dict d("type"_a = "offchain", "value"_a = parse_snake_data_string(cs));
    return d;
  } else {
    throw std::invalid_argument("Not valid prefix, must be 0x00 / 0x01");
  }
}

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
  m.def("code_disasseble", &code_disasseble);
  m.def("pack_address", &pack_address);
  m.def("load_address", &load_address);
  m.def("parse_token_data", &parse_token_data);

  py::class_<PyTVM>(m, "PyTVM")
      .def(py::init<int, std::string, std::string, bool, bool, bool>(), py::arg("log_level") = 0, py::arg("code") = "",
           py::arg("data") = "", py::arg("allow_debug") = false, py::arg("same_c3") = true, py::arg("skip_c7") = false)
      .def_property("code", &PyTVM::get_code, &PyTVM::set_code)
      .def_property("data", &PyTVM::set_data, &PyTVM::get_data)
      .def("set_stack", &PyTVM::set_stack)
      .def("set_libs", &PyTVM::set_libs)
      .def("get_ops", &PyTVM::get_ops)
      .def("get_stacks", &PyTVM::get_stacks)
      .def("clear_stack", &PyTVM::clear_stack)
      .def("set_gasLimit", &PyTVM::set_gasLimit, py::arg("gas_limit") = 0, py::arg("gas_max") = -1)
      .def("run_vm", &PyTVM::run_vm)
      .def("set_c7", &PyTVM::set_c7, py::arg("unixtime") = 0, py::arg("blocklt") = "0", py::arg("translt") = "0",
           py::arg("randseed") = "", py::arg("balanceGrams") = "", py::arg("address") = "",
           py::arg("globalConfig") = "")

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