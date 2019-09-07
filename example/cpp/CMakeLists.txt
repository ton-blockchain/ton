cmake_minimum_required(VERSION 3.1 FATAL_ERROR)

project(TonExample VERSION 1.0 LANGUAGES CXX)

find_package(Tonlib 0.5 REQUIRED)

add_executable(tonjson_example tonjson_example.cpp)
target_link_libraries(tonjson_example PRIVATE Tonlib::TonlibJson)
set_property(TARGET tonjson_example PROPERTY CXX_STANDARD 11)

add_executable(tonlib_example tonlib_example.cpp)

target_link_libraries(tonlib_example PRIVATE Tonlib::Tonlib)
set_property(TARGET tonlib_example PROPERTY CXX_STANDARD 14)
