cmake_minimum_required(VERSION 3.16)
project(TonlibConsumerTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Tonlib REQUIRED)

add_executable(tonlibjson_consumer tonlibjson_consumer.cpp)
target_link_libraries(tonlibjson_consumer PRIVATE Tonlib::TonlibJson)
