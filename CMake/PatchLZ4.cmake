if(NOT DEFINED LZ4_CMAKE_FILE)
  message(FATAL_ERROR "LZ4_CMAKE_FILE must be defined")
endif()

if(NOT EXISTS "${LZ4_CMAKE_FILE}")
  message(FATAL_ERROR "LZ4 CMakeLists.txt not found: ${LZ4_CMAKE_FILE}")
endif()

file(READ "${LZ4_CMAKE_FILE}" LZ4_CMAKE_CONTENTS)

if(LZ4_CMAKE_CONTENTS MATCHES "cmake_minimum_required\\(VERSION 3\\.5\\)")
  message(STATUS "LZ4 cmake_minimum_required already set to 3.5")
  return()
endif()

string(REGEX REPLACE
  "cmake_minimum_required\\(VERSION [0-9\\.]+\\)"
  "cmake_minimum_required(VERSION 3.5)"
  LZ4_CMAKE_CONTENTS_UPDATED
  "${LZ4_CMAKE_CONTENTS}"
)

if(LZ4_CMAKE_CONTENTS STREQUAL LZ4_CMAKE_CONTENTS_UPDATED)
  message(FATAL_ERROR "Failed to update cmake_minimum_required in ${LZ4_CMAKE_FILE}")
endif()

file(WRITE "${LZ4_CMAKE_FILE}" "${LZ4_CMAKE_CONTENTS_UPDATED}")
message(STATUS "Updated LZ4 cmake_minimum_required to 3.5")
