#pragma once

#include "emulator_export.h"

#ifdef __cplusplus
extern "C" {
#endif

EMULATOR_EXPORT void *transaction_emulator_create(const char *shard_account_boc, const char *config_params_boc);

EMULATOR_EXPORT const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *message_boc);

EMULATOR_EXPORT const char *transaction_emulator_get_shard_account(void *transaction_emulator);

EMULATOR_EXPORT const char *transaction_emulator_get_config(void *transaction_emulator);

EMULATOR_EXPORT void transaction_emulator_destroy(void *transaction_emulator);

#ifdef __cplusplus
}  // extern "C"
#endif
