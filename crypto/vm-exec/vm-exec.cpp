#include "vm/vm.h"
#include "vm/cp0.h"
#include "vm/boc.h"
#include "vm/stack.hpp"
#include "fift/utils.h"
#include "crypto/block/block.h"
#include "td/utils/base64.h"
#include "td/utils/ScopeGuard.h"
#include "terminal/terminal.h"
#include "auto/tl/lite_api.hpp"
#include "td/utils/Random.h"
#include "td/utils/Time.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/OptionParser.h"
#include <fstream>

using namespace std::literals::string_literals;

td::Ref<vm::Tuple> prepare_vm_c7() {
  auto now = static_cast<unsigned int>(td::Time::now());
  td::BitArray<256> rand_seed;
  td::RefInt256 rand_seed_int{true};
  td::Random::secure_bytes(rand_seed.as_slice());
  if (!rand_seed_int.unique_write().import_bits(rand_seed.cbits(), 256, false)) {
    return {};
  }
  auto balance = block::CurrencyCollection{1000, {}}.as_vm_tuple();
  auto tuple = vm::make_tuple_ref(
      td::make_refint(0x076ef1ea),  // [ magic:0x076ef1ea
      td::make_refint(0),           //   actions:Integer
      td::make_refint(0),           //   msgs_sent:Integer
      td::make_refint(now),         //   unixtime:Integer
      td::make_refint(now),         //   block_lt:Integer
      td::make_refint(now),         //   trans_lt:Integer
      std::move(rand_seed_int),     //   rand_seed:Integer
      balance                       //   balance_remaining:[Integer (Maybe Cell)]
                                    // my_addr,                      //  myself:MsgAddressInt
                                    // vm::StackEntry()              //  global_config:(Maybe Cell) ] = SmartContractInfo;
  );
  LOG(DEBUG) << "SmartContractInfo initialized with " << vm::StackEntry(tuple).to_string();
  return vm::make_tuple_ref(std::move(tuple));
}

vm::StackEntry json_to_stack_entry(td::JsonObject& obj) {
  auto type = get_json_object_string_field(obj, "type").move_as_ok();

  if (type == "int") {
    auto data = get_json_object_string_field(obj, "value").move_as_ok();
    return vm::StackEntry(td::dec_string_to_int256(data));
  }
  if (type == "cell") {
    auto data = get_json_object_string_field(obj, "value").move_as_ok();
    auto data_bytes = td::base64_decode(data).move_as_ok();
    auto boc = vm::std_boc_deserialize(data_bytes).move_as_ok();
    auto data_cell = boc->load_cell().move_as_ok();
    return vm::StackEntry(data_cell.data_cell);
  }
  if (type == "cell_slice") {
    auto data = get_json_object_string_field(obj, "value").move_as_ok();
    auto data_bytes = td::base64_decode(data).move_as_ok();
    auto boc = vm::std_boc_deserialize(data_bytes).move_as_ok();
    auto data_cell = boc->load_cell().move_as_ok();
    return vm::StackEntry(td::make_ref<vm::CellSlice>(vm::CellSlice(data_cell)));
  }
  if (type == "null") {
    return vm::StackEntry();
  }
  if (type == "tuple") {
    auto data = td::get_json_object_field(obj, "value", td::JsonValue::Type::Array, false).move_as_ok();
    auto& data_arr = data.get_array();

    std::vector<vm::StackEntry> tuple_components;

    for (auto& x : data_arr) {
      tuple_components.push_back(json_to_stack_entry(x.get_object()));
    }

    return vm::StackEntry(tuple_components);
  }

  return vm::StackEntry();
}

td::Ref<vm::Stack> json_to_stack(td::JsonArray& array) {
  auto stack = td::make_ref<vm::Stack>();

  for (auto& x : array) {
    auto& obj = x.get_object();
    auto entry = json_to_stack_entry(obj);
    stack.write().push(entry);
  }

  return stack;
}

std::string stack_entry_to_json(vm::StackEntry se) {
  auto out = td::TerminalIO::out();

  if (se.is_int()) {
    return R"({ "type": "int", "value": ")" + td::dec_string(se.as_int()) + R"("})";
  }
  if (se.is_cell()) {
    auto value = td::base64_encode(vm::std_boc_serialize(se.as_cell()).move_as_ok());
    return R"({ "type": "cell", "value": ")" + value + R"("})";
  }
  if (se.type() == vm::StackEntry::Type::t_slice) {
    vm::CellBuilder b;
    b.append_cellslice(se.as_slice());
    auto value = td::base64_encode(vm::std_boc_serialize(b.finalize()).move_as_ok());
    //    auto value = td::base64_encode(vm::std_boc_serialize(se.as_slice()->get_base_cell()).move_as_ok());
    return R"({ "type": "cell_slice", "value": ")" + value + R"("})";
  }
  if (se.is_null()) {
    return R"({ "type": "null" })";
  }
  if (se.is_tuple()) {
    std::string res = R"({ "type": "tuple", "value": [)";

    auto tuple = se.as_tuple();

    for (auto& x : *tuple) {
      res.append(stack_entry_to_json(x));
      res.append(",");
    }
    res = res.substr(0, res.length() - 1);
    res += "] }";

    return res;
  }

  //
  // Non supported by TVM
  //
  if (se.type() == vm::StackEntry::Type::t_builder) {
    return se.as_builder()->to_hex();
  }
  if (se.type() == vm::StackEntry::Type::t_vmcont) {
    return R"({ "type": "t_vmcont" })";
  }
  if (se.type() == vm::StackEntry::Type::t_string) {
    return R"({ "type": "string", value: ")" + se.as_string() + R"("})";
  }
  if (se.type() == vm::StackEntry::Type::t_bytes) {
    return R"({ "type": "bytes", value: ")" + td::base64_encode(se.as_bytes()) + R"("})";
  }
  if (se.type() == vm::StackEntry::Type::t_bitstring) {
    return R"({ "type": "bitstring", value: ")" + td::base64_encode(se.as_bytes()) + R"("})";
  }

  return R"({ "type": "unknown" })";
}

std::string stack2json(vm::Ref<vm::Stack> stack) {
  if (stack->is_empty()) {
    return "[]";
  }
  std::string out = "[";
  for (const auto& x : stack->as_span()) {
    out.append(stack_entry_to_json(x));
    out.append(",");
  }
  out = out.substr(0, out.length() - 1);
  out.append("]");
  return out;
}

std::string run_vm(td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data, td::JsonArray& stack_array, int function_selector) {
  auto out = td::TerminalIO::out();
  auto code = code_cell;

  auto stack = json_to_stack(stack_array);

  td::int64 method_id = function_selector;
  stack.write().push_smallint(method_id);

  long long gas_limit = vm::GasLimits::infty;
  vm::GasLimits gas{gas_limit};
  LOG(DEBUG) << "creating VM";
  vm::VmState vm{code, std::move(stack), gas, 1, data, vm::VmLog()};
  vm.set_c7(prepare_vm_c7());  // tuple with SmartContractInfo
  LOG(INFO) << "starting VM to run method `" << 0 << "` (" << 0 << ") of smart contract " << 0;

  int exit_code;
  try {
    exit_code = ~vm.run();
  } catch (vm::VmVirtError& err) {
    LOG(ERROR) << "virtualization error while running VM to locally compute runSmcMethod result: " << err.get_msg();
  } catch (vm::VmError& err) {
    LOG(ERROR) << "error while running VM to locally compute runSmcMethod result: " << err.get_msg();
  }
  stack = vm.get_stack_ref();

  std::string result;

  auto committed_state = vm.get_committed_state();

  auto serialized_data_cell = td::base64_encode(vm::std_boc_serialize(committed_state.c4).move_as_ok());
  auto serialized_action_list_cell = td::base64_encode(vm::std_boc_serialize(committed_state.c5).move_as_ok());
  auto new_code_cell = td::base64_encode(vm::std_boc_serialize(vm.get_code()->get_base_cell()).move_as_ok());

  result += "{";
  result += R"("exit_code":)" + std::to_string(exit_code) + ",";
  result += R"("stack":)" + stack2json(stack) + ",";
  result += R"("data_cell": ")" + serialized_data_cell + R"(",)";
  result += R"("action_list_cell": ")" + serialized_action_list_cell + R"(")";
  result += "}";

  return result;
}

void execute(td::Slice config_file_name) {
  auto out = td::TerminalIO::out();

  std::string input_file = config_file_name.str();
  auto input_data = td::read_file(input_file).move_as_ok().as_slice().str();

  auto input_json = td::json_decode(input_data).move_as_ok();
  auto& obj = input_json.get_object();

  auto code = td::get_json_object_string_field(obj, "code", false).move_as_ok();
  auto data = td::get_json_object_string_field(obj, "data", false).move_as_ok();
  auto function_selector = td::get_json_object_int_field(obj, "function_selector", false).move_as_ok();
  auto initial_stack = td::get_json_object_field(obj, "init_stack", td::JsonValue::Type::Array, false).move_as_ok();

  auto& initial_stack_array = initial_stack.get_array();

  auto decoded_code = td::base64_decode(code).move_as_ok();

  auto data_bytes = td::base64_decode(data).move_as_ok();
  auto boc = vm::std_boc_deserialize(data_bytes).move_as_ok();
  auto data_cell = boc->load_cell().move_as_ok();

  auto code_s = td::Slice(decoded_code);
  auto compiled_source = fift::compile_asm(code_s, "", false).move_as_ok();
  auto res = run_vm(compiled_source, data_cell.data_cell, initial_stack_array, function_selector);
  out << res;
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_FATAL);

  td::OptionParser p;

  p.set_description("TVM JSON Executor");
  p.add_option('h', "help", "prints_help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('c', "config", "path to config", [&](td::Slice fname) { execute(fname); });
  p.run(argc, argv).ensure();

  return 0;
}
