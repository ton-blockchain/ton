add_executable(test-scheduler-stop test-scheduler-stop.cpp)

target_link_libraries(test-scheduler-stop PRIVATE
    tdutils
    tdactor
    td-test-main
)

ton_test(test-scheduler-stop)
