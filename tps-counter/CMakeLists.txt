cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

add_executable(tps-counter tps-counter.cpp)
target_include_directories(tps-counter PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(tps-counter PRIVATE tonhttp rldp dht lite-client-common git terminal overlay)
