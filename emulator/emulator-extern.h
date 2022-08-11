#pragma once

#include "emulator_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates TransactionEmulator object
 * @param shard_account_boc Base64 encoded BoC serialized ShardAccount
 * @param config_params_boc Base64 encoded BoC serialized Config object (Hashmap 32 ^Cell)
 * @return Pointer to TransactionEmulator
 */
EMULATOR_EXPORT void *transaction_emulator_create(const char *shard_account_boc, const char *config_params_boc);

/**
 * @brief Emulate transaction
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param message_boc Base64 encoded BoC serialized inbound Message (internal or external)
 * @return Base64 encoded BoC serialized Transaction object
 */
EMULATOR_EXPORT const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *message_boc);

/**
 * @brief Getter for ShardAccount object
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @return Base64 encodedBoC serialized ShardAccount object
 */
EMULATOR_EXPORT const char *transaction_emulator_get_shard_account(void *transaction_emulator);

/**
 * @brief Getter for Config object. 
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @return Base64 encoded BoC serialized Config object (Hashmap 32 ^Cell)
 */
EMULATOR_EXPORT const char *transaction_emulator_get_config(void *transaction_emulator);

/**
 * @brief Destroy TransactionEmulator object
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @return void 
 */
EMULATOR_EXPORT void transaction_emulator_destroy(void *transaction_emulator);

#ifdef __cplusplus
}  // extern "C"
#endif
