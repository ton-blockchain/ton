add_executable(benchmark-fec benchmark.cpp )
target_include_directories(benchmark-fec PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(benchmark-fec PRIVATE tdfec)
