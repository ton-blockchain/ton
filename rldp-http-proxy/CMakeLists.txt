cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

add_executable(rldp-http-proxy rldp-http-proxy.cpp DNSResolver.h TonlibClient.h TonlibClient.cpp DNSResolver.cpp)
target_include_directories(rldp-http-proxy PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(rldp-http-proxy PRIVATE tonhttp rldp dht tonlib git)
