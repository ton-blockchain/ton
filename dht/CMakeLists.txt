cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

if (NOT OPENSSL_FOUND)
  find_package(OpenSSL REQUIRED)
endif()

set(DHT_SOURCE
  dht.cpp
  dht-remote-node.cpp
  dht-bucket.cpp
  dht-node.cpp
  dht-query.cpp
  dht-types.cpp

  dht-bucket.hpp
  dht-in.hpp
  dht-query.hpp
  dht-remote-node.hpp
  dht-types.h
  dht.h
  dht.hpp
)

add_library(dht STATIC ${DHT_SOURCE})

target_include_directories(dht PUBLIC 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}> 
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/.. 
  ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(dht PRIVATE tdutils tdactor adnl tl_api)

