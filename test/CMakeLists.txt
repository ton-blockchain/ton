add_library(td-test-main test-td-main.cpp)
target_link_libraries(td-test-main PRIVATE tdutils)

add_subdirectory(tdactor)
add_subdirectory(tonlib)
add_subdirectory(consensus)
