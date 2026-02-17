#pragma once

#include <memory>
#include <type_traits>

#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/StringBuilder.h"
#include "td/utils/crypto.h"

// Helper macros to reduce boilerplate around OpenSSL calls.
// They convert OpenSSL failures into td::Status using create_openssl_error
// and automatically attach source file/line information.

// Check that an expression returns non-null (for pointer returning functions)
// On failure returns from the current function with a detailed error.
#define OPENSSL_CHECK_PTR(expr, message)                                                          \
  do {                                                                                            \
    auto *_openssl_tmp_ptr = (expr);                                                              \
    if ((_openssl_tmp_ptr) == nullptr) {                                                          \
      return ::td::create_openssl_error(                                                          \
          -1, PSTRING() << (message) << " [" << #expr << "] at " << __FILE__ << ":" << __LINE__); \
    }                                                                                             \
  } while (0)

// Check that an expression returns 1 (common convention for OpenSSL success).
// On failure returns from the current function with a detailed error.
#define OPENSSL_CHECK_OK(expr, message)                                                           \
  do {                                                                                            \
    if ((expr) <= 0) {                                                                            \
      return ::td::create_openssl_error(                                                          \
          -1, PSTRING() << (message) << " [" << #expr << "] at " << __FILE__ << ":" << __LINE__); \
    }                                                                                             \
  } while (0)

// Generic RAII wrapper for OpenSSL pointers. The Deleter function is supplied
// as a non-type template parameter.
namespace openssl_detail {
template <class T, auto *FreeFn>
struct DeleterWrapper {
  void operator()(T *ptr) const {
    FreeFn(ptr);
  }
};
}  // namespace openssl_detail

template <class T, auto *FreeFn>
using openssl_ptr = std::unique_ptr<T, openssl_detail::DeleterWrapper<T, FreeFn>>;

// Allocate an OpenSSL object, check for nullptr, wrap into openssl_ptr.
// Example:
//   OPENSSL_MAKE_PTR(dctx,
//                    EVP_PKEY_CTX_new(pkey, nullptr),
//                    EVP_PKEY_CTX_free,
//                    "EVP_PKEY_CTX_new failed");
// After the macro, `dctx` is an openssl_ptr and will be freed automatically.
#define OPENSSL_MAKE_PTR(var, expr, free_fn, message) \
  auto *var##_raw = (expr);                           \
  OPENSSL_CHECK_PTR(var##_raw, message);              \
  openssl_ptr<std::remove_pointer_t<decltype(var##_raw)>, &free_fn> var{var##_raw};
