execute_process(
  COMMAND "${FUNC_EXECUTABLE}" "${SOURCE_FILE}"
  RESULT_VARIABLE func_result
  OUTPUT_VARIABLE func_output
  ERROR_VARIABLE func_error)

if (func_result EQUAL 0)
  message(FATAL_ERROR "FunC accepted a tensor with more than 254 components")
endif()

set(func_diagnostics "${func_output}${func_error}")
if (func_diagnostics MATCHES "Assertion failed")
  message(FATAL_ERROR "FunC aborted instead of reporting a compilation error:\n${func_diagnostics}")
endif()

if (NOT func_diagnostics MATCHES "\\.fc:[0-9]+:[0-9]+: error: tensor cannot contain more than 254 components")
  message(FATAL_ERROR "FunC did not report the oversized tensor diagnostic:\n${func_diagnostics}")
endif()
