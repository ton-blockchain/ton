cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()


set(CREATE_HARDFORK_SOURCE
  create-hardfork.cpp
)

add_executable(create-hardfork ${CREATE_HARDFORK_SOURCE})
target_link_libraries(create-hardfork overlay tdutils tdactor adnl tl_api dht
  rldp catchain validatorsession full-node validator-hardfork ton_validator
  validator-hardfork fift-lib memprof ${JEMALLOC_LIBRARIES})

install(TARGETS create-hardfork RUNTIME DESTINATION bin)
