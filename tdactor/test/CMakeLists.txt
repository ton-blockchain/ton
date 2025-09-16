add_executable(test-coro test-coro.cpp)
target_link_libraries(test-coro tdactor tdutils)
target_include_directories(test-coro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../)
