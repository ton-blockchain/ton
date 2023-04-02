#include "emulator-extern.h"
#include "td/utils/logging.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/misc.h"
#include "td/utils/optional.h"
#include "StringLog.h"
#include <iostream>
#include "crypto/common/bitstring.h"

struct TransactionEmulationParams {
  uint32_t utime;
  uint64_t lt;
  td::optional<std::string> rand_seed_hex;
  bool ignore_chksig;
  bool debug_enabled;
};

td::Result<TransactionEmulationParams> decode_transaction_emulation_params(const char* json) {
  TransactionEmulationParams params;

  std::string json_str(json);
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(json_str)));
  auto &obj = input_json.get_object();

  TRY_RESULT(utime_field, td::get_json_object_field(obj, "utime", td::JsonValue::Type::Number, false));
  TRY_RESULT(utime, td::to_integer_safe<td::uint32>(utime_field.get_number()));
  params.utime = utime;

  TRY_RESULT(lt_field, td::get_json_object_field(obj, "lt", td::JsonValue::Type::String, false));
  TRY_RESULT(lt, td::to_integer_safe<td::uint64>(lt_field.get_string()));
  params.lt = lt;

  TRY_RESULT(rand_seed_str, td::get_json_object_string_field(obj, "rand_seed", true));
  if (rand_seed_str.size() > 0) {
    params.rand_seed_hex = rand_seed_str;
  }

  TRY_RESULT(ignore_chksig, td::get_json_object_bool_field(obj, "ignore_chksig", false));
  params.ignore_chksig = ignore_chksig;

  TRY_RESULT(debug_enabled, td::get_json_object_bool_field(obj, "debug_enabled", false));
  params.debug_enabled = debug_enabled;

  return params;
}

struct GetMethodParams {
  std::string code;
  std::string data;
  int verbosity;
  td::optional<std::string> libs;
  std::string address;
  uint32_t unixtime;
  uint64_t balance;
  std::string rand_seed_hex;
  int64_t gas_limit;
  int method_id;
  bool debug_enabled;
};

td::Result<GetMethodParams> decode_get_method_params(const char* json) {
  GetMethodParams params;

  std::string json_str(json);
  TRY_RESULT(input_json, td::json_decode(td::MutableSlice(json_str)));
  auto &obj = input_json.get_object();

  TRY_RESULT(code, td::get_json_object_string_field(obj, "code", false));
  params.code = code;

  TRY_RESULT(data, td::get_json_object_string_field(obj, "data", false));
  params.data = data;

  TRY_RESULT(verbosity, td::get_json_object_int_field(obj, "verbosity", false));
  params.verbosity = verbosity;

  TRY_RESULT(libs, td::get_json_object_string_field(obj, "libs", true));
  if (libs.size() > 0) {
    params.libs = libs;
  }

  TRY_RESULT(address, td::get_json_object_string_field(obj, "address", false));
  params.address = address;

  TRY_RESULT(unixtime_field, td::get_json_object_field(obj, "unixtime", td::JsonValue::Type::Number, false));
  TRY_RESULT(unixtime, td::to_integer_safe<td::uint32>(unixtime_field.get_number()));
  params.unixtime = unixtime;

  TRY_RESULT(balance_field, td::get_json_object_field(obj, "balance", td::JsonValue::Type::String, false));
  TRY_RESULT(balance, td::to_integer_safe<td::uint64>(balance_field.get_string()));
  params.balance = balance;

  TRY_RESULT(rand_seed_str, td::get_json_object_string_field(obj, "rand_seed", false));
  params.rand_seed_hex = rand_seed_str;

  TRY_RESULT(gas_limit_field, td::get_json_object_field(obj, "gas_limit", td::JsonValue::Type::String, false));
  TRY_RESULT(gas_limit, td::to_integer_safe<td::uint64>(gas_limit_field.get_string()));
  params.gas_limit = gas_limit;

  TRY_RESULT(method_id, td::get_json_object_int_field(obj, "method_id", false));
  params.method_id = method_id;

  TRY_RESULT(debug_enabled, td::get_json_object_bool_field(obj, "debug_enabled", false));
  params.debug_enabled = debug_enabled;

  return params;
}

extern "C" {

const char *emulate(const char *config, const char* libs, int verbosity, const char* account, const char* message, const char* params) {
    StringLog logger;

    td::log_interface = &logger;
    SET_VERBOSITY_LEVEL(verbosity_DEBUG);

    auto decoded_params_res = decode_transaction_emulation_params(params);
    if (decoded_params_res.is_error()) {
        return strdup(R"({"fail":true,"message":"Can't decode other params"})");
    }
    auto decoded_params = decoded_params_res.move_as_ok();

    auto em = transaction_emulator_create(config, verbosity);

    bool rand_seed_set = true;
    if (decoded_params.rand_seed_hex) {
      rand_seed_set = transaction_emulator_set_rand_seed(em, decoded_params.rand_seed_hex.unwrap().c_str());
    }

    if (!transaction_emulator_set_libs(em, libs) ||
        !transaction_emulator_set_lt(em, decoded_params.lt) ||
        !transaction_emulator_set_unixtime(em, decoded_params.utime) ||
        !transaction_emulator_set_ignore_chksig(em, decoded_params.ignore_chksig) ||
        !transaction_emulator_set_debug_enabled(em, decoded_params.debug_enabled) ||
        !rand_seed_set) {
        transaction_emulator_destroy(em);
        return strdup(R"({"fail":true,"message":"Can't set params"})");
    }

    auto tx = transaction_emulator_emulate_transaction(em, account, message);

    transaction_emulator_destroy(em);

    const char* output = nullptr;
    {
        td::JsonBuilder jb;
        auto json_obj = jb.enter_object();
        json_obj("output", td::JsonRaw(td::Slice(tx)));
        json_obj("logs", logger.get_string());
        json_obj.leave();
        output = strdup(jb.string_builder().as_cslice().c_str());
    }
    free((void*) tx);

    return output;
}

const char *run_get_method(const char *params, const char* stack, const char* config) {
    StringLog logger;

    td::log_interface = &logger;
    SET_VERBOSITY_LEVEL(verbosity_DEBUG);

    auto decoded_params_res = decode_get_method_params(params);
    if (decoded_params_res.is_error()) {
        return strdup(R"({"fail":true,"message":"Can't decode params"})");
    }
    auto decoded_params = decoded_params_res.move_as_ok();

    auto tvm = tvm_emulator_create(decoded_params.code.c_str(), decoded_params.data.c_str(), decoded_params.verbosity);

    if ((decoded_params.libs && !tvm_emulator_set_libraries(tvm, decoded_params.libs.value().c_str())) ||
        !tvm_emulator_set_c7(tvm, decoded_params.address.c_str(), decoded_params.unixtime,
          decoded_params.balance, decoded_params.rand_seed_hex.c_str(), config) ||
        (decoded_params.gas_limit > 0 && !tvm_emulator_set_gas_limit(tvm, decoded_params.gas_limit)) ||
        !tvm_emulator_set_debug_enabled(tvm, decoded_params.debug_enabled)) {
      tvm_emulator_destroy(tvm);
      return strdup(R"({"fail":true,"message":"Can't set params"})");
    }

    auto res = tvm_emulator_run_get_method(tvm, decoded_params.method_id, stack);

    tvm_emulator_destroy(tvm);

    const char* output = nullptr;
    {
        td::JsonBuilder jb;
        auto json_obj = jb.enter_object();
        json_obj("output", td::JsonRaw(td::Slice(res)));
        json_obj("logs", logger.get_string());
        json_obj.leave();
        output = strdup(jb.string_builder().as_cslice().c_str());
    }
    free((void*) res);

    return output;
}

}