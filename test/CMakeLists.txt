add_library(td-test-main test-td-main.cpp)
target_link_libraries(td-test-main PRIVATE tdutils)

add_subdirectory(tonlib)
