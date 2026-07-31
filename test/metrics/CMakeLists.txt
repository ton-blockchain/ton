add_executable(test-metrics test-metrics.cpp)

# adnl / rldp2 / quic: the golden family-name tests render those subsystems' own metric structs.
target_link_libraries(test-metrics PRIVATE
    metrics
    adnl
    rldp2
    tl_api
    tl_lite_api
    tl-utils
    td-test-main
)

# quic only exists with -DUSE_QUIC=ON; without it the rest of the golden tests still build and run.
if (TARGET quic)
  target_link_libraries(test-metrics PRIVATE quic)
  target_compile_definitions(test-metrics PRIVATE TON_TEST_METRICS_QUIC)
endif()

ton_test(test-metrics)
