add_executable(test-overlay-manager test-overlay-manager.cpp)
target_link_libraries(test-overlay-manager PRIVATE
    overlay
    tdutils
    tdactor
    adnl
    adnltest
    tl_api
    dht
    td-test-main
)
ton_test(test-overlay-manager)
