if (NOT DEFINED OPENSSL_SSL_LIBRARY OR OPENSSL_SSL_LIBRARY STREQUAL "")
  message(FATAL_ERROR "OPENSSL_SSL_LIBRARY is not set; cannot verify QUIC symbols.")
endif()

if (NOT EXISTS "${OPENSSL_SSL_LIBRARY}")
  message(FATAL_ERROR "OpenSSL SSL library not found at ${OPENSSL_SSL_LIBRARY}.")
endif()

set(NM_EXECUTABLE "")
if (DEFINED OPENSSL_NM AND NOT OPENSSL_NM STREQUAL "")
  set(NM_EXECUTABLE "${OPENSSL_NM}")
else()
  find_program(NM_EXECUTABLE nm)
endif()

if (NM_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "nm not found; cannot verify QUIC symbols in ${OPENSSL_SSL_LIBRARY}.")
endif()

execute_process(
  COMMAND "${NM_EXECUTABLE}" -g "${OPENSSL_SSL_LIBRARY}"
  RESULT_VARIABLE NM_RESULT
  OUTPUT_VARIABLE NM_OUTPUT
  ERROR_VARIABLE NM_ERROR
)

if (NOT NM_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to run nm on ${OPENSSL_SSL_LIBRARY}: ${NM_ERROR}")
endif()

string(FIND "${NM_OUTPUT}" "SSL_provide_quic_data" HAVE_SSL_PROVIDE_QUIC_DATA)
string(FIND "${NM_OUTPUT}" "SSL_set_quic_tls_cbs" HAVE_SSL_SET_QUIC_TLS_CBS)

if (HAVE_SSL_PROVIDE_QUIC_DATA EQUAL -1 AND HAVE_SSL_SET_QUIC_TLS_CBS EQUAL -1)
  message(FATAL_ERROR "OpenSSL lacks QUIC symbols (SSL_provide_quic_data or SSL_set_quic_tls_cbs) in ${OPENSSL_SSL_LIBRARY}. Rebuild OpenSSL with enable-quic.")
endif()

if (DEFINED OPENSSL_QUIC_STAMP AND NOT OPENSSL_QUIC_STAMP STREQUAL "")
  file(WRITE "${OPENSSL_QUIC_STAMP}" "ok\n")
endif()
