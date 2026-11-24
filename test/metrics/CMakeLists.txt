add_executable(test-metrics test-metrics.cpp)

# ADNL and QUIC golden family-name tests render those subsystems' own metric structs.
target_link_libraries(test-metrics PRIVATE
    metrics
    adnl
    quic
    tl_api
    tl_lite_api
    tl-utils
    td-test-main
)

target_compile_definitions(test-metrics PRIVATE TON_TEST_METRICS_QUIC)

ton_test(test-metrics)
