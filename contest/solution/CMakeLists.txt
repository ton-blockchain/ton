cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

add_library(contest-solution STATIC solution.hpp solution.cpp contest-validate-query.cpp contest-validate-query.hpp)
target_include_directories(contest-solution PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>)
target_include_directories(contest-solution PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../validator>)
target_link_libraries(contest-solution PRIVATE tdactor tdutils ton_block ton_validator validator ton_crypto)