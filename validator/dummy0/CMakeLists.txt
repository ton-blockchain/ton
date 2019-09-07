cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(DUMMY_VALIDATOR_SOURCE
  accept-block.cpp
  fake-accept-block.cpp
  check-proof.cpp
  collate-query.cpp
  fabric.cpp
  shard.cpp
  signature-set.cpp
  top-shard-description.cpp
  validate-query.cpp
  validator-set.cpp

  check-proof.hpp
  collate-query.hpp
  external-message.hpp
  proof.hpp
  shard.hpp
  signature-set.hpp
  top-shard-description.hpp
  validate-query.hpp
  validator-set.hpp
)

add_library(dummy_validator STATIC ${DUMMY_VALIDATOR_SOURCE})

target_include_directories(dummy_validator PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/../crypto 
  ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(dummy_validator PRIVATE tdutils tdactor adnl tl_api dht tdfec
  overlay catchain validatorsession ton_crypto ton_block)
