add_executable(test-consensus test-consensus.cpp)
target_link_libraries(test-consensus PRIVATE
    adnl
    adnltest
    dht
    overlay
    tdactor
    tdutils
    tl_api
    ton_validator
    validator
)
