cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()


set(VALIDATOR_ENGINE_SOURCE
  validator-engine.hpp
  validator-engine.cpp
)

add_executable(validator-engine ${VALIDATOR_ENGINE_SOURCE})
target_link_libraries(validator-engine overlay tdutils tdactor adnl tl_api dht
  rldp catchain validatorsession full-node validator ton_validator validator
  fift-lib memprof ${JEMALLOC_LIBRARIES})

install(TARGETS validator-engine RUNTIME DESTINATION bin)
