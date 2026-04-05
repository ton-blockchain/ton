add_executable(test-persistent-treap test-persistent-treap.cpp)

target_link_libraries(test-persistent-treap PRIVATE
    tdutils
    td-test-main
)

ton_test(test-persistent-treap)
