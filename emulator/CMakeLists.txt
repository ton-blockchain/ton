cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(EMULATOR_SOURCE
  transaction-emulator.cpp
)

add_library(emulator SHARED ${EMULATOR_SOURCE})
target_link_libraries(emulator PRIVATE validator ton_crypto ton_block)

