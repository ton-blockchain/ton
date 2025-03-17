if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()


set (DHT_SERVER_SOURCE
  dht-server.cpp
  dht-server.hpp
)

add_executable (dht-server ${DHT_SERVER_SOURCE})
target_link_libraries(dht-server tdutils tdactor adnl tl_api dht memprof git ${JEMALLOC_LIBRARIES})

install(TARGETS dht-server RUNTIME DESTINATION bin)
