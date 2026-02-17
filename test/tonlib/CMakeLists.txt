set(TONLIB_TESTS
    test-ffi-awaitable
)

# FIXME: Enable this on MacOS once we figure out why it fails in CI because of too slow wake ups.
if(NOT APPLE)
    list(APPEND TONLIB_TESTS test-ffi-event-loop)
endif()

foreach(TEST_NAME IN LISTS TONLIB_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cpp)

    target_link_libraries(${TEST_NAME} PRIVATE td-test-main tonlibjson_objects tonlibjson_private_interface)

    ton_test(${TEST_NAME})
endforeach()
