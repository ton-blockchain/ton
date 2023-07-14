cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(CREATE_HARDFORK_SOURCE
  create-hardfork.cpp
)

add_executable(create-hardfork ${CREATE_HARDFORK_SOURCE})
add_dependencies(create-hardfork openssl)

target_link_libraries(create-hardfork overlay tdutils tdactor adnl tl_api dht
  rldp catchain validatorsession full-node validator-hardfork ton_validator
  validator-hardfork fift-lib memprof git ${JEMALLOC_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARY})

install(TARGETS create-hardfork RUNTIME DESTINATION bin)
