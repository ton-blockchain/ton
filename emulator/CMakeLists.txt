cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(EMULATOR_STATIC_SOURCE
  transaction-emulator.cpp
)

set(EMULATOR_HEADERS 
  transaction-emulator.h
  emulator-extern.h
)

set(EMULATOR_SOURCE
  emulator-extern.cpp
)

include(GenerateExportHeader)

add_library(emulator_static STATIC ${EMULATOR_STATIC_SOURCE})
target_link_libraries(emulator_static PUBLIC ton_validator ton_crypto ton_block)

add_library(emulator SHARED ${EMULATOR_SOURCE} ${EMULATOR_HEADERS})
target_link_libraries(emulator PUBLIC emulator_static)
generate_export_header(emulator EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/emulator_export.h)
target_include_directories(emulator PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
if (APPLE)
  set_target_properties(emulator PROPERTIES LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/emulator_export_list")
endif()

