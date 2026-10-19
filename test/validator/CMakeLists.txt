add_subdirectory(consensus)

add_executable(test-ext-message-expiry test-ext-message-expiry.cpp)
target_include_directories(test-ext-message-expiry PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-ext-message-expiry PRIVATE
    adnl
    dht
    metrics
    overlay
    tdactor
    tdutils
    td-test-main
    tl_api
    ton_validator
    validator
)
ton_test(test-ext-message-expiry)

add_executable(test-global-balance-calculator test-global-balance-calculator.cpp)
target_include_directories(test-global-balance-calculator PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-global-balance-calculator PRIVATE
    adnl
    dht
    metrics
    overlay
    tdactor
    tdutils
    td-test-main
    tl_api
    ton_validator
    validator
)
ton_test(test-global-balance-calculator)
