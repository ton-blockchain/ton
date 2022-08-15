#pragma once

#include "emulator_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates TransactionEmulator object
 * @param config_params_boc Base64 encoded BoC serialized Config dictionary (Hashmap 32 ^Cell)
 * @param shardchain_libs_boc Base64 encoded BoC serialized shardchain libraries dictionary (HashmapE 256 ^Cell). Can be NULL if no shardchain libraries needed.
 * @return Pointer to TransactionEmulator or nullptr in case of error
 */
EMULATOR_EXPORT void *transaction_emulator_create(const char *config_params_boc, const char *shardchain_libs_boc);

/**
 * @brief Emulate transaction
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param shard_account_boc Base64 encoded BoC serialized ShardAccount
 * @param message_boc Base64 encoded BoC serialized inbound Message (internal or external)
 * @return Json object with error:
 * { "success": false, "error": "Error description" }
 * or success:
 * { "success": true, "transaction": "Base64 encoded Transaction boc", "shard_account": "Base64 encoded ShardAccount boc" }
 */
EMULATOR_EXPORT const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *shard_account_boc, const char *message_boc);

/**
 * @brief Destroy TransactionEmulator object
 * @param transaction_emulator Pointer to TransactionEmulator object
 */
EMULATOR_EXPORT void transaction_emulator_destroy(void *transaction_emulator);

#ifdef __cplusplus
}  // extern "C"
#endif
