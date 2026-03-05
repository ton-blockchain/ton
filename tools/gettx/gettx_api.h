#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for transaction lookup context
typedef void* gettx_handle_t;

// Result structure for transaction lookup
typedef struct {
  char* json_data;  // JSON string (must be freed by caller)
  int error_code;   // 0 on success, non-zero on error
  char* error_msg;  // Error message (must be freed by caller if error_code != 0)
} gettx_result_t;

// Create a new transaction lookup context
// Returns handle on success, NULL on failure
gettx_handle_t gettx_create(const char* db_path, int include_deleted);

// Lookup transactions
// Returns result structure that must be freed with gettx_free_result
gettx_result_t gettx_lookup(gettx_handle_t handle,
                           int workchain,
                           const char* address,
                           unsigned long long logical_time,
                           const char* hash,
                           unsigned count);

// Free a result structure
void gettx_free_result(gettx_result_t* result);

// Destroy a transaction lookup context
void gettx_destroy(gettx_handle_t handle);

#ifdef __cplusplus
}
#endif
