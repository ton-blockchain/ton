set(CREATE_HARDFORK_SOURCE
  create-hardfork.cpp
)

add_executable(create-hardfork ${CREATE_HARDFORK_SOURCE})
target_link_libraries(create-hardfork overlay tdutils tdactor adnl tl_api dht rldp catchain validatorsession full-node validator-hardfork ton_validator
  validator-hardfork fift-lib memprof git ${JEMALLOC_LIBRARIES})

install(TARGETS create-hardfork RUNTIME DESTINATION bin)
