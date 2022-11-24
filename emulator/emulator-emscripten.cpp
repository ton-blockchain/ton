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

  return params;
}

extern "C" {

const char *emulate(const char *config, const char* libs, int verbosity, const char* account, const char* message, const char* params) {
    StringLog logger;

    td::log_interface = &logger;
    SET_VERBOSITY_LEVEL(verbosity_DEBUG);

    const char* output = nullptr;

    auto decoded_params_res = decode_transaction_emulation_params(params);
    if (decoded_params_res.is_error()) {
        output = strdup(R"({"fail":true,"message":"Can't decode other params"})");
        return output;
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
        !rand_seed_set) {
        transaction_emulator_destroy(em);
        output = strdup(R"({"fail":true,"message":"Can't set params"})");
        return output;
    }

    auto tx = transaction_emulator_emulate_transaction(em, account, message);

    transaction_emulator_destroy(em);

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

}