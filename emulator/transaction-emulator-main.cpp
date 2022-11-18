#include "emulator-extern.h"
#include "td/utils/logging.h"
#include "td/utils/JsonBuilder.h"
#include "StringLog.h"
#include <iostream>

extern "C" {

const char *emulate(const char *config, const char* libs, int verbosity, const char* account, const char* message, const char* params) {
    StringLog logger;

    td::log_interface = &logger;
    SET_VERBOSITY_LEVEL(verbosity_DEBUG);
    logger.clear();

    auto em = transaction_emulator_create(config, libs, verbosity);

    auto tx = transaction_emulator_emulate_transaction(em, account, message, params);

    transaction_emulator_destroy(em);

    const char* output = nullptr;

    {
        td::JsonBuilder jb;
        auto json_obj = jb.enter_object();
        json_obj("output", tx);
        json_obj("logs", logger.get_string());
        json_obj.leave();
        output = strdup(jb.string_builder().as_cslice().c_str());
    }
    free((void*) tx);

    return output;
}

}