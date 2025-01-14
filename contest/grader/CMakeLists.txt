cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

add_executable(contest-grader grader.cpp)
target_include_directories(contest-grader PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>)
target_link_libraries(contest-grader contest-solution tdutils tdactor ton_block ton_crypto tl-utils tl_api git terminal)
