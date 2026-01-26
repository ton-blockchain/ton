if(NOT DEFINED PATCH_FILE)
  message(FATAL_ERROR "PATCH_FILE must be defined")
endif()

if(NOT DEFINED WORKING_DIR)
  message(FATAL_ERROR "WORKING_DIR must be defined")
endif()

if(NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
endif()

if(NOT EXISTS "${WORKING_DIR}")
  message(FATAL_ERROR "Working directory does not exist: ${WORKING_DIR}")
endif()

find_program(GIT_EXECUTABLE git)
if(NOT GIT_EXECUTABLE)
  message(FATAL_ERROR "git executable not found; cannot apply ${PATCH_FILE}")
endif()

set(PATCH_FILE_PATH "${PATCH_FILE}")
set(WORKING_DIR_PATH "${WORKING_DIR}")
if(MINGW OR MSYS)
  find_program(CYGPATH_EXECUTABLE cygpath)
  if(CYGPATH_EXECUTABLE)
    execute_process(
      COMMAND ${CYGPATH_EXECUTABLE} -u "${PATCH_FILE}"
      OUTPUT_VARIABLE PATCH_FILE_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
      COMMAND ${CYGPATH_EXECUTABLE} -u "${WORKING_DIR}"
      OUTPUT_VARIABLE WORKING_DIR_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
endif()

execute_process(
  COMMAND ${GIT_EXECUTABLE} -C "${WORKING_DIR_PATH}" apply --reverse --check "${PATCH_FILE_PATH}"
  RESULT_VARIABLE PATCH_REVERSE_CHECK_RESULT
  OUTPUT_QUIET
  ERROR_QUIET
)

if(PATCH_REVERSE_CHECK_RESULT EQUAL 0)
  message(STATUS "Patch already applied: ${PATCH_FILE}")
  return()
endif()

execute_process(
  COMMAND ${GIT_EXECUTABLE} -C "${WORKING_DIR_PATH}" apply --check "${PATCH_FILE_PATH}"
  RESULT_VARIABLE PATCH_CHECK_RESULT
)

if(NOT PATCH_CHECK_RESULT EQUAL 0)
  message(FATAL_ERROR "Patch check failed: ${PATCH_FILE}")
endif()

execute_process(
  COMMAND ${GIT_EXECUTABLE} -C "${WORKING_DIR_PATH}" apply "${PATCH_FILE_PATH}"
  RESULT_VARIABLE PATCH_APPLY_RESULT
)

if(NOT PATCH_APPLY_RESULT EQUAL 0)
  message(FATAL_ERROR "Patch apply failed: ${PATCH_FILE}")
endif()

message(STATUS "Applied patch: ${PATCH_FILE}")
