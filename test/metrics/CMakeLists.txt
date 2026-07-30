add_executable(test-metrics test-metrics.cpp)

# adnl / rldp2 / quic: the golden family-name tests render those subsystems' own metric structs.
target_link_libraries(test-metrics PRIVATE
    metrics
    adnl
    rldp2
    quic
    tl_api
    tl-utils
    td-test-main
)

ton_test(test-metrics)
