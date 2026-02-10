add_executable(test-runtime test-runtime.cpp)

target_link_libraries(test-runtime PRIVATE
    validator
    tdutils
    tdactor
    td-test-main
)

ton_test(test-runtime)
