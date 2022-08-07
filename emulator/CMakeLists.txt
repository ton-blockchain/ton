cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(EMULATOR_SOURCE
  transaction-emulator.cpp
)

set(EMULATOR_HEADERS 
  transaction-emulator.h
)

add_library(emulator SHARED ${EMULATOR_SOURCE} ${EMULATOR_HEADERS})
target_link_libraries(emulator PRIVATE validator ton_crypto ton_block)
