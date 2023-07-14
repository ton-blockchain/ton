cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set (DHT_SERVER_SOURCE
  dht-server.cpp
  dht-server.hpp
)

add_executable (dht-server ${DHT_SERVER_SOURCE})
target_link_libraries(dht-server tdutils tdactor adnl tl_api dht memprof git ${JEMALLOC_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARY})

install(TARGETS dht-server RUNTIME DESTINATION bin)
