
set(LITESERVER_DAEMON_SOURCE
        lite-server-daemon.cpp lite-server-config.hpp)

add_executable(lite-server ${LITESERVER_DAEMON_SOURCE})

target_link_libraries(lite-server overlay tdutils tdactor adnl tl_api dht
        rldp rldp2 catchain validatorsession full-node validator-disk validator ton_validator validator
        fift-lib memprof git ${JEMALLOC_LIBRARIES})

install(TARGETS lite-server RUNTIME DESTINATION bin)