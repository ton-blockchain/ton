add_executable(test-metrics test-metrics.cpp)

target_link_libraries(test-metrics PRIVATE
    metrics
    td-test-main
)

ton_test(test-metrics)
