PROJECT(wingetopt)
cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

add_definitions(-D_CRT_SECURE_NO_WARNINGS)
add_library(wingetopt src/getopt.c src/getopt.h)
target_include_directories(wingetopt PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)