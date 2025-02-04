#pragma once

#include <stdint.h>
#include "emulator_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates TransactionEmulator object
 * @param config_params_boc Base64 encoded BoC serialized Config dictionary (Hashmap 32 ^Cell)
 * @param vm_log_verbosity Verbosity level of VM log. 0 - log truncated to last 256 characters. 1 - unlimited length log.
 * 2 - for each command prints its cell hash and offset. 3 - for each command log prints all stack values.
 * @return Pointer to TransactionEmulator or nullptr in case of error
 */
EMULATOR_EXPORT void *transaction_emulator_create(const char *config_params_boc, int vm_log_verbosity);

/**
 * @brief Creates Config object from base64 encoded BoC
 * @param config_params_boc Base64 encoded BoC serialized Config dictionary (Hashmap 32 ^Cell)
 * @return Pointer to Config object or nullptr in case of error
 */
EMULATOR_EXPORT void *emulator_config_create(const char *config_params_boc);

/**
 * @brief Set unixtime for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param unixtime Unix timestamp
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_unixtime(void *transaction_emulator, uint32_t unixtime);

/**
 * @brief Set lt for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param lt Logical time
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_lt(void *transaction_emulator, uint64_t lt);

/**
 * @brief Set rand seed for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param rand_seed_hex Hex string of length 64
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_rand_seed(void *transaction_emulator, const char* rand_seed_hex);

/**
 * @brief Set ignore_chksig flag for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param ignore_chksig Whether emulation should always succeed on CHKSIG operation
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_ignore_chksig(void *transaction_emulator, bool ignore_chksig);

/**
 * @brief Set config for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param config_boc Base64 encoded BoC serialized Config dictionary (Hashmap 32 ^Cell) 
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_config(void *transaction_emulator, const char* config_boc);

/**
 * @brief Set config for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param config Pointer to Config object
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_config_object(void *transaction_emulator, void* config);

/**
 * @brief Set libraries for emulation
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param libs_boc Base64 encoded BoC serialized shared libraries dictionary (HashmapE 256 ^Cell).
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_libs(void *transaction_emulator, const char* libs_boc);

/**
 * @brief Enable or disable TVM debug primitives
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param debug_enabled Whether debug primitives should be enabled or not
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_debug_enabled(void *transaction_emulator, bool debug_enabled);

/**
 * @brief Set tuple of previous blocks (13th element of c7)
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param info_boc Base64 encoded BoC serialized TVM tuple (VmStackValue).
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool transaction_emulator_set_prev_blocks_info(void *transaction_emulator, const char* info_boc);

/**
 * @brief Emulate transaction
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param shard_account_boc Base64 encoded BoC serialized ShardAccount
 * @param message_boc Base64 encoded BoC serialized inbound Message (internal or external)
 * @return Json object with error:
 * { 
 *   "success": false, 
 *   "error": "Error description",
 *   "external_not_accepted": false,
 *   // and optional fields "vm_exit_code", "vm_log", "elapsed_time" in case external message was not accepted.
 * } 
 * Or success:
 * { 
 *   "success": true, 
 *   "transaction": "Base64 encoded Transaction boc", 
 *   "shard_account": "Base64 encoded new ShardAccount boc", 
 *   "vm_log": "execute DUP...", 
 *   "actions": "Base64 encoded compute phase actions boc (OutList n)",
 *   "elapsed_time": 0.02
 * }
 */
EMULATOR_EXPORT const char *transaction_emulator_emulate_transaction(void *transaction_emulator, const char *shard_account_boc, const char *message_boc);

/**
 * @brief Emulate tick tock transaction
 * @param transaction_emulator Pointer to TransactionEmulator object
 * @param shard_account_boc Base64 encoded BoC serialized ShardAccount of special account
 * @param is_tock True for tock transactions, false for tick
 * @return Json object with error:
 * { 
 *   "success": false, 
 *   "error": "Error description",
 *   "external_not_accepted": false
 * } 
 * Or success:
 * { 
 *   "success": true, 
 *   "transaction": "Base64 encoded Transaction boc", 
 *   "shard_account": "Base64 encoded new ShardAccount boc", 
 *   "vm_log": "execute DUP...", 
 *   "actions": "Base64 encoded compute phase actions boc (OutList n)",
 *   "elapsed_time": 0.02
 * }
 */
EMULATOR_EXPORT const char *transaction_emulator_emulate_tick_tock_transaction(void *transaction_emulator, const char *shard_account_boc, bool is_tock);

/**
 * @brief Destroy TransactionEmulator object
 * @param transaction_emulator Pointer to TransactionEmulator object
 */
EMULATOR_EXPORT void transaction_emulator_destroy(void *transaction_emulator);

/**
 * @brief Set global verbosity level of the library
 * @param verbosity_level New verbosity level (0 - never, 1 - error, 2 - warning, 3 - info, 4 - debug)
 */
EMULATOR_EXPORT bool emulator_set_verbosity_level(int verbosity_level);

/**
 * @brief Create TVM emulator
 * @param code_boc Base64 encoded BoC serialized smart contract code cell
 * @param data_boc Base64 encoded BoC serialized smart contract data cell
 * @param vm_log_verbosity Verbosity level of VM log
 * @return Pointer to TVM emulator object
 */
EMULATOR_EXPORT void *tvm_emulator_create(const char *code_boc, const char *data_boc, int vm_log_verbosity);

/**
 * @brief Set libraries for TVM emulator
 * @param libs_boc Base64 encoded BoC serialized libraries dictionary (HashmapE 256 ^Cell).
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool tvm_emulator_set_libraries(void *tvm_emulator, const char *libs_boc);

/**
 * @brief Set c7 parameters
 * @param tvm_emulator Pointer to TVM emulator
 * @param address Adress of smart contract
 * @param unixtime Unix timestamp
 * @param balance Smart contract balance
 * @param rand_seed_hex Random seed as hex string of length 64
 * @param config Base64 encoded BoC serialized Config dictionary (Hashmap 32 ^Cell). Optional.
 * @return true in case of success, false in case of error 
 */
EMULATOR_EXPORT bool tvm_emulator_set_c7(void *tvm_emulator, const char *address, uint32_t unixtime, uint64_t balance, const char *rand_seed_hex, const char *config);

/**
 * @brief Set extra currencies balance
 * @param tvm_emulator Pointer to TVM emulator
 * @param extra_currencies String with extra currencies balance in format "currency_id1=balance1 currency_id2=balance2 ..."
 * @return true in case of success, false in case of error 
 */
EMULATOR_EXPORT bool tvm_emulator_set_extra_currencies(void *tvm_emulator, const char *extra_currencies);

/**
 * @brief Set config for TVM emulator
 * @param tvm_emulator Pointer to TVM emulator
 * @param config Pointer to Config object
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool tvm_emulator_set_config_object(void* tvm_emulator, void* config);

/**
 * @brief Set tuple of previous blocks (13th element of c7)
 * @param tvm_emulator Pointer to TVM emulator
 * @param info_boc Base64 encoded BoC serialized TVM tuple (VmStackValue).
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool tvm_emulator_set_prev_blocks_info(void *tvm_emulator, const char* info_boc);

/**
 * @brief Set TVM gas limit
 * @param tvm_emulator Pointer to TVM emulator
 * @param gas_limit Gas limit
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool tvm_emulator_set_gas_limit(void *tvm_emulator, int64_t gas_limit);

/**
 * @brief Enable or disable TVM debug primitives
 * @param tvm_emulator Pointer to TVM emulator
 * @param debug_enabled Whether debug primitives should be enabled or not
 * @return true in case of success, false in case of error
 */
EMULATOR_EXPORT bool tvm_emulator_set_debug_enabled(void *tvm_emulator, bool debug_enabled);

/**
 * @brief Run get method
 * @param tvm_emulator Pointer to TVM emulator
 * @param method_id Integer method id
 * @param stack_boc Base64 encoded BoC serialized stack (VmStack)
 * @return Json object with error:
 * { 
 *   "success": false, 
 *   "error": "Error description"
 * } 
 * Or success:
 * {
 *   "success": true
 *   "vm_log": "...", 
 *   "vm_exit_code": 0, 
 *   "stack": "Base64 encoded BoC serialized stack (VmStack)", 
 *   "missing_library": null, 
 *   "gas_used": 1212
 * }
 */
EMULATOR_EXPORT const char *tvm_emulator_run_get_method(void *tvm_emulator, int method_id, const char *stack_boc);

/**
 * @brief Optimized version of "run get method" with all passed parameters in a single call
 * @param len Length of params_boc buffer
 * @param params_boc BoC serialized parameters, scheme: request$_ code:^Cell data:^Cell stack:^VmStack params:^[c7:^VmStack libs:^Cell] method_id:(## 32)
 * @param gas_limit Gas limit
 * @return Char* with first 4 bytes defining length, and the rest BoC serialized result
 *         Scheme: result$_ exit_code:(## 32) gas_used:(## 32) stack:^VmStack
 */
EMULATOR_EXPORT const char *tvm_emulator_emulate_run_method(uint32_t len, const char *params_boc, int64_t gas_limit);

/**
 * @brief Send external message
 * @param tvm_emulator Pointer to TVM emulator
 * @param message_body_boc Base64 encoded BoC serialized message body cell.
 * @return Json object with error:
 * { 
 *   "success": false, 
 *   "error": "Error description"
 * } 
 * Or success:
 * {
 *   "success": true,
 *   "new_code": "Base64 boc decoded new code cell",
 *   "new_data": "Base64 boc decoded new data cell",
 *   "accepted": true,
 *   "vm_exit_code": 0, 
 *   "vm_log": "...", 
 *   "missing_library": null, 
 *   "gas_used": 1212,
 *   "actions": "Base64 boc decoded actions cell of type (OutList n)"
 * }
 */
EMULATOR_EXPORT const char *tvm_emulator_send_external_message(void *tvm_emulator, const char *message_body_boc);

/**
 * @brief Send internal message
 * @param tvm_emulator Pointer to TVM emulator
 * @param message_body_boc Base64 encoded BoC serialized message body cell.
 * @param amount Amount of nanograms attached with internal message.
 * @return Json object with error:
 * { 
 *   "success": false, 
 *   "error": "Error description"
 * } 
 * Or success:
 * {
 *   "success": true,
 *   "new_code": "Base64 boc decoded new code cell",
 *   "new_data": "Base64 boc decoded new data cell",
 *   "accepted": true,
 *   "vm_exit_code": 0, 
 *   "vm_log": "...", 
 *   "missing_library": null, 
 *   "gas_used": 1212,
 *   "actions": "Base64 boc decoded actions cell of type (OutList n)"
 * }
 */
EMULATOR_EXPORT const char *tvm_emulator_send_internal_message(void *tvm_emulator, const char *message_body_boc, uint64_t amount);

/**
 * @brief Destroy TVM emulator object
 * @param tvm_emulator Pointer to TVM emulator object
 */
EMULATOR_EXPORT void tvm_emulator_destroy(void *tvm_emulator);

/**
 * @brief Destroy Config object
 * @param tvm_emulator Pointer to Config object
 */
EMULATOR_EXPORT void emulator_config_destroy(void *config);

/**
 * @brief Get git commit hash and date of the library
 */
EMULATOR_EXPORT const char* emulator_version();

#ifdef __cplusplus
}  // extern "C"
#endif
