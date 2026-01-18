add_executable(test-consensus test-consensus.cpp)
target_link_libraries(test-consensus overlay tdutils tdactor adnl adnltest rldp tl_api dht validator ton_validator validator)
# add_test(test-consensus test-consensus)
