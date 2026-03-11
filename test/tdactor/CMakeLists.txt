add_executable(test-scheduler-stop test-scheduler-stop.cpp)

target_link_libraries(test-scheduler-stop PRIVATE
    tdutils
    tdactor
    td-test-main
)

ton_test(test-scheduler-stop)

add_executable(test-runtime test-runtime.cpp)

target_link_libraries(test-runtime PRIVATE
    tdutils
    tdactor
    td-test-main
)

ton_test(test-runtime)
