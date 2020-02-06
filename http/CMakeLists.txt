cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

set(HTTP_SOURCE
  http.h
  http.cpp
  http-connection.h
  http-connection.cpp
  http-inbound-connection.h
  http-inbound-connection.cpp
  http-outbound-connection.h
  http-outbound-connection.cpp
  http-server.h
  http-server.cpp
  http-client.h
  http-client.hpp
  http-client.cpp
)

add_library(tonhttp STATIC ${HTTP_SOURCE})
target_include_directories(tonhttp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(tonhttp PUBLIC tdactor ton_crypto tl_api tdnet )

add_executable(http-proxy http-proxy.cpp)
target_include_directories(http-proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(http-proxy PRIVATE tonhttp)
