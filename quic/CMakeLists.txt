add_library(quic STATIC quic-sender.cpp quic-server.cpp quic-pimpl.cpp)
target_include_directories(quic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/..
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(quic PUBLIC tl_api tdutils tdactor metrics ngtcp2_static ngtcp2_crypto_ossl_static ${OPENSSL_LIBRARIES} ton_crypto_core)
add_dependencies(quic OpenSSL)

# FIXME: enable or delete
# add_executable(quic-example-client quic-example-client.cpp)
# target_include_directories(quic-example-client PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
# target_link_libraries(quic-example-client PUBLIC quic)

# add_executable(quic-example-server quic-example-server.cpp)
# target_include_directories(quic-example-server PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
# target_link_libraries(quic-example-server PUBLIC quic)
