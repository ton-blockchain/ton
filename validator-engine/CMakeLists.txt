cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(VALIDATOR_ENGINE_SOURCE
  validator-engine.hpp
  validator-engine.cpp
)

add_executable(validator-engine ${VALIDATOR_ENGINE_SOURCE})
add_dependencies(validator-engine openssl)

target_link_libraries(validator-engine overlay tdutils tdactor adnl tl_api dht
  rldp rldp2 catchain validatorsession full-node validator ton_validator validator
  fift-lib memprof git ${JEMALLOC_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARY})

install(TARGETS validator-engine RUNTIME DESTINATION bin)
