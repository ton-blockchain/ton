# Standalone getTransactions tool for TON validator databases

add_executable(gettx
  main.cpp
  db-index.cpp
  package-reader.cpp
  transaction-lookup.cpp
  shard-iterator.cpp
  block-tx-extractor.cpp
)

target_include_directories(gettx
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/validator
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_SOURCE_DIR}/crypto/vm
    ${CMAKE_SOURCE_DIR}/crypto/block
    ${CMAKE_SOURCE_DIR}/crypto/tl
    ${CMAKE_SOURCE_DIR}/tdutils
    ${CMAKE_SOURCE_DIR}/tdutils/tl
    ${CMAKE_SOURCE_DIR}/tdutils/db
)

target_link_libraries(gettx
  PRIVATE
    tdutils
    tddb
    ton_crypto
    tl_api
    tl-utils
    ton_db
    ton_block
    validator-db
)

target_compile_features(gettx PRIVATE cxx_std_20)

# Shared library for Python bindings
add_library(gettx_shared SHARED
  gettx_api.cpp
  db-index.cpp
  package-reader.cpp
  transaction-lookup.cpp
  shard-iterator.cpp
  block-tx-extractor.cpp
  ${CMAKE_SOURCE_DIR}/tl/generate/auto/tl/lite_api.cpp
)

target_include_directories(gettx_shared
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/validator
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_SOURCE_DIR}/crypto/vm
    ${CMAKE_SOURCE_DIR}/crypto/block
    ${CMAKE_SOURCE_DIR}/crypto/tl
    ${CMAKE_SOURCE_DIR}/tl
    ${CMAKE_SOURCE_DIR}/tl/generate
    ${CMAKE_SOURCE_DIR}/tdutils
    ${CMAKE_SOURCE_DIR}/tdutils/tl
    ${CMAKE_SOURCE_DIR}/tdutils/db
)

# Link libraries - use validator (superset of validator-db) to avoid duplicate symbols
target_link_libraries(gettx_shared
  PRIVATE
    tdutils
    tddb
    ton_crypto
    tl_api
    tl-utils
    ton_db
    ton_block
    validator
)

target_compile_features(gettx_shared PRIVATE cxx_std_20)

# Set visibility to export symbols
target_compile_options(gettx_shared PRIVATE -fPIC)
target_compile_definitions(gettx_shared PRIVATE GETTX_EXPORTS)

# Set output name for the shared library
set_target_properties(gettx_shared PROPERTIES
  OUTPUT_NAME "gettx"
  VERSION 1.0.0
  SOVERSION 1
  POSITION_INDEPENDENT_CODE ON
)
